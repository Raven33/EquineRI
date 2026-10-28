# Per-frame upload and readback rings (H1), 2026-10-16

Item H1 of `docs/reviews/Sandboxes-2026-10-15.md`: the first helper both sandboxes need. A
helper, not core (P41): built only on RI's public functions, replaceable.

## What the sandboxes do today

- **DX12 sandbox, upload:** `UploadBuffer<T>` and `StagingBuffer` hold one copy per frame in
  flight, in permanently mapped upload memory, 256-byte aligned, indexed by the frame. Each pass
  binds a root constant buffer view at `getGpuAddress(frameIndex)`.
- **DX12 sandbox, readback:** `initiateReadback(ctx, offset, size)` returns a ticket;
  `isReadbackComplete(ticket)` polls; `mapReadback(ticket)` gives the bytes. One readback in
  flight at a time.
- **Vulkan sandbox, readback:** reads its allocator state back every frame and waits on a fence
  each time.

RI has no root constant buffer views (P94 (8)): a constant buffer is a heap slot holding a
`ConstantBufferView`, and the slot's index goes in root data. So the upload ring hands out heap
slots per frame too.

## Proposal

Both rings follow a `FrameRing` (`RI/Helpers/FrameRing.hpp`). `beginFrame` already waits until
the frame's command list is free, so the ring can reuse that frame's region with no wait of its
own.

```cpp
// RI/Helpers/Rings.hpp

struct UploadRingDesc {
    uint64 bytesPerFrame = 4 << 20;
    MemoryKind memory = MemoryKind::eUpload;     // or eGPUUpload when the device has it
    DescriptorHeap *heap = nullptr;              // nullptr: no constant views
    uint32 firstSlot = 0;                        // slotsPerFrame * framesInFlight slots from here
    uint32 slotsPerFrame = 0;
};

struct UploadSpace {
    std::byte *cpu = nullptr;     // write here this frame
    const Buffer *buffer = nullptr;
    uint64 offset = 0;            // in *buffer: for cmdCopyBuffer, or a raw load in a shader
};

UploadRing createUploadRing(Device &device, FrameRing &frames, const UploadRingDesc &desc);
void destroyUploadRing(UploadRing &ring);

// Space in this frame's region, valid until the frame comes round again.
UploadSpace allocateUpload(UploadRing &ring, uint64 size, uint64 alignment = 16);
// Copies `data` into this frame's region, writes a ConstantBufferView over it in this frame's
// next slot, and returns the slot: what replaces a root constant buffer view.
uint32 uploadConstants(UploadRing &ring, std::span<const std::byte> data);

struct ReadbackTicket {
    uint64 frame = 0;
    uint64 offset = 0;
    uint64 size = 0;
};

ReadbackRing createReadbackRing(Device &device, FrameRing &frames, uint64 bytesPerFrame);
void destroyReadbackRing(ReadbackRing &ring);

// Records a copy of src's range into this frame's region.
ReadbackTicket requestReadback(ReadbackRing &ring, CommandList &cmd, const Buffer &src, uint64 offset,
                               uint64 size);
// The bytes once the ticket's frame has finished; empty before. Valid until the end of the
// frame framesInFlight frames after the ticket's: there is always one frame to read them in.
std::span<const std::byte> readbackData(ReadbackRing &ring, const ReadbackTicket &ticket);
```

Typed forms (`uploadConstants(ring, value)`) go next to the other typed helpers (P107).

### Choices made in the sketch

1. **The rings follow a `FrameRing`.** They read its frame number, so nobody has to call "begin
   frame" on each ring. `FrameRing` gains a frame counter (it has only the index today).
2. **Constants get a slot each.** `uploadConstants` writes one `ConstantBufferView` per call,
   into a fixed slot range per frame (`firstSlot + frameIndex * slotsPerFrame`): the range comes
   back with the region, so no slot allocator is involved. `allocateUpload` is the plain form,
   for copies and for shaders that load from one view over the region with an offset.
3. **Fixed capacity.** Running out of bytes or slots in a frame is a programmer error; the ring
   keeps the highest use per frame so the sizes can be set from a run. Growing would allocate
   in the middle of a frame.
4. **Readback has one region more than frames in flight.** With N regions, frame n's region is
   reused at frame n + N, which is also the first frame where its data is certain to be done:
   there would be no frame to read it in. With N + 1 regions, frame n's data is readable for
   certain during frame n + N, with no wait.

## Questions for the user

1. Should the rings follow a `FrameRing` (sketch), or take the frame index from the caller?
2. Constants: a slot per `uploadConstants` call (sketch), only offsets into one view over the
   region, or both?
3. Fixed capacity with an error (sketch), or growth?
4. Readback lifetime: N + 1 regions, so frame n reads at n + N (sketch), or copy the bytes out
   to a CPU vector when the frame finishes?

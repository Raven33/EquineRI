---
symbols: [RI::cmdSetConstantBuffer, RI::kMaxConstantBuffers, RI::kConstantBufferRootCost, RI::kConstantBufferAlignment]
---

# RI::cmdSetConstantBuffer

## What it does

`RI::cmdSetConstantBuffer(cmd, slot, buffer, offset)` binds a range of a buffer as a
**bindful constant buffer**: the block the shader declares at that slot with
`EQ_CONSTANT_BUFFER` ([P112](../design-notes/decisions.md#p112)). Nothing is created in
advance: no descriptor, no heap slot. The bind is recorded at once.

Use it for blocks that change per pass or per draw, such as a camera or a pass description.
Root data stays the place for a few values and heap slots
([`cmdSetRootData`](cmdSetRootData.md)). A heap-indexed `ConstantBuffer<T>` stays possible too.

In the shader, after `#include <equine/root.hlsli>`:

```hlsl
struct Pass {
    float4x4 viewProjection;
    float4 tint;
};
EQ_CONSTANT_BUFFER(Pass, pass, 0); // slot 0 to 7; the slot may be a macro
```

In C++:

```cpp
RI::cmdSetPipeline(cmd, pipeline);              // pipeline.constantBuffers has bit 0 set
RI::cmdSetConstantBuffer(cmd, 0, passes, 256);  // the block at byte 256 of `passes`
RI::cmdDraw(cmd, 3);
```

## Signature

--8<-- "generated/signatures/RI.cmdSetConstantBuffer.md"

## How RI does it

- **Vulkan**: the slots are uniform buffers at set 2, filled with push descriptors
  (`vkCmdPushDescriptorSet`, core in Vulkan 1.4, else `VK_KHR_push_descriptor`).
- **D3D12**: the slots are `register(b<slot>, space998)`, and RI adds one root CBV per slot to
  the root signature, after the root constants.

On RADV, a pushed constant buffer reads as fast as a heap-indexed one. A bind costs about
120 ns more on the CPU than writing a heap index (`docs/measurements/constant_buffers/`).

## Rules and errors

- Call it after [`cmdSetPipeline`](cmdSetPipeline.md) or `cmdSetWorkGraph`. A pipeline change
  **forgets every slot**: set them again. In validation builds, a draw, dispatch or graph
  dispatch with a declared slot left unset is a **programmer error**
  (`cmdDispatch: constant buffer slot 0 is not set ...`).
- The slot must be one the pipeline declares (`pipeline.constantBuffers`), the buffer needs
  `BufferUsage::eConstant`, and the offset must be a multiple of `kConstantBufferAlignment`
  (256, D3D12's rule, on both backends). The block, its size rounded up to 16 bytes, must fit
  in the buffer from that offset. Anything else is a **programmer error**, in every build.
- Each declared slot takes `kConstantBufferRootCost` (8) bytes of the root budget, on both
  backends. So root data plus 8 bytes per slot must fit in `DeviceLimits::maxRootDataSize`.
  Otherwise pipeline creation is **unsupported**.
- Every stage that declares a slot must declare the same block size.
- Space 998 (D3D12) and set 2 (Vulkan) belong to `EQ_CONSTANT_BUFFER`. Anything else there,
  or a slot past 7, is refused at pipeline creation.
- Work graphs: RI does not read libraries, so give the slots in `WorkGraphDesc::constantBuffers`.
  Their sizes are unknown, so binds are not checked against them.
- After a native section, `endNative` binds the slots again.

## Related decisions

- [P112](../design-notes/decisions.md#p112): bindful constant buffers.
- [P45](../design-notes/decisions.md#p45): root data and its reserved space.
- [P6](../design-notes/decisions.md#p6): bindless first; P112 adds this bindful exception.

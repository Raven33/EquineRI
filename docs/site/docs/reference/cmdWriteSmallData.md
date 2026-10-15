---
symbols: [RI::cmdWriteSmallData, RI::kMaxSmallWriteSize]
---

# RI::cmdWriteSmallData

## What it does

`RI::cmdWriteSmallData(cmd, buffer, offset, data)` writes **a few values** into a buffer, in
order with the commands around it: a counter reset at the start of a frame, a flag, a
handful of indices. The bytes are recorded **into the command list itself**, so no upload
buffer or shader is involved ([P107](../design-notes/decisions.md#p107)).

It is for small data only: at most `kMaxSmallWriteSize` (64) bytes per call. Larger data goes
through an upload buffer and [`cmdCopyBuffer`](cmdCopyBuffer.md).

Core takes bytes (`std::span<const std::byte>`). The typed form,
[`RI::Helpers::cmdWriteSmallData(cmd, buffer, offset, value)`](../helpers/typed-commands.md),
takes any value of 4 to 64 bytes and checks its size **at compile time**.

## Signature

--8<-- "generated/signatures/RI.cmdWriteSmallData.md"

## Example

```cpp
--8<-- "snippets/ri/copies.cpp:small-write"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/copies.cpp:small-write"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/copies.cpp:small-write"
    ```

    What to watch for: `vkCmdUpdateBuffer` is a transfer command, outside render passes, up
    to 65,536 bytes. The data is copied into the command buffer when it is recorded.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/copies.cpp:small-write"
    ```

    What to watch for: `WriteBufferImmediate` takes one parameter per 32-bit value, each with
    its own GPU address, so the command list grows with every value. It is allowed inside a
    render pass; RI forbids it there so the rule is the same on both backends.

## How RI does it

- **Vulkan**: one `vkCmdUpdateBuffer`.
- **D3D12**: one `WriteBufferImmediate` with one parameter per 32-bit value.

## Rules and errors

In validation builds, each of these is a **programmer error**:

- The buffer needs `BufferUsage::eCopyDst`.
- `offset` and the size are multiples of 4, and the size is not zero.
- The size is at most `kMaxSmallWriteSize`:
  `cmdWriteSmallData: 68 bytes is more than kMaxSmallWriteSize (64); write larger data
  through an upload buffer and cmdCopyBuffer (P107)`.
- The range fits in the buffer.
- Not inside a rendering section.

The typed helper turns the size rules into compile-time constraints; the offset and the range
are still checked at run time.

For barriers, **a small write counts as a copy** (`Stage::eCopy`), like a clear: follow it
with `cmdBarrier(cmd, Stage::eCopy, ...)` before the shaders read what it wrote.

## Related decisions

- [P107](../design-notes/decisions.md#p107): small writes and typed values in helpers.
- [P36](../design-notes/decisions.md#p36): coarse barriers by stage.

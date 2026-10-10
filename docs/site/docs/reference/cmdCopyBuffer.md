---
symbols: [RI::cmdCopyBuffer]
---

# RI::cmdCopyBuffer

## What it does

`RI::cmdCopyBuffer(cmd, src, srcOffset, dst, dstOffset, size)` records a copy of `size` bytes
from `src` (starting at `srcOffset`) to `dst` (starting at `dstOffset`), done by the GPU when
the list runs ([P47](../design-notes/decisions.md#p47)).

Copies are how data moves between [memory kinds](../concepts/memory.md):

- **upload**: write into a buffer in `eUpload` memory from the CPU, then copy it to a buffer in
  `eGPU` memory;
- **readback**: copy from `eGPU` memory to a buffer in `eReadback` memory, wait for the
  [timeline point](../concepts/timeline.md), then read it on the CPU.

The source needs the usage `eCopySrc`, the destination `eCopyDst`.

## Signature

--8<-- "generated/signatures/RI.cmdCopyBuffer.md"

## Example

```cpp
--8<-- "snippets/ri/compute.cpp:copy-in"
```

A copy is a write like any other: put a [barrier](cmdBarrier.md) between it and a later
command that reads the destination, and between an earlier command that writes the source and
the copy.

```cpp
--8<-- "snippets/ri/compute.cpp:record"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:copy-in"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:copy-in"
    ```

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:copy-out"
    ```

    What to watch for: one call can copy several regions (`VkBufferCopy` array); the source
    comes first.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:copy-in"
    ```

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:copy-out"
    ```

    What to watch for: `CopyBufferRegion` takes the **destination first**.

## Rules and errors

- Both ranges must lie inside their buffers, and must not overlap if `src` and `dst` are the
  same buffer; the native validation layers report violations.
- In validation builds, a destroyed buffer is a **programmer error**.
- Copies between textures and buffers come with textures, together with a query of the exact
  staging layout ([P47](../design-notes/decisions.md#p47)).

## Related decisions

- [P47](../design-notes/decisions.md#p47): recorded copy commands; upload and readback as user
  steps.
- [P36](../design-notes/decisions.md#p36): explicit barriers.

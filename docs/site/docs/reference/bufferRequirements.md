---
symbols: [RI::bufferRequirements, RI::MemoryRequirements]
---

# RI::bufferRequirements

## What it does

Before you place a buffer in a memory block, you need to know **how many bytes** it takes and
**at what alignment** its offset must be. Both depend on the GPU and on the buffer's usage, so
RI asks the driver: `RI::bufferRequirements(device, desc)` returns them for a buffer
description, without creating anything ([P58](../design-notes/decisions.md#p58)).

Use the result to size memory blocks and to choose offsets: each buffer's offset must be a
multiple of `alignment`, and `size` bytes from there must fit in the block.

Every buffer fits every [memory kind](../concepts/memory.md), so there is no "memory type"
part in the answer.

## Signature

--8<-- "generated/signatures/RI.bufferRequirements.md"

--8<-- "generated/signatures/RI.MemoryRequirements.md"

## Example

```cpp
--8<-- "snippets/ri/compute.cpp:requirements"
```

with a small helper:

```cpp
--8<-- "snippets/ri/compute.cpp:align-up"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:requirements"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:requirements"
    ```

    What to watch for: before Vulkan 1.3 you had to create the buffer first and call
    `vkGetBufferMemoryRequirements`. `memoryTypeBits` lists the memory types the buffer
    accepts.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:requirements"
    ```

    What to watch for: placed buffers are aligned to 64 KiB
    (`D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT`), and their size is rounded up to it.

## Rules and errors

- Typical values: on D3D12, size and alignment are multiples of 64 KiB for every buffer; on
  Vulkan, alignments are much smaller (often 16 to 256 bytes), so many buffers fit in a block.
- In validation builds, [`createBuffer`](buffer.md) checks the placement: an offset that is
  not a multiple of `alignment`, or a buffer that does not fit, is a **programmer error**
  (`createBuffer: placing the buffer in memory: offset 100 is not a multiple of the required
  alignment 256`).

## Related decisions

- [P58](../design-notes/decisions.md#p58): a memory-requirements query per resource
  description.
- [P31](../design-notes/decisions.md#p31): placed resources.

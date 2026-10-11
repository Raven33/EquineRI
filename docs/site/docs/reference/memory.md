---
symbols: [RI::allocateMemory, RI::freeMemory, RI::Memory, RI::MemoryKind]
---

# RI::allocateMemory, RI::freeMemory

## What it does

`RI::allocateMemory(device, kind, size)` reserves a **block of memory** of `size` bytes and
one of four [kinds](../concepts/memory.md). You then place buffers in it with
[`RI::createBuffer`](buffer.md), at offsets you choose. `RI::freeMemory(device, memory)` gives
the block back.

Step by step, `allocateMemory`:

1. picks the native memory for the kind (chosen when the device was created). With a third
   argument, `allocateMemory(device, kind, size, &requirements)`, it picks memory of that
   kind that **suits that resource**: some drivers allow some resources only in some of a
   kind's native memory types (RADV puts [indirect scratch](indirect.md) only in its
   32-bit-addressable GPU memory). On D3D12 the argument changes nothing;
2. allocates `size` bytes of it, allowing GPU addresses for buffers placed inside;
3. for every kind except `eGPU`, **maps** the block once: `memory.mapped` is its CPU address
   for the block's whole life ([P33](../design-notes/decisions.md#p33));
4. returns an `RI::Memory`: the kind, the size, the mapped address.

Memory is a large, slow allocation in the driver. Allocate a few big blocks and place many
buffers in each, rather than one block per buffer.

## Signature

--8<-- "generated/signatures/RI.allocateMemory.md"

--8<-- "generated/signatures/RI.freeMemory.md"

--8<-- "generated/signatures/RI.Memory.md"

--8<-- "generated/signatures/RI.MemoryKind.md"

## Example

```cpp
--8<-- "snippets/ri/compute.cpp:allocate-memory"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:allocate-memory"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:find-memory-type"
    ```

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:allocate-function"
    ```

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:allocate-memory"
    ```

    What to watch for: `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` is needed for any buffer in
    the block to have a GPU address. Drivers limit the number of allocations
    (`maxMemoryAllocationCount`, often 4096).

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:allocate-function"
    ```

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:allocate-memory"
    ```

    What to watch for: heap sizes are whole 64 KiB blocks. RI's heaps accept buffers and
    textures (`ALLOW_ALL_BUFFERS_AND_TEXTURES`, resource heap tier 2); the canonical code
    above takes only buffers.

## Rules and errors

- `eGPUUpload` needs ReBAR: check `device.limits.gpuUploadMemory`. Without it, the call
  reports an **unsupported** error.
- Running out of memory reports an **out of memory** error.
- Free a block only after destroying every buffer placed in it, and only when the GPU no
  longer uses them (after their [timeline point](../concepts/timeline.md)).
- `freeMemory` resets the struct to empty. In validation builds, using freed memory (or
  memory from another device) is a **programmer error**.
- Placing a resource in memory whose native type it does not allow is a **programmer error**
  (Vulkan, validation builds). `requirements.kinds` lists the kinds that can hold it; allocate
  with `&requirements` when the kind's usual memory does not.
- On D3D12, the block's size is rounded up to 64 KiB; `memory.size` is the size you asked for.

## Related decisions

- [P31](../design-notes/decisions.md#p31): you allocate memory and place resources in it.
- [P32](../design-notes/decisions.md#p32): the four memory kinds.
- [P33](../design-notes/decisions.md#p33): CPU-visible kinds stay mapped; coherent memory.
- [P61](../design-notes/decisions.md#p61): GPU addresses for every buffer.
- [P94](../design-notes/decisions.md#p94): how memory types are chosen on Vulkan.

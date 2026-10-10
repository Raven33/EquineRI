# Memory kinds and placed resources

## Two steps: memory, then resources

In many engines, "create a buffer" also finds memory for it. RI splits this in two
([P31](../design-notes/decisions.md#p31)):

1. **Allocate a memory block** of a given kind and size: [`RI::allocateMemory`](../reference/memory.md).
2. **Place resources in it** at offsets you choose: [`RI::createBuffer(device, memory, offset, desc)`](../reference/buffer.md).

A buffer owns no memory; it is a view of part of a block. This lets you write your own
allocator (one large block, sub-allocated however you like), and place two resources at the
same offset when they are never used at the same time (*aliasing*). Allocating memory for each
buffer automatically is a helper, built on these two calls ([P41](../design-notes/decisions.md#p41)).

To place a buffer you need its size and alignment, which depend on the GPU and the usage:
[`RI::bufferRequirements`](../reference/bufferRequirements.md) gives both. Every buffer fits
every memory kind, so you never have to match buffers to memory types.

```cpp
--8<-- "snippets/ri/compute.cpp:requirements"
```

```cpp
--8<-- "snippets/ri/compute.cpp:allocate-memory"
```

```cpp
--8<-- "snippets/ri/compute.cpp:create-buffers"
```

## The four memory kinds

Native APIs list many memory types; RI has four **kinds**, named after what they are for
([P32](../design-notes/decisions.md#p32)):

| Kind | Who writes, who reads | Mapped | Typical use |
|---|---|---|---|
| `eGPU` | the GPU | no | everything the GPU works on: fastest for the GPU |
| `eUpload` | the CPU writes, the GPU reads | yes | staging data to copy into `eGPU` memory; small per-frame constants |
| `eReadback` | the GPU writes, the CPU reads | yes | results copied back from `eGPU` memory |
| `eGPUUpload` | the CPU writes into GPU memory | yes | data the GPU reads often and the CPU updates, without a copy (needs ReBAR) |

`eGPUUpload` exists only where **all** of the GPU's memory is visible to the CPU (resizable
BAR, "ReBAR"). Check [`device.limits.gpuUploadMemory`](../reference/DeviceLimits.md) before
using it; allocating it elsewhere reports an **unsupported** error.

### What each kind is natively

| Kind | Vulkan memory type | D3D12 heap type |
|---|---|---|
| `eGPU` | `DEVICE_LOCAL`, preferably not `HOST_VISIBLE` | `DEFAULT` |
| `eUpload` | `HOST_VISIBLE \| HOST_COHERENT`, preferably neither `DEVICE_LOCAL` nor `HOST_CACHED` | `UPLOAD` |
| `eReadback` | `HOST_VISIBLE \| HOST_COHERENT`, preferably `HOST_CACHED`, not `DEVICE_LOCAL` | `READBACK` |
| `eGPUUpload` | `DEVICE_LOCAL \| HOST_VISIBLE \| HOST_COHERENT`, on the largest device-local heap | `GPU_UPLOAD` |

On Vulkan, RI picks one memory type per kind when the device is created, among the types
that accept buffers. It never uses AMD's device-coherent or uncached types, protected memory,
or lazily allocated memory ([P94](../design-notes/decisions.md#p94)). On D3D12, memory is an
`ID3D12Heap`; buffers are placed in it with `CreatePlacedResource2`. Which native memory pool
each kind uses matters for budgets, which RI does not track
([P66](../design-notes/decisions.md#p66)).

## Mapping: always on, never flushed

Memory of every kind except `eGPU` is **mapped once, when it is allocated**, and stays mapped
until it is freed ([P33](../design-notes/decisions.md#p33)). `memory.mapped` is the CPU address
of the block, and `buffer.mapped` the address of a buffer placed in it (`memory.mapped +
offset`). There is no map or unmap call, and no flush or invalidate: RI chooses coherent memory.

```cpp
--8<-- "snippets/ri/compute.cpp:write-mapped"
```

Two rules:

- **Upload memory is write-only in practice.** Reading it from the CPU works but is very slow
  (it is uncached).
- **Read GPU results only after the timeline point** of the work that wrote them
  ([Timeline points](timeline.md)). RI makes the GPU's writes visible to the CPU at that
  point on both backends.

## GPU addresses

Every buffer has a 64-bit **GPU address**, `buffer.gpuAddress`, always
([P61](../design-notes/decisions.md#p61)). Shaders can read through it with buffer pointers,
and GPU-built tables (acceleration-structure instances, work-graph inputs, your own
virtual-memory tables) store it. On Vulkan this means every memory block is allocated with
`VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` and every buffer has
`VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`; on D3D12 every buffer has an address anyway.

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

    What to watch for: memory types differ per GPU, so you search for one with the properties
    you need among those the resource accepts (`memoryTypeBits`). Without
    `HOST_COHERENT`, CPU writes need `vkFlushMappedMemoryRanges` and CPU reads
    `vkInvalidateMappedMemoryRanges`.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:allocate-function"
    ```

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:allocate-memory"
    ```

    What to watch for: D3D12 maps **resources**, not heaps. To keep a whole block mapped, RI
    places one buffer spanning the heap and maps it once; the buffers you place later share
    its memory.

## Related decisions

- [P31](../design-notes/decisions.md#p31): RI is "level 2": you allocate memory and place
  resources; automatic allocation is a helper.
- [P32](../design-notes/decisions.md#p32): the four memory kinds.
- [P33](../design-notes/decisions.md#p33): mapped once for the whole lifetime, coherent.
- [P61](../design-notes/decisions.md#p61): every buffer has a GPU address.
- [P94](../design-notes/decisions.md#p94): how Vulkan memory types are chosen; `eGPUUpload` only
  with ReBAR.

---
symbols: [RI::createBuffer, RI::destroyBuffer, RI::Buffer, RI::BufferDesc, RI::BufferUsage]
---

# RI::createBuffer, RI::destroyBuffer

## What it does

A **buffer** is a range of GPU-accessible memory with no format: shaders read it as structured
elements or raw bytes, copies move it, constant buffers live in it.

`RI::createBuffer(device, memory, offset, desc)` creates a buffer of `desc.size` bytes,
**placed** in `memory` at `offset` ([P31](../design-notes/decisions.md#p31)). It allocates
nothing: the bytes belong to the memory block. `RI::destroyBuffer(device, buffer)` destroys
the buffer; the memory stays.

The returned `RI::Buffer` holds facts you use directly:

- `size` and `usage`, as you described them;
- `gpuAddress`: its 64-bit GPU address, always available ([P61](../design-notes/decisions.md#p61));
- `mapped`: its CPU address (`memory.mapped + offset`), or null in `eGPU` memory.

The **usage flags** say what the buffer will be used for
([P58](../design-notes/decisions.md#p58)). Set every use you need; unused flags can cost
performance on some GPUs.

| `BufferUsage` | Lets you | Vulkan usage | D3D12 |
|---|---|---|---|
| `eRead` | write a [`ReadView`](writeDescriptors.md) (`StructuredBuffer`, `ByteAddressBuffer`) | `STORAGE_BUFFER` | - |
| `eReadWrite` | write a `ReadWriteView` (`RWStructuredBuffer`, ...) | `STORAGE_BUFFER` | `ALLOW_UNORDERED_ACCESS` |
| `eConstant` | write a `ConstantBufferView` (`ConstantBuffer<T>`) | `UNIFORM_BUFFER` | - |
| `eCopySrc` / `eCopyDst` | copy from / to it ([`cmdCopyBuffer`](cmdCopyBuffer.md)) | `TRANSFER_SRC` / `TRANSFER_DST` | - |
| `eIndirect` | read indirect arguments from it (later) | `INDIRECT_BUFFER` | - |

Every Vulkan buffer also gets `SHADER_DEVICE_ADDRESS`, for its GPU address. Combine flags
with `|`.

## Signature

--8<-- "generated/signatures/RI.createBuffer.md"

--8<-- "generated/signatures/RI.destroyBuffer.md"

--8<-- "generated/signatures/RI.Buffer.md"

--8<-- "generated/signatures/RI.BufferDesc.md"

--8<-- "generated/signatures/RI.BufferUsage.md"

## Example

```cpp
--8<-- "snippets/ri/compute.cpp:create-buffers"
```

Writing through a mapped buffer:

```cpp
--8<-- "snippets/ri/compute.cpp:write-mapped"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:create-buffers"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:create-buffer-function"
    ```

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:create-buffers"
    ```

    What to watch for: a Vulkan buffer is created unbound, then bound to memory once with
    `vkBindBufferMemory`. The memory must have been allocated with the device-address flag
    for `vkGetBufferDeviceAddress` to work.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:buffer-desc"
    ```

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:create-buffer-function"
    ```

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:create-buffers"
    ```

    What to watch for: `CreatePlacedResource2` (with enhanced barriers) takes an initial
    *layout*; buffers have none, so it is `UNDEFINED` and stays so.

## Rules and errors

- `offset` must be a multiple of the alignment from
  [`bufferRequirements`](bufferRequirements.md), and the buffer must fit in the block:
  checked in validation builds (**programmer error**).
- Destroy a buffer only when the GPU no longer uses it (after its
  [timeline point](../concepts/timeline.md)), and before freeing its memory.
- `destroyBuffer` resets the struct to empty. `Buffer` is a thin value: copies refer to the
  same buffer ([P81](../design-notes/decisions.md#p81)). In validation builds, using any copy
  of a destroyed buffer (in a descriptor or a copy) is a **programmer error**:
  `buffer was destroyed`.
- Two buffers may share memory (aliasing) if they are never used at the same time.

## Related decisions

- [P31](../design-notes/decisions.md#p31): buffers are placed in user memory.
- [P58](../design-notes/decisions.md#p58): explicit usage flags.
- [P61](../design-notes/decisions.md#p61): every buffer has a GPU address.
- [P81](../design-notes/decisions.md#p81), [P82](../design-notes/decisions.md#p82): thin
  values with public fields.

# Descriptor heaps and bindless

## What a descriptor is

A shader cannot use a raw pointer to "the buffer". It reads through a **descriptor**: a small
GPU record that says where a resource is, how big it is, and how the shader sees it (read-only,
read-write, constant buffer). RI names descriptors by what the shader does with them
([P87](../design-notes/decisions.md#p87)):

| RI | HLSL | D3D12 | Vulkan |
|---|---|---|---|
| `RI::ReadView` | `StructuredBuffer`, `ByteAddressBuffer` | SRV | storage buffer |
| `RI::ReadWriteView` | `RWStructuredBuffer`, `RWByteAddressBuffer` | UAV | storage buffer |
| `RI::ConstantBufferView` | `ConstantBuffer<T>` | CBV | uniform buffer |

Texture views and samplers come with textures.

## Bindless: one heap, slots you choose

RI is **bindless** ([P6](../design-notes/decisions.md#p6)). Instead of binding each resource
to a numbered register before a draw or dispatch, you keep one large array of descriptors, the
**descriptor heap**, and the shader picks the one it wants by index:

```hlsl
--8<-- "snippets/hlsl/doc_compute.hlsl:heap-access"
```

`ResourceDescriptorHeap` is HLSL's name for the heap (shader model 6.6). The slots usually
arrive through [root data](root-data.md), as here, or are read from another buffer.

On the C++ side, a heap is created with a size you choose, within
[`device.limits`](../reference/DeviceLimits.md) ([P59](../design-notes/decisions.md#p59)):

```cpp
--8<-- "snippets/ri/compute.cpp:create-heap"
```

and you **write descriptors into slots you choose** ([P42](../design-notes/decisions.md#p42)).
RI never assigns slots for you; doing so automatically is a helper.

```cpp
--8<-- "snippets/ri/compute.cpp:write-descriptors"
```

A heap is really a pair: one array for views (`resourceCount` slots) and one for samplers
(`samplerCount` slots), as in D3D12.

## Rules

- **Writes take effect at once.** Never overwrite a slot that GPU work still in flight may
  read: wait for that work's [timeline point](timeline.md) first, as for destruction
  ([P59](../design-notes/decisions.md#p59)).
- **Set the heap before the pipeline.** In a command list, call
  [`cmdSetHeap`](../reference/cmdSetHeap.md) before
  [`cmdSetPipeline`](../reference/cmdSetPipeline.md). D3D12 requires the descriptor heaps to be
  bound before a root signature that indexes them
  (`D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`), so RI makes it the rule on
  every backend, checked in validation builds ([P94](../design-notes/decisions.md#p94)).
- **View alignment.** One rule for both backends (D3D12 counts view ranges in elements and
  constant buffers in 256-byte blocks), checked in validation builds:
    - read and read-write views start at a multiple of **16 bytes** and of the stride, and
      cover whole elements (4 bytes for raw views, `stride` for structured ones);
    - constant buffer views start at a multiple of **256 bytes**; their size is rounded up to
      256, and the rounded size must fit in the buffer.
- **Slots are checked** in validation builds: a slot past the heap's end is a programmer
  error (on D3D12 it would silently write past the heap).

## How the heap works on each backend

=== "RI"

    One call creates both arrays; one call writes any number of descriptors.

    ```cpp
    --8<-- "snippets/ri/compute.cpp:write-descriptors-span"
    ```

=== "Vulkan"

    Vulkan has no descriptor heap in its core API. RI emulates one the way DXC compiles
    `ResourceDescriptorHeap[]` by default ([P94](../design-notes/decisions.md#p94), D1):

    - **set 0, binding 0** is one large array of **mutable descriptors**
      (`VK_EXT_mutable_descriptor_type`): each slot can hold any buffer or image descriptor;
    - **set 1, binding 0** is the sampler array. DXC's default puts samplers at set 0,
      binding 1; RI's compile rule moves them with `-fvk-bind-sampler-heap 0 1`, so both arrays
      can have any size ([Compiling shaders](shaders.md)). A shader compiled without it is
      rejected with a readable error;
    - both arrays are **update-after-bind**, **partially bound** and of **variable count**:
      you may write slots while the set is bound, leave slots empty, and size each heap freely;
    - one set layout per array for the whole device, so every pipeline works with every heap.

    `writeDescriptors` is one `vkUpdateDescriptorSets` call for the whole list.

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:set-layout"
    ```

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:create-heap"
    ```

    The closest native match to D3D12, `VK_EXT_descriptor_heap`, is not available on every
    target driver yet; RI will revisit it.

=== "D3D12"

    Two shader-visible descriptor heaps: `CBV_SRV_UAV` and `SAMPLER`. A slot is the heap's
    start plus `slot` times the descriptor size. `writeDescriptors` calls
    `CreateShaderResourceView`, `CreateUnorderedAccessView` or `CreateConstantBufferView` once
    per write.

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:create-heap"
    ```

## Related decisions

- [P6](../design-notes/decisions.md#p6): bindless first.
- [P30](../design-notes/decisions.md#p30): the shader decides how it reaches the heap.
  Resources declared at registers (not through the heap) are not supported yet: pipeline
  creation reports a readable error.
- [P42](../design-notes/decisions.md#p42): you write descriptors into slots you choose.
- [P59](../design-notes/decisions.md#p59): heap sizes are yours, within device limits; writes
  take effect at once.
- [P87](../design-notes/decisions.md#p87): descriptor kinds named by access.
- [P94](../design-notes/decisions.md#p94): the Vulkan mechanism (D1), the batching form, the
  view alignment rule, heap before pipeline.

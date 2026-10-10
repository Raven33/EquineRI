---
symbols: [RI::createDescriptorHeap, RI::destroyDescriptorHeap, RI::DescriptorHeap, RI::DescriptorHeapDesc]
---

# RI::createDescriptorHeap, RI::destroyDescriptorHeap

## What it does

A **descriptor heap** is the array of descriptors that bindless shaders index
(`ResourceDescriptorHeap[slot]`). `RI::createDescriptorHeap(device, desc)` creates one with
`desc.resourceCount` slots for views and `desc.samplerCount` slots for samplers
([P59](../design-notes/decisions.md#p59)). The slots start empty; you fill them with
[`RI::writeDescriptors`](writeDescriptors.md) and make the heap current in a command list with
[`RI::cmdSetHeap`](cmdSetHeap.md).

How the heap works on each backend, and why, is explained in
[Descriptor heaps and bindless](../concepts/descriptors.md).

Most programs create **one heap** for the whole run and pick slots in it; switching heaps in
the middle of a command list is allowed but costs time on some GPUs.

## Signature

--8<-- "generated/signatures/RI.createDescriptorHeap.md"

--8<-- "generated/signatures/RI.destroyDescriptorHeap.md"

--8<-- "generated/signatures/RI.DescriptorHeap.md"

--8<-- "generated/signatures/RI.DescriptorHeapDesc.md"

## Example

```cpp
--8<-- "snippets/ri/compute.cpp:create-heap"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:create-heap"
    ```

=== "Vulkan"

    A set layout (once per device in RI), a pool, and one descriptor set of the chosen size:

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:set-layout"
    ```

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:create-heap"
    ```

    What to watch for: the pool, the layout and the binding all need their update-after-bind
    flags, and the mutable type list must be given to both the layout and the pool. RI also
    allocates a second set (set 1) for samplers.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:create-heap"
    ```

    What to watch for: only one shader-visible heap of each type can be bound at a time, and
    the views heap of a GPU is limited to 1,000,000 descriptors.

## Rules and errors

- `resourceCount` and `samplerCount` must not exceed
  [`device.limits`](DeviceLimits.md) (`maxResourceDescriptors`, `maxSamplerDescriptors`):
  otherwise a **programmer error**, in every build. The limits are at most 1,000,000 and 2048
  (D3D12's resource binding tier 3), or less if the GPU has less.
- A count of 0 is allowed: RI still creates one slot internally (some drivers reject empty
  arrays), but `heap.resourceCount` stays 0 and every slot is out of range.
- Destroy a heap only when no in-flight GPU work uses it (after the
  [timeline point](../concepts/timeline.md)). `destroyDescriptorHeap` resets the struct.

## Related decisions

- [P59](../design-notes/decisions.md#p59): you create the heaps with sizes you choose, within
  device limits.
- [P42](../design-notes/decisions.md#p42): slots are yours.
- [P94](../design-notes/decisions.md#p94): the Vulkan mechanism (D1) and the limits.

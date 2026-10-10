---
symbols: [RI::Vk::native, RI::Vk::NativeDescriptorHeap, RI::Vk::NativePipeline]
---

# Vulkan: native handles of RI objects

## What it does

Every RI object has a `RI::Vk::native(...)` overload in `<RI/Vk/Native.hpp>` that returns the
Vulkan handle(s) behind it ([P84](../../design-notes/decisions.md#p84)). Use them to call
Vulkan functions RI does not wrap: naming objects for debuggers, querying properties,
recording a native command.

| RI object | `RI::Vk::native(...)` returns |
|---|---|
| `Device` | `NativeDevice`: instance, physical device, device, function table, queue (see [device creation](device-creation.md)) |
| `Memory` | `VkDeviceMemory` |
| `Buffer` | `VkBuffer` |
| `DescriptorHeap` | `NativeDescriptorHeap`: the descriptor pool and the two sets (set 0: views, set 1: samplers) |
| `Pipeline` | `NativePipeline`: the `VkPipeline` and its `VkPipelineLayout` |
| `CommandList` | `VkCommandBuffer` |

The handles are **borrowed**: RI destroys them when you destroy the RI object.

## Signature

--8<-- "generated/signatures/RI.Vk.native.md"

--8<-- "generated/signatures/RI.Vk.NativeDescriptorHeap.md"

--8<-- "generated/signatures/RI.Vk.NativePipeline.md"

## Example: name a buffer

```cpp
--8<-- "snippets/vulkan/native-device.cpp:object-handles"
```

## Example: a native command in an RI command list

```cpp
--8<-- "snippets/vulkan/native-device.cpp:native-command"
```

Device-level functions such as `vkCmdFillBuffer` go through the device's function table,
`RI::Vk::native(device).functions` ([P89](../../design-notes/decisions.md#p89)).

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/vulkan/native-device.cpp:object-handles"
    ```

=== "Vulkan"

    In a plain Vulkan program you already hold these handles; see
    [`createBuffer`](../../reference/buffer.md) and the other reference pages for how each is
    created.

=== "D3D12"

    The D3D12 equivalents: [D3D12: native objects](../d3d12/objects.md).

## Rules

- Do not destroy or free a handle RI owns; destroy the RI object instead.
- A native command recorded between RI commands must leave the state RI relies on as it was:
  the bound pipeline, its layout, the heap's descriptor sets and the push constants. Commands
  that bind nothing (fills, copies, queries, debug labels) are safe. Native sections that save
  and restore RI's state are planned ([P27](../../design-notes/decisions.md#p27),
  [P70](../../design-notes/decisions.md#p70)).
- Synchronize native work with barriers as usual; RI's [`cmdBarrier`](../../reference/cmdBarrier.md)
  covers native commands too, by stage.
- A descriptor written natively into the heap's set must use a type from RI's mutable list:
  sampled or storage image, uniform or storage texel buffer, uniform or storage buffer.

## Related decisions

- [P84](../../design-notes/decisions.md#p84): native access is gated.
- [P22](../../design-notes/decisions.md#p22): native resources are always reachable.
- [P27](../../design-notes/decisions.md#p27), [P70](../../design-notes/decisions.md#p70):
  native sections (later).
- [P89](../../design-notes/decisions.md#p89): the per-device function table.
- [P94](../../design-notes/decisions.md#p94): the heap's sets.

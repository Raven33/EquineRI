---
symbols: [RI::Vk::DeviceFeatures, RI::Vk::defaultDeviceFeatures, RI::Vk::NativeDeviceAdditions, RI::Vk::createDevice, RI::Vk::native, RI::Vk::NativeDevice, RI::Vk::LayoutMode]
---

# Vulkan: device creation

## What it does

[`RI::createDevice`](../../reference/createDevice.md) turns on every feature the GPU
supports except robustness, and the extensions RI uses. When you need something else (robust
buffer access to debug an out-of-bounds read, an extension RI does not enable), the Vulkan
backend has a second `createDevice` that takes **native additions**
([P60](../../design-notes/decisions.md#p60), [P92](../../design-notes/decisions.md#p92)).
All of it lives in `<RI/Vk/Native.hpp>`.

There are three ways to change the device, and you can combine them:

1. **Change RI's features.** `RI::Vk::defaultDeviceFeatures(gpu)` returns exactly what RI
   would enable, as a plain struct of Vulkan feature structures. Change any field and pass a
   pointer to it in `.features`.
2. **Add extensions.** List extension names in `.extensions`; RI enables them alongside its
   own.
3. **Add feature structures RI does not know.** Put them in `.pNext`. RI appends your chain
   after its own and does not touch it.

A fourth addition is for testing and measuring: `.layoutMode` forces how the backend hides
image layouts (below).

## Signature

--8<-- "generated/signatures/RI.Vk.DeviceFeatures.md"

--8<-- "generated/signatures/RI.Vk.defaultDeviceFeatures.md"

--8<-- "generated/signatures/RI.Vk.NativeDeviceAdditions.md"

--8<-- "generated/signatures/RI.Vk.createDevice.md"

## Example: turn robustness on

```cpp
--8<-- "snippets/vulkan/native-device.cpp:robustness"
```

## Example: an extension with its feature structure

```cpp
--8<-- "snippets/vulkan/native-device.cpp:extension"
```

## Example: force a layout mode

```cpp
--8<-- "snippets/vulkan/native-device.cpp:layout-mode"
```

The Vulkan backend hides image layouts in one of three **layout modes**
([P78](../../design-notes/decisions.md#p78)), described in
[Image layouts are RI's business](../../concepts/layouts.md):

--8<-- "generated/signatures/RI.Vk.LayoutMode.md"

| `layoutMode` | Effect |
|---|---|
| `eAuto` (default) | `eUnified` when `VK_KHR_unified_image_layouts` is enabled with its `unifiedImageLayouts` feature; otherwise `eTransitions` until the Milestone 3 measurement decides per GPU |
| `eUnified` | every texture rests in `GENERAL`, no switches; needs the extension (**unsupported** error otherwise) |
| `eGeneral` | mode G: `GENERAL` everywhere without the extension |
| `eTransitions` | mode T: resting layouts per usage, switched around RI's operations |

Your RI code is identical in every mode ([P16](../../design-notes/decisions.md#p16)); only
native code that touches RI's textures sees the difference, through each texture's resting
layout ([Vulkan: native handles](objects.md)). `NativeDevice::layoutMode` reports the mode in
use, never `eAuto`.

## The same in native code

In plain Vulkan, you build the feature chain yourself. RI's `DeviceFeatures` is that chain,
already filled in, without the `sType` / `pNext` bookkeeping.

=== "RI"

    ```cpp
    --8<-- "snippets/vulkan/native-device.cpp:robustness"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/device.cpp:create-device"
    ```

=== "D3D12"

    D3D12 has no feature enable step: the equivalent is a hook that runs before the device
    is created. See [D3D12: device creation](../d3d12/device-creation.md).

## Rules

- **`sType` and `pNext` in `DeviceFeatures` are ignored.** RI links the structures itself.
- Members of extensions RI does not enable on this GPU are ignored (e.g. `mesh` on a GPU
  without `VK_EXT_mesh_shader`). `v14` is used only on Vulkan 1.4 devices.
- After your changes, RI **turns off features whose prerequisites are off**, so
  `vkCreateDevice` stays valid. Today: update-after-bind for uniform/storage (texel)
  buffers when robust buffer access is on and the GPU cannot combine them; multiview mesh
  shaders without multiview; acceleration structures without buffer device addresses; ray
  queries without acceleration structures; and two features that need extensions RI does
  not enable (per-primitive shading rate in mesh shaders, unified image layouts for video).
- **Do not put RI's structures in `pNext`**, nor their single-feature equivalents (for
  example `VkPhysicalDeviceImageRobustnessFeatures`, which duplicates
  `v13.robustImageAccess`): Vulkan forbids chaining the same feature twice. Change
  `DeviceFeatures` instead.
- `defaultDeviceFeatures` opens a short-lived instance to ask the GPU. Call it with a `Gpu`
  from `listGpus`.
- RI's own commands need some features (mutable descriptors, timeline semaphores, buffer
  device addresses, synchronization2, descriptor indexing with update-after-bind, dynamic
  rendering, separate depth-stencil layouts, host query reset). Turning one
  of them off in `DeviceFeatures` makes `createDevice` report an **unsupported** error that
  names it. `mutableDescriptor` is the structure of `VK_EXT_mutable_descriptor_type`, which
  RI's [descriptor heaps](../../concepts/descriptors.md) are built on.
- An extension or feature the GPU does not support makes `vkCreateDevice` fail, reported as
  **unsupported** through the [error channel](../../concepts/error-channel.md).

## Native handles

--8<-- "generated/signatures/RI.Vk.NativeDevice.md"

--8<-- "generated/signatures/RI.Vk.native.md"

```cpp
--8<-- "snippets/vulkan/native-device.cpp:native-handles"
```

RI loads Vulkan with **volk** and never links the system loader
([P89](../../design-notes/decisions.md#p89)):

- **Device-level functions** (`vkCmd*`, `vkCreateBuffer`, `vkDeviceWaitIdle`, ...): call them
  through `functions`, the device's own function table. It skips the loader's dispatch.
- **Core instance-level functions** (`vkGetPhysicalDeviceProperties`, ...): call them
  directly; they are volk's globals.
- **Instance extension functions**: fetch them with
  `vkGetInstanceProcAddr(instance, "...")`. Volk's globals follow the newest instance, which
  may lack the extension.

The handles are borrowed: RI destroys them in `destroyDevice`.

## Related decisions

- [P60](../../design-notes/decisions.md#p60): everything supported except robustness;
  native additions at creation.
- [P92](../../design-notes/decisions.md#p92): `DeviceFeatures`, `defaultDeviceFeatures`,
  `extensions` and `pNext`; why replacing RI's structures by `sType` was rejected.
- [P84](../../design-notes/decisions.md#p84): native access is gated.
- [P89](../../design-notes/decisions.md#p89): volk and the per-device function table.
- [P78](../../design-notes/decisions.md#p78): the layout modes and the option to force one.

---
symbols: [RI::createDevice, RI::destroyDevice]
---

# RI::createDevice, RI::destroyDevice

## What it does

A **device** is your program's open connection to one GPU. Almost everything in RI (memory,
buffers, pipelines, command lists) is created from a device.

`RI::createDevice(desc)` opens the GPU named in `desc.gpu` (one returned by
[`RI::listGpus`](listGpus.md)) and returns an [`RI::Device`](DeviceDesc.md).
`RI::destroyDevice(device)` closes it again.

Step by step, `createDevice`:

1. opens the native API for this device alone: on Vulkan, the device's **own instance**; on
   D3D12, its own DXGI factory. Two devices never share these
   ([P92](../design-notes/decisions.md#p92));
2. finds your GPU again by its `id`, and checks it still meets RI's requirements;
3. creates the native device with **every feature the GPU supports turned on**, so Vulkan
   behaves like D3D12, which has no feature list ([P60](../design-notes/decisions.md#p60));
4. gets one **graphics queue** (Vulkan: from a queue family with graphics and compute; D3D12:
   a DIRECT queue);
5. prepares what the rest of RI needs: the queue's timeline (a Vulkan timeline semaphore or a
   D3D12 fence, see [Timeline points](../concepts/timeline.md)), the memory type behind each
   [memory kind](../concepts/memory.md), and on Vulkan the descriptor set layouts behind
   [descriptor heaps](../concepts/descriptors.md);
6. fills in [`device.limits`](DeviceLimits.md) and `device.graphicsQueue`;
7. in validation builds, turns on the native validation layer and routes its errors to the
   [error channel](../concepts/error-channel.md) (see [Validation](../concepts/validation.md)).

On Vulkan, the GPU must also support what RI's own commands rely on, or `createDevice`
reports an **unsupported** error naming what is missing: `VK_EXT_mutable_descriptor_type`,
timeline semaphores, buffer device addresses, synchronization2, and descriptor indexing with
partially bound, variable-count, update-after-bind descriptors of every buffer and image type.
Every GPU at RI's hardware floor has them.

The returned `device.gpu` is a fresh description of the GPU, read while creating the device.

`destroyDevice(device)` destroys the native device and the instance (or factory) created for
it, then sets `device.internal.state` to null.

## Signature

--8<-- "generated/signatures/RI.createDevice.md"

--8<-- "generated/signatures/RI.destroyDevice.md"

## Example

```cpp
--8<-- "snippets/ri/device.cpp:create-device"
```

```cpp
--8<-- "snippets/ri/device.cpp:destroy-device"
```

## The same in native code

The Vulkan and D3D12 code is a *canonical* native version: how you would create a device in
your own renderer, not necessarily what RI does internally.

=== "RI"

    ```cpp
    --8<-- "snippets/ri/device.cpp:create-device"
    ```

    ```cpp
    --8<-- "snippets/ri/device.cpp:destroy-device"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/device.cpp:create-device"
    ```

    ```cpp
    --8<-- "snippets/vulkan/device.cpp:destroy-device"
    ```

    What to watch for:

    - Vulkan enables **nothing** by default. Every feature you use must be turned on at
      device creation, through a chain of feature structures linked by `pNext`.
    - Asking with `vkGetPhysicalDeviceFeatures2` and passing the same chain back turns on
      everything supported. Turn off robustness yourself: it costs performance just by being
      on.
    - When the chain is in `pNext`, leave `pEnabledFeatures` null.
    - Device extensions (swapchain, mesh shaders, ray queries, ...) are separate from
      features: check them with `vkEnumerateDeviceExtensionProperties` and list them in
      `ppEnabledExtensionNames`. RI enables its own when the GPU has them.
    - Destroy in reverse order: the device, then the instance.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/device.cpp:create-device"
    ```

    ```cpp
    --8<-- "snippets/d3d12/device.cpp:destroy-device"
    ```

    What to watch for:

    - D3D12 has no feature list: once the device exists, everything the GPU supports is
      available. Ask with `CheckFeatureSupport` before using something.
    - The executable must export the Agility SDK version, or Windows uses its own, older
      D3D12 runtime:

        ```cpp
        --8<-- "snippets/d3d12/device.cpp:agility-exports"
        ```

    - `D3D12CreateDevice` returns the **same** `ID3D12Device` for every call on one adapter:
      it is reference counted, not a new device.

## What RI enables on Vulkan

"Everything supported" means every **feature** the GPU reports in the core Vulkan 1.0 to
1.4 feature structures and in the structures of RI's extensions, with these turned off:

- robustness: `robustBufferAccess`, `robustImageAccess`, `pipelineRobustness`;
- capture/replay features (for capture tools; they can change how drivers place memory);
- features that need an extension RI does not enable.

It does **not** mean every extension, because some extensions change behaviour just by
being on. RI always enables mutable descriptor type (required, for descriptor heaps), and
enables these extensions when the GPU has them: swapchain, mesh shader,
deferred host operations with acceleration structure and ray query, unified image layouts,
and device fault (validation builds only). The instance always has `VK_EXT_debug_utils`,
for object names and markers ([P53](../design-notes/decisions.md#p53)).

To change any of this (turn robustness on, add an extension), use the native overload
described in [Vulkan device creation](../native/vulkan/device-creation.md). On D3D12, a hook
runs just before the device is created: [D3D12 device creation](../native/d3d12/device-creation.md).

## Rules and errors

- Pass a GPU returned by `listGpus`. A GPU that is not available (for example a
  hand-made `RI::Gpu`) reports a **programmer error**:
  `the GPU '<name>' is not available; pass a GPU returned by listGpus()`.
- Vulkan validation builds need `VK_LAYER_KHRONOS_validation` installed; D3D12 validation
  builds need the Agility SDK debug layer next to the executable. Otherwise:
  **unsupported** error.
- `RI::Device` is a thin value: **copies refer to the same device**
  ([P81](../design-notes/decisions.md#p81)). Destroy it once.
- `destroyDevice` on a device that was never created, or on the copy you already destroyed,
  reports a **programmer error**. Other copies of a destroyed device are not detected
  today: do not use them.
- Wait for the GPU to finish the device's work before destroying it (there is no work
  submission yet; this matters once there is).
- Several devices may exist at once, also on the same GPU. On D3D12 they share one
  `ID3D12Device`, each holding a reference.

## Related decisions

- [P60](../design-notes/decisions.md#p60): you pick the GPU; Vulkan enables everything the
  GPU supports except robustness; native additions at creation.
- [P92](../design-notes/decisions.md#p92): how listing and device creation are implemented
  on each backend.
- [P81](../design-notes/decisions.md#p81): RI objects are thin values that own nothing.
- [P89](../design-notes/decisions.md#p89): Vulkan device functions go through a per-device
  function table (volk).
- [P21](../design-notes/decisions.md#p21): what validation builds add.

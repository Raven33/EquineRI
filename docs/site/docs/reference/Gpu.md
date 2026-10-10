---
symbols: [RI::Gpu, RI::GpuVendor, RI::GpuKind]
---

# RI::Gpu

## What it is

`RI::Gpu` describes one GPU as [`RI::listGpus`](listGpus.md) found it: plain facts, with no
native object inside. You read its fields to choose a GPU, then pass the whole struct to
[`RI::createDevice`](createDevice.md), which finds the same GPU again through its `id`.

Because it holds no native handle, a `Gpu` stays valid after the listing is gone, can be
copied freely, and is the same type on both backends.

## Signature

--8<-- "generated/signatures/RI.Gpu.md"

### GpuVendor

--8<-- "generated/signatures/RI.GpuVendor.md"

### GpuKind

--8<-- "generated/signatures/RI.GpuKind.md"

## Where each field comes from

| Field | Vulkan | D3D12 |
|---|---|---|
| `name` | `VkPhysicalDeviceProperties::deviceName` | `DXGI_ADAPTER_DESC1::Description` (converted to UTF-8) |
| `vendorId`, `deviceId` | `vendorID`, `deviceID` | `VendorId`, `DeviceId` |
| `vendor` | from the PCI vendor ID: `0x10DE` NVIDIA, `0x1002` / `0x1022` AMD, `0x8086` Intel | same |
| `kind` | `deviceType`: discrete, integrated, CPU (software) | `DXGI_ADAPTER_FLAG_SOFTWARE` gives software; otherwise integrated if the GPU is UMA (shares system memory), else discrete |
| `memory` | the largest heap with `VK_MEMORY_HEAP_DEVICE_LOCAL_BIT` | `DedicatedVideoMemory`; on UMA GPUs the larger of that and `SharedSystemMemory` |
| `id` | `deviceUUID` (16 bytes) | the adapter LUID (8 bytes; the rest is zero) |

So the same integrated GPU can report a different `memory` on each backend, and the `id`
of one GPU differs between backends. The `id` is stable within the process: use it to
recognise a GPU, not to store across runs.

## Example

```cpp
--8<-- "snippets/ri/device.cpp:list-gpus"
```

```cpp
--8<-- "snippets/ri/device.cpp:pick-gpu"
```

## The same in native code

Reading the same facts directly from the native API (canonical code, not RI's internals):

=== "RI"

    ```cpp
    --8<-- "snippets/ri/device.cpp:pick-gpu"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/device.cpp:describe-gpu"
    ```

    What to watch for: the stable ID (`VkPhysicalDeviceIDProperties`) is not in the basic
    properties. Chain it in `pNext` of `VkPhysicalDeviceProperties2`.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/device.cpp:describe-gpu"
    ```

    What to watch for: `DXGI_ADAPTER_DESC1` says nothing about D3D12 support. Create a
    short-lived device and ask it with `CheckFeatureSupport`.

## Rules and errors

- Pass `createDevice` a `Gpu` that `listGpus` returned (in this process). A hand-made `Gpu`
  whose `id` matches no available GPU reports a **programmer error**.
- Fields you do not set keep their defaults (`eOther`, `0`, an all-zero `id`).

## Related decisions

- [P60](../design-notes/decisions.md#p60): RI lists the GPUs (name, vendor, memory) and you
  pick one.
- [P92](../design-notes/decisions.md#p92): the fields, the `id` per backend, and how
  `memory` is computed.
- [P86](../design-notes/decisions.md#p86): plain structs, `e`-prefixed enumerators.

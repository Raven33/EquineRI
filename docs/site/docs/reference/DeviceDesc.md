---
symbols: [RI::DeviceDesc, RI::Device]
---

# RI::DeviceDesc, RI::Device

## What they are

`RI::DeviceDesc` is the **description** you pass to [`RI::createDevice`](createDevice.md).
Today it holds one field, the GPU to open. Descriptions in RI are plain structs filled with
C++20 designated initializers; fields you leave out keep their defaults, so new fields can be
added later without breaking your code ([P86](../design-notes/decisions.md#p86)).

`RI::Device` is what `createDevice` returns: a small struct with one public field, `gpu` (the
GPU it runs on), and an `internal` field that holds the backend's private state. You never
touch `internal`.

A `Device` is a **thin value** ([P81](../design-notes/decisions.md#p81)): copying it is cheap
and every copy refers to the same device. It owns nothing in the C++ sense (no destructor),
so you destroy it explicitly with [`RI::destroyDevice`](createDevice.md), exactly once.

## Signature

--8<-- "generated/signatures/RI.DeviceDesc.md"

--8<-- "generated/signatures/RI.Device.md"

The device struct is defined by each backend (`RI::Vk::Device`, `RI::D3D12::Device`) with the
same public fields ([P83](../design-notes/decisions.md#p83)); `RI::Device` names the one you
build with.

## Example

```cpp
--8<-- "snippets/ri/device.cpp:create-device"
```

## The same in native code

There is no description struct for the device in RI's sense: the native equivalent of
`DeviceDesc` is the arguments of the native create call.

=== "RI"

    ```cpp
    --8<-- "snippets/ri/device.cpp:create-device"
    ```

=== "Vulkan"

    `VkDeviceCreateInfo` plays the role of `DeviceDesc`; `VkDevice` and `VkQueue` the role of
    `RI::Device`.

    ```cpp
    --8<-- "snippets/vulkan/device.cpp:create-device"
    ```

=== "D3D12"

    The adapter and minimum feature level passed to `D3D12CreateDevice` play the role of
    `DeviceDesc`; `ID3D12Device` and `ID3D12CommandQueue` the role of `RI::Device`.

    ```cpp
    --8<-- "snippets/d3d12/device.cpp:create-device"
    ```

## Reaching the native objects

The native handles behind a `Device` (the `VkDevice`, the `ID3D12Device`, the queue, ...) are
not fields: you reach them through the backend's native header, with
`RI::Vk::native(device)` or `RI::D3D12::native(device)`. See
[Native access](../native/index.md).

## Rules and errors

- `DeviceDesc::gpu` must come from [`RI::listGpus`](listGpus.md); otherwise
  `createDevice` reports a **programmer error**.
- Do not copy `internal` between devices or change it.

## Related decisions

- [P81](../design-notes/decisions.md#p81): thin value objects.
- [P82](../design-notes/decisions.md#p82): plain structs with public fields; hidden state in
  `internal`.
- [P84](../design-notes/decisions.md#p84): native handles only through the native header.
- [P86](../design-notes/decisions.md#p86): descriptions with designated initializers.
- [P92](../design-notes/decisions.md#p92): the device owns its instance (Vulkan) or factory
  (D3D12).

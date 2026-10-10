---
symbols: [RI::D3D12::NativeDeviceAdditions, RI::D3D12::createDevice, RI::D3D12::native, RI::D3D12::NativeDevice]
---

# D3D12: device creation

## What it does

D3D12 has no feature enable step: once the device exists, everything the GPU supports is
available. A few things must still happen **before** `D3D12CreateDevice`, such as
`D3D12EnableExperimentalFeatures` for preview shader models. For those, the D3D12 backend
has a second `createDevice` with a **hook**, `beforeDeviceCreation`, that RI calls just
before it creates the device ([P60](../../design-notes/decisions.md#p60),
[P92](../../design-notes/decisions.md#p92)). All of it lives in `<RI/D3D12/Native.hpp>`.

Step by step:

1. write a function `void hook(IDXGIAdapter1 *adapter, void *user)`;
2. pass it in `.beforeDeviceCreation`, and anything it needs in `.user`;
3. RI picks the adapter for your GPU, calls `hook(adapter, user)`, then creates the device.

## Signature

--8<-- "generated/signatures/RI.D3D12.NativeDeviceAdditions.md"

--8<-- "generated/signatures/RI.D3D12.createDevice.md"

## Example

```cpp
--8<-- "snippets/d3d12/native-device.cpp:hook"
```

```cpp
--8<-- "snippets/d3d12/native-device.cpp:create-with-hook"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/d3d12/native-device.cpp:create-with-hook"
    ```

=== "Vulkan"

    Vulkan needs no hook: what must be decided before creation is the feature chain and
    the extension list, which RI exposes directly. See
    [Vulkan: device creation](../vulkan/device-creation.md).

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/device.cpp:experimental-features"
    ```

    What to watch for: experimental features apply to the whole process and need Windows
    developer mode. Enable them before *any* device is created on the adapter.

## The Agility SDK exports

A D3D12 executable must export the Agility SDK version and path, or Windows loads its own,
older D3D12 runtime. Exporting them from a library has no effect. RI provides a macro for
it; put it in exactly one `.cpp` file of each D3D12 executable, at namespace scope
([P55](../../design-notes/decisions.md#p55)):

```cpp
--8<-- "snippets/d3d12/native-device.cpp:agility-exports"
```

and in CMake, `equine_deploy_agility_sdk(<target>)` copies the runtime next to the
executable. In plain D3D12, the same two exports look like this:

```cpp
--8<-- "snippets/d3d12/device.cpp:agility-exports"
```

## Rules

- The hook runs **only** in `createDevice`. `listGpus` also creates short-lived devices to
  check each adapter; the hook does not run for those.
- The hook runs after the validation debug layer is turned on (validation builds), on the
  adapter that matches your `Gpu`.
- D3D12 returns the same `ID3D12Device` for every device created on one adapter. Two RI
  devices on one GPU share it, each holding a reference; a feature turned on by one hook
  affects both.

## Native handles

--8<-- "generated/signatures/RI.D3D12.NativeDevice.md"

--8<-- "generated/signatures/RI.D3D12.native.md"

```cpp
--8<-- "snippets/d3d12/native-device.cpp:native-handles"
```

The pointers are **borrowed**: RI keeps its references and releases them in
`destroyDevice`. Call `AddRef` if you need one to outlive the device.

## Related decisions

- [P60](../../design-notes/decisions.md#p60): native additions at device creation, a D3D12
  pre-device hook.
- [P92](../../design-notes/decisions.md#p92): the device owns its DXGI factory; the hook; the
  shared `ID3D12Device`.
- [P55](../../design-notes/decisions.md#p55): the pinned Agility SDK and its exports.
- [P84](../../design-notes/decisions.md#p84): native access is gated.

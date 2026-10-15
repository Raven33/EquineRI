# Validation

## What it is

A **validation build** is a build where extra checking is compiled in: the native
validation layers of Vulkan and D3D12 **and** RI's own checks. It is a compile-time switch,
`EQUINE_VALIDATION`, not a runtime option ([P21](../design-notes/decisions.md#p21)). A
release build has none of the checks and pays nothing for them.

## Turning it on

Each preset sets the switch explicitly: **on** for the `debug` presets, **off** for
`release` and `bench`. To override it:

```text
cmake --preset clang-debug -DEQUINE_VALIDATION=OFF
```

The value reaches your code as the macro `EQUINE_VALIDATION` (`0` or `1`), defined for every
target that links RI.

## What a validation build turns on

=== "RI"

    RI turns everything on for you when you create a device; there is nothing to call.

    - The native layer for the backend (see the Vulkan and D3D12 tabs).
    - Native errors go to the [error channel](error-channel.md) as **programmer errors**, so
      the program stops at the first one with RI's usual message. Warnings are printed and
      the program continues.
    - RI's own checks (below).

=== "Vulkan"

    - The device's instance loads `VK_LAYER_KHRONOS_validation`. If the layer is not
      installed, `createDevice` reports an *unsupported* error that tells you to install the
      Vulkan SDK or your distribution's validation-layers package.
    - A debug messenger receives warnings and errors. Errors of the *validation* type go to
      the error channel; everything else is printed with an `[EquineRI] Vulkan warning:` or
      `Vulkan error:` prefix. The loader's own errors (for example a stale driver manifest
      on your system) are printed, not treated as your mistake.
    - `VK_EXT_device_fault` is enabled when the GPU has it, for GPU crash diagnostics
      ([P53](../design-notes/decisions.md#p53)).
    - Layer settings turn on GPU-assisted validation (`gpuav_enable`) and shader `printf`
      (`printf_enable`, printed to standard output): see [Debugging](debugging.md).
    - `listGpus` never loads the layer: it uses a short-lived instance without layers.

    Canonical Vulkan: enable the layer when you create the instance.

    ```cpp
    --8<-- "snippets/vulkan/device.cpp:create-instance"
    ```

=== "D3D12"

    - The D3D12 debug layer, GPU-based validation and DRED (GPU crash breadcrumbs and
      page-fault reports, see [Debugging](debugging.md)) are turned on once per process,
      before the first device. If the debug layer is missing,
      `createDevice` reports an *unsupported* error: check that the Agility SDK runtime
      (`D3D12\d3d12SDKLayers.dll`) is next to the executable.
    - Debug-layer errors and corruption messages go to the error channel through
      `ID3D12InfoQueue1`; warnings are printed. Without `ID3D12InfoQueue1`, messages reach
      only the debugger output.
    - The DXGI debug factory is not used: it needs Windows' optional Graphics Tools.

    Canonical D3D12: enable the debug layer before creating the device.

    ```cpp
    --8<-- "snippets/d3d12/device.cpp:debug-layer"
    ```

## What RI checks itself

The native layers already check the native API thoroughly, and GPU-assisted validation
checks shader accesses. RI does **not** repeat those checks. It checks only what the native
layers cannot see: **RI's own rules** ([P88](../design-notes/decisions.md#p88)). Examples,
as the features arrive:

- using an object after it was destroyed (RI objects are copyable values, so the native
  layer cannot tell which copy is stale). The message names the object when it was given a
  name, at creation or with `setName`, for the last 4096 named objects destroyed
  ([P106](../design-notes/decisions.md#p106));
- the discard required after memory aliasing, the out map of a native section, root data
  marking and size, descriptor slot reuse, command-list reuse.

!!! note "Today"
    The checking machinery is in place and tested, but no RI function runs a check yet:
    the first checks arrive with buffers and textures. `destroyDevice` on a device that is
    already destroyed is reported in every build, validation or not.

### How a check works

This part is for readers of RI's source, or anyone writing checks for their own helpers in
the same style ([P91](../design-notes/decisions.md#p91)).

1. A **check** is a plain function in `src/RI/Validation/` that returns a `CheckResult`
   (`std::optional<std::string>`): empty when the rule holds, the message when it does not.
   A check never reports by itself, so tests can call it directly.
2. A backend function runs it with one line, `EQ_VALIDATE(check(...));`. In a validation
   build, a failed check is reported as a programmer error. With validation off, the line
   compiles to nothing and **its argument is not evaluated**, so a check may be expensive.
3. Use-after-destroy uses an `ObjectTracker` kept by each device in validation builds: each
   object carries an id; destroying any copy marks the id dead for all copies; ids are never
   reused, so an object from another device is also caught.

The reported location is the RI function that ran the check, not your line.

## Related decisions

- [P21](../design-notes/decisions.md#p21): validation is a compile-time flag; on for debug
  presets, off for release and bench.
- [P88](../design-notes/decisions.md#p88): RI checks only what native layers cannot see;
  checks are shared functions called through `EQ_VALIDATE`.
- [P91](../design-notes/decisions.md#p91): the validation skeleton: `CheckResult`,
  `EQ_VALIDATE`, `ObjectTracker`.
- [P92](../design-notes/decisions.md#p92): what device creation turns on in validation
  builds, per backend.
- [P53](../design-notes/decisions.md#p53): crash diagnostics (DRED, device fault) come with
  the validation flag.

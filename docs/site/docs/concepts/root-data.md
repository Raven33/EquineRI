# Root data

## What it is

**Root data** is a small block of values that you set from C++ for each dispatch or
draw, directly in the command list, without any buffer: heap slots, a scale factor, an
element count. It is the fastest way to pass a few values to a shader. Natively it is Vulkan's
*push constants* and D3D12's *root constants*.

## Declaring it in the shader

Mark the shader's root data once, with the macro `EQ_ROOT_DATA(Type, name)` from
`shaders/equine/root.hlsli` ([P45](../design-notes/decisions.md#p45)):

```hlsl
--8<-- "snippets/hlsl/doc_compute.hlsl:root-data"
```

The macro expands differently for each target:

| Target | `EQ_ROOT_DATA(Root, root)` becomes |
|---|---|
| SPIR-V (Vulkan) | `[[vk::push_constant]] ConstantBuffer<Root> root` |
| DXIL (D3D12) | `ConstantBuffer<Root> root : register(b0, space999)` |

Register space 999 is **reserved** for root data: nothing else may use it. RI finds the
constant buffer there in the DXIL (even with reflection stripped) and turns it into root
constants.

There is no size to write anywhere, no `[RootSignature]` attribute, and nothing to declare
per pipeline in C++: **the shader is the source of truth** for the size and layout of root
data ([P44](../design-notes/decisions.md#p44), [P54](../design-notes/decisions.md#p54)).
A shader may declare at most one root data block.

## Setting it from C++

Write a C++ struct with the same members in the same order:

```cpp
--8<-- "snippets/ri/compute.cpp:root-struct"
```

and set it after the pipeline, before the dispatch:

```cpp
--8<-- "snippets/ri/compute.cpp:set-state"
```

`cmdSetRootData(cmd, value)` sends `sizeof(value)` bytes. The bytes are recorded into the
command list at once, so you may change the struct afterwards.

## One C++ struct for both backends

SPIR-V and DXIL lay out constant buffers differently by default. RI's compile rule passes
`-fvk-use-dx-layout` to DXC when compiling to SPIR-V, so **both use D3D12's layout** and one
C++ struct matches both ([P54](../design-notes/decisions.md#p54),
[P93](../design-notes/decisions.md#p93)). With that layout, a struct of 32-bit scalars, as
above, maps one to one. Be careful with vectors: a `float3` followed by a `float` packs into
16 bytes, but a `float3` does not start in the middle of a 16-byte row.

The rule behind it: **a vector never crosses a 16-byte boundary.** A `float2` at offset 12
moves to 16, so `uint, uint, uint, float2` is 24 bytes in HLSL but 20 in C++. The C++ struct
does not move it for you: add the padding member yourself, or keep root data to 32-bit
scalars (`uint`, `float`, `int`), which match one to one. The [size check](#rules-and-errors)
catches a mismatch, but only the size, so a `static_assert(sizeof(MyRoot) == ...)` next to
the C++ struct helps.

A block the shader never reads is removed by the compiler and counts as **none**: a vertex
shader that declares `EQ_ROOT_DATA` but does not use it has `rootDataSize` 0, and setting
root data for it is an error. Set root data only for pipelines whose shaders read it.

## Rules and errors

- **Size check** (validation builds, [P24](../design-notes/decisions.md#p24)): the size you
  pass must equal the size the shader declares (`pipeline.rootDataSize`). Otherwise:
  `programmer error: root data is 8 bytes, but the pipeline's shader declares 20 bytes`.
  Setting root data for a pipeline whose shader declares none is also an error.
- **Size limit** ([P44](../design-notes/decisions.md#p44)): a shader may declare up to
  `device.limits.maxRootDataSize` bytes: 256 on D3D12 (64 root constants), the GPU's
  push-constant limit on Vulkan (at least 128). A larger shader fails
  `createComputePipeline` with an **unsupported** error.
- Set the pipeline first: root data is checked against the pipeline currently set.

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:set-state"
    ```

=== "Vulkan"

    The pipeline layout declares a push-constant range of exactly the root data's size:

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:create-pipeline"
    ```

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:root-data"
    ```

=== "D3D12"

    The root signature declares 32-bit root constants at `b0, space999`:

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:create-pipeline"
    ```

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:root-data"
    ```

## Related decisions

- [P44](../design-notes/decisions.md#p44): the size is whatever the shader declares; RI picks
  no budget.
- [P45](../design-notes/decisions.md#p45): `EQ_ROOT_DATA` and the reserved register space.
- [P24](../design-notes/decisions.md#p24): the C++ side is checked against the shader in
  validation builds.
- [P54](../design-notes/decisions.md#p54): one layout rule (`-fvk-use-dx-layout`).
- [P94](../design-notes/decisions.md#p94): root data size read from SPIR-V (SPIRV-Reflect) and
  DXIL (RI's own reader).

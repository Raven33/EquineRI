# Compiling shaders for RI

## RI takes bytecode

RI does not compile shaders: you give it **bytecode**, SPIR-V on Vulkan and DXIL on D3D12
([P29](../design-notes/decisions.md#p29)). Shaders are written in HLSL and compiled with
**one DXC** for both targets ([P4](../design-notes/decisions.md#p4)). To compile at run time, use the
[shader compiler helper](../helpers/shader-compiler.md).

```cpp
--8<-- "snippets/ri/compute.cpp:load-shader"
```

## The compile rule

Shaders for RI must be compiled with these DXC options
([P93](../design-notes/decisions.md#p93)):

| Option | Target | Why |
|---|---|---|
| `-T cs_6_6` (or newer) | both | shader model 6.6: `ResourceDescriptorHeap[]` (bindless) |
| `-E main` | both | RI's entry point is `main` |
| `-HV 2021` | both | HLSL 2021 |
| `-I <repository>/shaders` | both | finds `equine/root.hlsli` ([root data](root-data.md)) |
| `-spirv -fspv-target-env=vulkan1.3` | SPIR-V | Vulkan 1.3 SPIR-V |
| `-fvk-use-dx-layout` | SPIR-V | D3D12's buffer layout, so one C++ struct fits both ([P54](../design-notes/decisions.md#p54)) |
| `-fvk-bind-sampler-heap 0 1` | SPIR-V | samplers in set 1, binding 0, where RI's [heap](descriptors.md) puts them ([P94](../design-notes/decisions.md#p94)) |

Nothing is needed for DXIL beyond the common options. RI reads what it needs from DXIL with
its own reader, even with reflection and debug information stripped
(`-Qstrip_reflect -Qstrip_debug`) ([P67](../design-notes/decisions.md#p67)).

By hand:

```text
dxc -T cs_6_6 -E main -HV 2021 -I shaders -spirv -fspv-target-env=vulkan1.3 \
    -fvk-use-dx-layout -fvk-bind-sampler-heap 0 1 -Fo my_shader.spv my_shader.hlsl
dxc -T cs_6_6 -E main -HV 2021 -I shaders -Fo my_shader.dxil my_shader.hlsl
```

## In CMake

The build downloads one pinned DXC release (verified by SHA-256) at configure time. It ships
the DXIL signer, so DXIL compiled on Linux loads on Windows. `equine_add_shaders` compiles each
file to both `.spv` and `.dxil`, with the rule above:

```cmake
equine_add_shaders(my_app PROFILE cs_6_6 SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/my_shader.hlsl")
```

The outputs go to `<build>/shaders/`, and the target gets the define `EQUINE_SHADER_DIR`
pointing there. The function is available when tests, samples or the documentation snippets
are built.

## HLSL that works on both backends

Three things compile on both targets but behave differently unless you follow one rule:

- **Pass values from the vertex to the pixel shader in one struct**: see
  [below](#values-passed-to-the-pixel-shader).
- **Matrices in buffers are column-major.** A `float4x4` in a constant or structured buffer is
  read column by column unless declared `row_major`, on both backends (the compile rule gives
  both D3D12's layout). With `mul(matrix, vector)`, store the matrix column by column from
  C++ (GLM's layout), or declare it `row_major float4x4` if your C++ matrices are stored row by
  row. A wrong choice is silent: the image is just wrong.
- **Clip space is D3D12's on both backends:** y up, depth from 0 (near) to 1 (far). Use
  projection matrices for a 0-to-1 depth range (GLM: `GLM_FORCE_DEPTH_ZERO_TO_ONE`), not
  OpenGL's -1-to-1 ([rendering](rendering.md#clip-space-points-up-on-both-backends)).

## Values passed to the pixel shader

The vertex shader (or mesh shader) outputs a position and other values: a color, texture
coordinates, a normal. With tessellation or a geometry shader, the last of those stages
(geometry, else domain) plays this part: "the vertex shader" below means the stage just before
the pixel shader. The GPU interpolates them across each triangle and gives them to the
pixel shader as its inputs. Each backend pairs outputs and inputs its own way:

- **Vulkan** numbers the values (their *location*) in the order they are declared, leaving
  out `SV_Position`, and pairs equal numbers.
- **D3D12** places the values in *registers*, in the order they are declared, `SV_Position`
  included, and pairs equal registers.

Neither pairs them by name. So the rules are:

1. **Use one struct.** Return it from the vertex shader and take the same struct in the pixel
   shader. This always works.
2. A pixel shader that declares its own inputs lists the **same values in the same order**,
   `SV_Position` included, and may leave out only values **at the end**.
3. Leaving out a value in the middle breaks both backends. If the vertex shader outputs `uv`
   then `normal` and the pixel shader takes only `normal`, Vulkan gives it the `uv` (silently:
   the image is just wrong) and D3D12 refuses the pipeline.

Values the GPU makes for the pixel shader (`SV_IsFrontFace`, `SV_SampleIndex`, ...) are not
outputs of the vertex shader and may be declared anywhere.

In validation builds, `createGraphicsPipeline` checks rules 2 and 3 on both backends
([P101](../design-notes/decisions.md#p101)), matching values by semantic:

```text
programmer error: createGraphicsPipeline: the pixel shader reads NORMAL0 at location 0, but the
vertex shader writes it at location 1: ...; pass them in one struct (P101)
```

One case is visible only on D3D12: a pixel shader that leaves out `SV_Position` at the start
while taking the values after it. Vulkan has no location for `SV_Position`, so it works there,
and D3D12 refuses it. The check relies on the names DXC gives the values in SPIR-V; shaders
from another compiler are not checked on Vulkan.

## What RI checks at pipeline creation

[`createComputePipeline`](../reference/pipeline.md) reads the shader and reports a readable
**programmer error** when:

- the bytecode is not SPIR-V (Vulkan backend) or not DXIL (D3D12 backend);
- there is no entry point `main`, or the shader is not a compute shader;
- the shader declares more than one root data block;
- the shader declares a resource at a register or binding instead of using the heap
  (`declared resources are not supported yet; reach resources through ResourceDescriptorHeap[]`,
  [P30](../design-notes/decisions.md#p30));
- on Vulkan, `SamplerDescriptorHeap` is at set 0, binding 1, DXC's default: the message tells
  you to add `-fvk-bind-sampler-heap 0 1`.

Root data larger than the device allows is an **unsupported** error
([Root data](root-data.md)).

## Related decisions

- [P4](../design-notes/decisions.md#p4): HLSL with one DXC for both targets.
- [P29](../design-notes/decisions.md#p29): RI consumes bytecode only.
- [P54](../design-notes/decisions.md#p54): the shader is the source of truth; one layout rule.
- [P67](../design-notes/decisions.md#p67): RI's own DXIL reader.
- [P93](../design-notes/decisions.md#p93): the pinned DXC and `equine_add_shaders`.
- [P94](../design-notes/decisions.md#p94): the sampler set rule.

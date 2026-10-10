# Compiling shaders for RI

## RI takes bytecode

RI does not compile shaders: you give it **bytecode**, SPIR-V on Vulkan and DXIL on D3D12
([P29](../design-notes/decisions.md#p29)). Shaders are written in HLSL and compiled with
**one DXC** for both targets ([P4](../design-notes/decisions.md#p4)). A helper for compiling
at run time will come later.

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

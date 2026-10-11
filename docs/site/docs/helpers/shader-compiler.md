# Shader compiler

## What it does

RI takes bytecode only ([P29](../design-notes/decisions.md#p29)). `<RI/Helpers/ShaderCompiler.hpp>` compiles HLSL at run
time, through the DXC library, with RI's compile rule ([P93](../design-notes/decisions.md#p93), [P94](../design-notes/decisions.md#p94)): SPIR-V
when RI is built for Vulkan, DXIL for D3D12. The same rule builds the shaders of RI's own
tests at build time.

- `createShaderCompiler(dxcLibrary)` loads `libdxcompiler.so` / `dxcompiler.dll`;
  `nullptr` loads the build's pinned DXC.
- `compileShader` takes the code, profile (`cs_6_6`, `vs_6_6`, `lib_6_8`, ...), entry,
  a name for messages, include directories and defines. RI's `<equine/root.hlsli>` is always
  found.
- The result holds the bytecode (empty on failure) and DXC's messages.

## Example

```cpp
--8<-- "snippets/ri/helpers.cpp:compiler"
```

## How to roll your own

Call `dxc` or the DXC library with RI's arguments, as `equine_add_shaders` does in
`cmake/EquineDXC.cmake`:

- both: `-T <profile> -E main -HV 2021 -I <RI's shaders directory>`;
- SPIR-V: `-spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout -fvk-bind-sampler-heap 0 1`;
- DXIL: no extra argument (RI's build adds `-Qstrip_reflect -Qstrip_debug` to show RI does
  not need reflection). Libraries (`lib_` profiles) compile to DXIL only, without `-E`.

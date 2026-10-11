# Use RI in your project

RI is meant to be pulled into another repository as a **git submodule** and added with
`add_subdirectory`. As a subproject, RI changes nothing global in your build: no compiler
flags rewritten, no clang-tidy, no sanitizers unless you ask for them.

## 1. Add the submodule

```text
git submodule add https://github.com/Raven33/EquineRI.git external/EquineRI
git submodule update --init --recursive    # RI's own submodules: Vulkan-Headers, volk, ...
```

## 2. CMake

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyApp LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)

# Pick the backend(s) before adding RI. D3D12 is Windows only.
set(EQUINE_VULKAN_BACKEND ON)
# set(EQUINE_D3D12_BACKEND ON)
# set(EQUINE_VALIDATION ON)   # native layers + RI's checks: use it in debug builds
add_subdirectory(external/EquineRI)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE EquineVulkanBackend EquineHelpersVulkan)

# Compile shaders with RI's rule (SPIR-V and DXIL) at build time.
equine_add_shaders(my_app PROFILE cs_6_6 SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fill.hlsl")
```

Code for one backend is compiled once per backend: to build both, make one executable per
backend (each links one backend library).

| Option | Default as a subproject | Meaning |
|---|---|---|
| `EQUINE_VULKAN_BACKEND`, `EQUINE_D3D12_BACKEND` | `OFF` | The backends to build. |
| `EQUINE_VALIDATION` | `OFF` | Native validation layers and RI's own checks ([validation](../concepts/validation.md)). |
| `EQUINE_BUILD_HELPERS` | `ON` | The [helpers](../helpers/index.md): `EquineHelpersVulkan` / `EquineHelpersD3D12`, `EquineGlfwWindow`. |
| `EQUINE_SANITIZERS` | `OFF` | AddressSanitizer + UBSan on RI's code; turn it on for your whole build too. |
| `EQUINE_CLANG_TIDY` | `OFF` | clang-tidy on RI's sources. |
| `EQUINE_BUILD_TESTS`, `EQUINE_BUILD_SAMPLES`, `EQUINE_BUILD_DOC_SNIPPETS` | `OFF` | RI's own tests, samples and doc snippets. |

## 3. Shaders

`equine_add_shaders(<target> PROFILE <profile> SOURCES <files>)` compiles each file with the
pinned DXC (downloaded at configure time) to `<build>/shaders/<name>.spv` and `.dxil`, and
defines `EQUINE_SHADER_DIR` on the target. Load the file of your backend and pass its bytes
to `createComputePipeline` / `createGraphicsPipeline`. Libraries (`lib_6_8`, work graphs)
compile to DXIL only. Shaders include `<equine/root.hlsli>` for `EQ_ROOT_DATA`
([compiling shaders](../concepts/shaders.md)). To compile at run time instead, use the
[shader compiler helper](../helpers/shader-compiler.md).

## 4. D3D12 executables: the Agility SDK

D3D12 loads RI's Agility SDK only when the **executable** exports two symbols. In exactly one
`.cpp` of each D3D12 executable, at namespace scope:

```cpp
#include <D3D12AgilityExports.hpp>
EQUINE_D3D12_AGILITY_SDK_EXPORTS()
```

and in CMake:

```cmake
equine_deploy_agility_sdk(my_app)   # copies the runtime to <exe dir>/D3D12/
```

## Next

[Your first compute dispatch](first-compute.md) walks through a complete program.

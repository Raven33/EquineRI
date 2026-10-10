# Build and test

This page takes you from a fresh clone to a passing test run. EquineRI builds with CMake and
Ninja, with clang on Linux (Vulkan backend) and clang-cl on Windows (Vulkan and D3D12
backends) ([P2](../design-notes/decisions.md#p2), [P14](../design-notes/decisions.md#p14)).

## 1. Install the tools

=== "Linux"

    - CMake 3.25 or newer, and Ninja.
    - clang and **clang-tidy** (every build runs clang-tidy, and configuring fails without it).
    - The Vulkan headers and loader (your distribution's `vulkan-headers` and
      `vulkan-icd-loader`, or the Vulkan SDK).
    - For debug builds: the Khronos validation layer (`vulkan-validation-layers`, or the
      Vulkan SDK). Debug builds stop with an *unsupported* error if it is missing.
    - A GPU driver with Vulkan 1.3, to run the GPU tests.

=== "Windows"

    - CMake 3.25 or newer, Ninja, and LLVM (clang-cl and clang-tidy).
    - The Vulkan SDK (headers, loader, and the validation layer for debug builds).
    - The Windows SDK. The **Agility SDK** (the newer D3D12 runtime) is downloaded by CMake
      at configure time, so you do not install it
      ([P55](../design-notes/decisions.md#p55)).
    - A GPU driver with Vulkan 1.3 and D3D12 shader model 6.6, to run the GPU tests.

Configuring also downloads one pinned DirectX Shader Compiler release, used to build the test
shaders ([P93](../design-notes/decisions.md#p93)). Both downloads are checked against a
SHA-256 hash, so the first configure needs network access.

## 2. Get the sources

The Vulkan function loader, volk, is a git submodule ([P89](../design-notes/decisions.md#p89)):

```text
git clone --recursive <repository URL>
```

In an existing clone, fetch it with `git submodule update --init`. Configuring stops with a
clear message if it is missing.

## 3. Configure and build with a preset

The presets in `CMakePresets.json` hold every setting, so one name is enough. Each preset
builds into `build/<preset>`.

=== "Linux"

    ```text
    cmake --preset clang-debug
    cmake --build --preset clang-debug
    ```

=== "Windows"

    ```text
    cmake --preset clangcl-debug
    cmake --build --preset clangcl-debug
    ```

Run these from a terminal where clang-cl is on the `PATH` (on Windows, a Visual Studio
developer prompt works).

| Preset (Linux / Windows) | Build type | Validation | Sanitizers | Tests |
|---|---|---|---|---|
| `clang-debug` / `clangcl-debug` | Debug | on | on | yes |
| `clang-release` / `clangcl-release` | Release | off | on | no |
| `clang-bench` / `clangcl-bench` | Release | off | **off** | yes |

- **Validation** (`EQUINE_VALIDATION`) turns on the native validation layers and RI's own
  checks; see [Validation](../concepts/validation.md).
- **Sanitizers** (AddressSanitizer and UndefinedBehaviorSanitizer) are on in every
  configuration except the `bench` presets, which exist only for measuring performance.

## 4. Run the tests

```text
ctest --preset clang-debug
```

(`clangcl-debug` on Windows.) The tests come in two groups:

- **GPU-free tests** (`ri.*`): the error channel, validation checks, and the like. They run
  anywhere, including machines without a GPU.
- **GPU tests** (`vk.*`, `d3d12.*`): they list GPUs and create devices on your real GPU.
  They carry the label `gpu`, so you can skip them:

```text
ctest --preset clang-debug -LE gpu
```

Some tests abort on purpose (for example, the one that checks what an error message looks
like). They still pass: CTest checks the message they print.

## 5. Use RI in your own code

Link the backend library and include one header:

```cmake
target_link_libraries(my_app PRIVATE EquineVulkanBackend)   # or EquineD3D12Backend
```

```cpp
--8<-- "snippets/ri/device.cpp:create-device"
```

`<RI/RI.hpp>` makes `RI::` name the backend you linked ([P90](../design-notes/decisions.md#p90)).
Code that should run on both backends is compiled twice, once per backend, in separate targets
([P83](../design-notes/decisions.md#p83)). A D3D12 executable also exports the Agility SDK
version; see [D3D12 device creation](../native/d3d12/device-creation.md).

## Optional: compile the documentation snippets

Every code example on this site is a complete source file under `docs/site/snippets/`. To
check that they all compile against your build:

```text
cmake --preset clang-debug -DEQUINE_BUILD_DOC_SNIPPETS=ON
cmake --build --preset clang-debug
```

The snippets are compiled, never run. D3D12 snippets are compiled only when the D3D12
backend is on (Windows). To build this site itself, see [Writing pages](../about/writing-pages.md).

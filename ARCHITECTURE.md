# EquineRI architecture

The entry point for anyone changing RI. It says where things are, how a call travels, which
rules every change keeps, how to make common changes, and where Vulkan and D3D12 differ
underneath. The standard this follows is `docs/CodeDocumentation.md`.

Using RI from another repository (a git submodule and `add_subdirectory`):
`docs/site/docs/getting-started/use-in-your-project.md`.

Other documents: `CONTEXT.md` (every decision, `P1`…, and why), `docs/Plan.md` (milestones),
`HANDOFF.md` (current state), the docs site `docs/site/` (for people using RI), and the
[Vulkan / D3D12 → RI glossary](docs/site/docs/concepts/glossary.md).

## 1. Layout

| Path | What it holds |
|---|---|
| `src/RI/` | The portable part, namespace `RI::`: descriptions, enums, flags (`Resources.hpp`, `Textures.hpp`, `Commands.hpp`, `Device.hpp`, `RayTracing.hpp`, `Window.hpp`), the error channel (`Error.*`), and `RI.hpp`, the one header user code includes. Library `EquineRI`. |
| `src/RI/Validation/` | RI's own checks (P88, P91): `Checks.*` (one function per rule), `ObjectTracker.*` (use after destroy), `Placement.*` (aliasing and discard tracking), `Validation.hpp` (`EQ_VALIDATE`). |
| `src/RI/Reflection/` | RI's DXIL reader (P67): root data size and bindings from DXIL. SPIR-V uses SPIRV-Reflect. |
| `src/types.hpp` | Integer aliases (`uint32`, …). |
| `backends/vulkan/` | Namespace `RI::Vk::`, library `EquineVulkanBackend`. `RI/Vk/Backend.hpp` declares the backend's structs and functions; `RI/Vk/Native.hpp` the native access (P84). One `.cpp` per area. `DeviceState.hpp` is the hidden per-device state. |
| `backends/d3d12/` | The same for D3D12: `RI::D3D12::`, `EquineD3D12Backend`, `RI/D3D12/Backend.hpp`, `RI/D3D12/Native.hpp`. Windows only. |
| `shaders/equine/root.hlsli` | `EQ_ROOT_DATA` (P45), included by every shader. |
| `helpers/` | Optional code built only on RI's public functions (P41): `core/` (allocators, transfers, frame ring, profiler, shader compiler; libraries `EquineHelpersVulkan` / `EquineHelpersD3D12`) and `glfw/` (`EquineGlfwWindow`). |
| `tests/` | `*.cpp` at the top: GPU-free tests (`EquineRITests`). `gpu/`: portable GPU tests, compiled once per backend. `vulkan/`, `d3d12/`: backend-only GPU tests (native access, backend quirks). `conformance/`: both backends declare the same signatures. `shaders/`: the tests' HLSL. |
| `samples/` | Small programs on RI. |
| `tools/layout_bench/` | The L1 measurement harness (P96, P97). |
| `cmake/` | The pinned DXC (`EquineDXC.cmake`, `equine_add_shaders`) and the Agility SDK (`EquineAgilitySDK.cmake`). |
| `third_party/` | Pinned submodules: Vulkan-Headers, volk, SPIRV-Reflect, GLFW. |
| `docs/site/` | The MkDocs site: `docs/` pages, `snippets/` (complete programs compiled with `-DEQUINE_BUILD_DOC_SNIPPETS=ON`), `scripts/` (header extraction, coverage, decision pages). |

## 2. How a call travels

User code includes `<RI/RI.hpp>` and links one backend library. That library defines
`EQUINE_BACKEND_VULKAN` or `EQUINE_BACKEND_D3D12`, and `RI.hpp` adds
`namespace RI { using namespace Vk; }` (or `D3D12`), so `RI::createBuffer` is
`RI::Vk::createBuffer` (P83, P90). Code for both backends is compiled once per backend.

Inside a backend function, in order:

1. **Liveness:** `EQ_VK_CHECK_ALIVE` / `EQ_DX_CHECK_ALIVE` checks the object's id against the
   device's `ObjectTracker` (validation builds only, P81, P91).
2. **RI's rules:** `EQ_VALIDATE(Validation::checkSomething(...))`. A check returns a message
   or nothing; `EQ_VALIDATE` reports a message as a programmer error and compiles to nothing
   (without evaluating its argument) when `EQUINE_VALIDATION` is off.
3. **Capability:** an unsupported feature calls `reportError(ErrorCategory::eUnsupported, …)`
   in every build (P11, P57).
4. **The native call**, written at once (recording is immediate, P34). Vulkan calls go
   through the device's volk table (`state.functions`, P89); D3D12 calls through COM
   interfaces. Failures go through `checkVk` / `checkHr`, which report device loss with
   its details (P53).

Errors all reach `RI::reportError` (`src/RI/Error.cpp`): it prints
`[EquineRI] <category>: <message>` with the source location and aborts (X1 is still open).

**Objects** are small copyable structs with public fields and a hidden `internal` struct of
native handles and ids (P81, P82). Per-device state that objects share (caches, RI's own CPU
heaps, the internal command list, the trackers) lives in `DeviceState`, reached through
`device.internal.state` or `cmd.internal.device`.

**Validation builds** (`EQUINE_VALIDATION`, the `*-debug` presets) also turn on the native
layers: Vulkan validation with GPU-AV, the D3D12 debug layer with GBV and DRED.
`EQUINE_GPU_VALIDATION=0` turns GPU-AV and GBV off at run time; `EQUINE_GPU=nvidia|amd|<n>`
picks the GPU tests run on.

## 3. Invariants

Every change keeps these. The decision behind each is in `CONTEXT.md`.

- **Both backends, same signatures.** A feature exists on Vulkan and D3D12 with identical
  declarations (checked by `tests/conformance/`), or it is reported unsupported where it
  cannot exist, behind a `DeviceLimits` flag (P11, P83).
- **Signatures change only with the user's agreement.** They should explain themselves;
  comments cover only what they cannot say (`docs/CodeDocumentation.md`).
- **No object-oriented style:** plain structs, public fields, free functions, hidden state
  in `internal`; no classes with private members, no methods (P82). Commands are
  `cmd*` functions (P85); descriptions use designated initializers (P86).
- **RI allocates no memory on its own** (P31). The user places every buffer, texture and
  scratch range. RI's own objects are small and fixed: CPU descriptor heaps, one internal
  command list, caches of native layouts, signatures and (Vulkan) samplers (P99).
- **RI infers only what is evident** (knowable without tracking past commands) and always
  keeps an explicit form (P36). Barriers are the user's, as coarse stage masks (P36).
- **Layouts are never visible** (P16). Textures rest in a layout chosen from their usage;
  RI switches only around its own operations (rendering sections, clears, copies, discard).
- **Shaders are the source of truth** for root data and bindings (P54); RI consumes bytecode
  only (P29). Every test and sample shader is compiled to SPIR-V and DXIL with one rule
  (P93, P94).
- **Native access stays possible** (P22) and is gated by the backend's `Native.hpp` (P84).
- **Checks go where native layers cannot see** (P88): RI's own rules only, in
  `src/RI/Validation/`, written once for both backends.
- **No frame concept, no deferred destruction:** the user waits on timeline points (P20).
  Convenience of that kind is a helper (P15, P41).
- **The layer above RI does not shape RI** (P80).
- **Performance-affecting choices are measured first** (P18).

## 4. Recipes

### Add a command or function to both backends

1. If it needs new portable types (descriptions, enums, flags), add them to the matching
   `src/RI/*.hpp`.
2. Declare it in `backends/vulkan/RI/Vk/Backend.hpp` and `backends/d3d12/RI/D3D12/Backend.hpp`
   with the same signature, next to its relatives, with a comment only where the signature
   cannot say it.
3. Implement it in the matching `.cpp` of each backend, in the order of section 2.
4. If a capability gates it, add a `DeviceLimits` field (`src/RI/Device.hpp`), set it in each
   backend's device creation, and report unsupported when it is false.
5. Add its signature to `tests/conformance/Conformance.cpp` (`EQ_SIGNATURE`).
6. Add a GPU test (next recipe), the docs page (the one after), and, if a choice was made, a
   decision row in `CONTEXT.md`.
7. Update the file headers and this document where they describe what changed.

### Add a validation check

1. Write the check in `src/RI/Validation/Checks.{hpp,cpp}`: a free function returning
   `CheckResult` (empty when the rule holds), with a message that names the call and the fix.
2. Call it from each backend with `EQ_VALIDATE(...)`.
3. Test it: a GPU-free case in `tests/ValidationTests.cpp` when the check needs no GPU, or a
   `*_reports` GPU test listed in `EQUINE_GPU_SHARED_ABORT_TESTS` (`tests/CMakeLists.txt`)
   with the expected message.

### Add a GPU test

1. Portable: add `EQ_TEST(group, name)` to a file in `tests/gpu/`; backend-only: in
   `tests/vulkan/DeviceTests.cpp` or `tests/d3d12/DeviceTests.cpp`.
2. Register the name in `tests/CMakeLists.txt` (`EQUINE_GPU_SHARED_TEST_NAMES`, or the
   backend's list). A `registry_matches` test fails if a case is not registered.
3. New shaders go in `tests/shaders/` and in `equine_add_gpu_test_shaders`.
4. Tests that need a feature return early when its `DeviceLimits` flag is false.

### Add a docs page

1. Write `docs/site/docs/reference/<name>.md` with `symbols:` front matter (the coverage
   report checks it), following `docs/site/docs/about/writing-pages.md`.
2. Put examples in `docs/site/snippets/`: complete programs, compiled per backend.
3. Add the page to `nav` in `docs/site/mkdocs.yml`.
4. Build strictly: `mkdocs build --strict -f docs/site/mkdocs.yml`, and compile the snippets
   (`-DEQUINE_BUILD_DOC_SNIPPETS=ON`, target `EquineDocSnippets`).

### Verify

1. Linux: `cmake --preset clang-debug`, build, `ctest --preset clang-debug` (Vulkan on the
   local GPU).
2. Windows (both backends, NVIDIA and AMD): the `windows-equineri` session builds
   `clangcl-debug` and runs the suite on each GPU on request; it pushes logs to a branch,
   never to master. D3D12 code is only compiled there.
3. Before committing: no warnings, all tests pass, the docs build strictly.

## 5. Where Vulkan and D3D12 differ underneath

What RI hides, and how each backend bridges it. The glossary maps names; this section maps
mechanisms.

| Area | Vulkan | D3D12 |
|---|---|---|
| Loading, device | volk, an instance per device, every feature the GPU reports turned on (P60, P89, P92) | Agility SDK 1.619, a DXGI factory per device (P55, P92) |
| Memory kinds (P32) | One memory type per kind, chosen at device creation, plus other types of the kind for resources that need them (`allocateMemory(..., &requirements)`) | One heap type per kind; resource heap tier 2 holds everything; CPU kinds mapped through one buffer over the whole heap |
| Descriptor heaps (P94, P99) | Two descriptor sets: set 0 one mutable binding for every resource view (as DXC's `ResourceDescriptorHeap`), set 1 samplers (a `VkSampler` cached per description); update-after-bind; heap size limited by what the driver accepts | Native shader-visible heaps, directly indexed |
| Root data (P44, P45) | Push constants, one range for all stages | Root constants at root parameter 0 (`b0, space999`) |
| Shaders (P93, P94, P67) | SPIR-V from DXC with `-fvk-use-dx-layout -fvk-bind-sampler-heap 0 1`; SPIRV-Reflect | DXIL; RI's own DXIL reader |
| Layouts (P95, P97) | Modes: unified (`GENERAL` everywhere), T (resting layouts per usage), G; RI switches around rendering, clears, copies | Enhanced barriers; textures rest in `COMMON` or their attachment layout |
| Barriers (P36) | One `VkMemoryBarrier2` from stage masks | One global barrier; attachment stages left to RI's switches |
| Rendering (P40) | Dynamic rendering; negative viewport height so clip space is y-up | `OMSetRenderTargets` with RTV/DSV in RI's CPU heaps |
| Clears (P65, P95) | Native clear commands | `Clear*View`, UAV clears through RI's small shader-visible heap, then the user's heaps and root signature again |
| Timeline (P20) | Timeline semaphore | Fence |
| Indirect (P48) | Level 1 native; Level 2 `VK_EXT_device_generated_commands` with the user's scratch | `ExecuteIndirect`, command signatures cached per root signature |
| Ray tracing (P49) | `VK_KHR_acceleration_structure` + ray query; acceleration structures must be a mutable descriptor type (off on AMD Windows) | DXR tier 1.1 |
| Work graphs (P50, P62) | Unsupported | State object, `SetProgram`, `DispatchGraph` |
| Swapchain (P43, P63, P64) | `VK_KHR_swapchain`; prerecorded layout switches per image | DXGI flip model; sRGB views on UNORM buffers |
| Debugging (P53) | Debug utils names and labels; device fault | `SetName`, PIX events; DRED |
| Validation (P21) | Validation layer, GPU-AV, printf | Debug layer, GBV |

## 6. Keeping this true

Every change updates the comments, file headers and sections of this document it touches. A
check run with the docs build fails when a source file has no header comment, or when a
comment names a decision that is not in `CONTEXT.md`.

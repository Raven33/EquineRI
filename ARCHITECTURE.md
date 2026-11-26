# EquineRI architecture

The entry point for anyone changing RI. It says where things are, how a call travels, which
rules every change keeps, how to make common changes, and where Vulkan, D3D12 and Metal differ
underneath. The standard this follows is `docs/CodeDocumentation.md`.

Using RI from another repository (a git submodule and `add_subdirectory`):
`docs/site/docs/getting-started/use-in-your-project.md`.

Other documents: `CONTEXT.md` (every decision, `P1`…, and why), `docs/Plan.md` (milestones),
`HANDOFF.md` (current state; the Metal backend's is `docs/MetalBackend-Handoff-2026-11-09.md`), the docs site `docs/site/` (for people using RI), and the
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
| `backends/metal/` | The same for Metal 4 (P113): `RI::Metal::`, `EquineMetalBackend` (option `EQUINE_METAL_BACKEND`), `RI/Metal/Backend.hpp`, `RI/Metal/Native.hpp`, C++ on metal-cpp. macOS on Apple silicon only. `Emulation.cpp` runs geometry and tessellation through Metal Shader Converter's emulation (P116 (1)). |
| `shaders/equine/root.hlsli` | `EQ_ROOT_DATA` (P45), included by every shader. |
| `helpers/` | Optional code built only on RI's public functions (P41): `core/` (allocators, transfers, frame ring, profiler, shader compiler; libraries `EquineHelpersVulkan` / `EquineHelpersD3D12` / `EquineHelpersMetal`). No window helper: GLFW is a test-only dependency (P109). |
| `tests/` | `*.cpp` at the top: GPU-free tests (`EquineRITests`). `gpu/`: portable GPU tests, compiled once per backend. `vulkan/`, `d3d12/`, `metal/`: backend-only GPU tests (native access, backend quirks). `conformance/`: every backend declares the same signatures. `shaders/`: the tests' HLSL. |
| `samples/` | Small programs on RI. |
| `tools/` | Measurement harnesses: `layout_bench/` (L1, P96, P97), `descriptor_bench/` (P94), `statistics_bench/` (statistics-query cost, P105), `metal_view_bench/` (the cost of Metal's pixel-format views, P74), `indirect_count_bench/` (Metal's GPU-written indirect counts against indirect command buffers, P116 (3)); and `metal_pack/`, the build step that packs Metal libraries and MSC's reflection into `.mtl.dxil` (P114). |
| `cmake/` | The pinned DXC (`EquineDXC.cmake`, `equine_add_shaders`), the Agility SDK (`EquineAgilitySDK.cmake`), and Metal Shader Converter from a local layout (`EquineMSC.cmake`, `EQUINE_MSC_DIR`; not redistributed, P113 (4)). |
| `third_party/` | Pinned submodules: Vulkan-Headers, volk, SPIRV-Reflect, GLFW, metal-cpp. |
| `docs/site/` | The MkDocs site: `docs/` pages, `snippets/` (complete programs compiled with `-DEQUINE_BUILD_DOC_SNIPPETS=ON`), `scripts/` (header extraction, coverage, decision pages). |

## 2. How a call travels

User code includes `<RI/RI.hpp>` and links one backend library. That library defines
`EQUINE_BACKEND_VULKAN`, `EQUINE_BACKEND_D3D12` or `EQUINE_BACKEND_METAL`, and `RI.hpp` adds
`namespace RI { using namespace Vk; }` (or `D3D12`, `Metal`), so `RI::createBuffer` is
`RI::Vk::createBuffer` (P83, P90). Code for every backend is compiled once per backend.

Inside a backend function, in order:

0. **Thread:** `EQ_VK_CHECK_THREAD` / `EQ_DX_CHECK_THREAD` / `EQ_MTL_CHECK_THREAD` checks that the call runs on the
   device thread, the one that called `createDevice` (validation builds only, P106). Every
   public function has it, except the any-thread list: pipeline and work-graph creation,
   `workGraphEntry`, `wait`, `isReached` and the pure queries.
1. **Liveness:** `EQ_VK_CHECK_ALIVE` / `EQ_DX_CHECK_ALIVE` / `EQ_MTL_CHECK_ALIVE` checks the object's id against the
   device's `ObjectTracker` (validation builds only, P81, P91).
2. **RI's rules:** `EQ_VALIDATE(Validation::checkSomething(...))`. A check returns a message
   or nothing; `EQ_VALIDATE` reports a message as a programmer error and compiles to nothing
   (without evaluating its argument) when `EQUINE_VALIDATION` is off.
3. **Capability:** an unsupported feature calls `reportError(ErrorCategory::eUnsupported, …)`
   in every build (P11, P57).
4. **The native call**, written at once (recording is immediate, P34). Vulkan calls go
   through the device's volk table (`state.functions`, P89); D3D12 calls through COM
   interfaces; Metal calls through metal-cpp. Failures go through `checkVk` / `checkHr`,
   which report device loss with its details (P53); on Metal, `reportNsError` for calls that
   return an `NSError`, and GPU errors arrive in commit feedback and are reported as device
   loss at the next `wait` or `isReached`.

Errors all reach `RI::reportError` (`src/RI/Error.cpp`): it prints
`[EquineRI] <category>: <message>` with the source location and aborts (X1 is still open).

**Objects** are small copyable structs with public fields and a hidden `internal` struct of
native handles and ids (P81, P82). Per-device state that objects share (caches, RI's own CPU
heaps, the internal command list, the trackers) lives in `DeviceState`, reached through
`device.internal.state` or `cmd.internal.device`.

**Validation builds** (`EQUINE_VALIDATION`, the `*-debug` presets) also turn on the native
layers: Vulkan validation with GPU-AV, the D3D12 debug layer with GBV and DRED. Metal's are
set by the environment, not by RI (P113 (5)): the `macos-debug` preset sets
`MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1` for the tests.
`EQUINE_GPU_VALIDATION=0` turns GPU-AV and GBV off at run time; `EQUINE_GPU=nvidia|amd|<n>`
picks the GPU tests run on.

## 3. Invariants

Every change keeps these. The decision behind each is in `CONTEXT.md`.

- **Every backend, same signatures.** A feature exists on Vulkan, D3D12 and Metal with
  identical declarations (checked by `tests/conformance/`), or it is reported unsupported where
  it cannot exist, behind a `DeviceLimits` flag (P11, P83). Behaviour that differs anyway is
  documented in that backend's `Backend.hpp` (Metal's divergences, P116).
- **Signatures change only with the user's agreement.** They should explain themselves;
  comments cover only what they cannot say (`docs/CodeDocumentation.md`).
- **No object-oriented style:** plain structs, public fields, free functions, hidden state
  in `internal`; no classes with private members, no methods (P82). Commands are
  `cmd*` functions (P85); descriptions use designated initializers (P86).
- **RI allocates no memory on its own** (P31). The user places every buffer, texture and
  scratch range. RI's own objects are small and fixed: CPU descriptor heaps, one internal
  command list, caches of native layouts, signatures and (Vulkan, Metal) samplers (P99). The
  one exception is command memory that a command list owns and recycles at reset (Metal, P115).
- **RI infers only what is evident** (knowable without tracking past commands) and always
  keeps an explicit form (P36). Barriers are the user's, as coarse stage masks (P36).
- **Layouts are never visible** (P16). Textures rest in a layout chosen from their usage;
  RI switches only around its own operations (rendering sections, clears, copies, discard).
- **Shaders are the source of truth** for root data and bindings (P54); RI consumes bytecode
  only (P29). Every test and sample shader is compiled to SPIR-V and DXIL with one rule
  (P93, P94).
- **Native access stays possible** (P22) and is gated by the backend's `Native.hpp` (P84).
- **Checks go where native layers cannot see** (P88): RI's own rules only, in
  `src/RI/Validation/`, written once for every backend.
- **One device thread** (P56, P106): RI objects are not thread-safe. Only the any-thread list
  runs elsewhere, and what it touches is either fixed after device creation or locked (the
  validation object tracker). A function added to the list must touch no other `DeviceState`.
- **Portable limits:** where the backends differ on a limit RI checks (buffer view elements,
  group counts), the rule is the smaller one on every backend, so a program tested on one
  backend never breaks on the other (P106).
- **No frame concept, no deferred destruction:** the user waits on timeline points (P20).
  Convenience of that kind is a helper (P15, P41).
- **The layer above RI does not shape RI** (P80).
- **Performance-affecting choices are measured first** (P18).

## 4. Recipes

### Add a command or function to every backend

1. If it needs new portable types (descriptions, enums, flags), add them to the matching
   `src/RI/*.hpp`.
2. Declare it in `backends/vulkan/RI/Vk/Backend.hpp`, `backends/d3d12/RI/D3D12/Backend.hpp` and
   `backends/metal/RI/Metal/Backend.hpp` with the same signature, next to its relatives, with a comment only where the signature
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
   `tests/vulkan/DeviceTests.cpp`, `tests/d3d12/DeviceTests.cpp` or a file of `tests/metal/`.
2. Register the name in `tests/CMakeLists.txt` (`EQUINE_GPU_SHARED_TEST_NAMES`, or the
   backend's list). A `registry_matches` test fails if a case is not registered. Metal runs
   every shared case except those in `EQUINE_METAL_SKIPPED_TESTS`, which are compiled out with
   `#if !EQUINE_BACKEND_METAL`.
3. New shaders go in `tests/shaders/` and in `equine_add_gpu_test_shaders`, and in the
   `EquineRIMetalTests` shader lists. A shader MSC cannot convert is marked `NO_METAL`.
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
3. macOS (Metal): `cmake --preset macos-debug`, build, `ctest --preset macos-debug`, on the
   borrowed Mac's M4 (`docs/MetalBackend-Handoff-2026-11-09.md` §2). Metal code is only compiled
   there; Vulkan and D3D12 code is not compiled on it.
4. Before committing: no warnings, all tests pass, the docs build strictly.

## 5. Where Vulkan, D3D12 and Metal differ underneath

What RI hides, and how each backend bridges it. The glossary maps names; this section maps
mechanisms. Metal's behaviour that RI does not hide (its divergences) is listed in
`backends/metal/RI/Metal/Backend.hpp`.

| Area | Vulkan | D3D12 | Metal |
|---|---|---|---|
| Loading, device | volk, an instance per device, every feature the GPU reports turned on (P60, P89, P92) | Agility SDK 1.619, a DXGI factory per device (P55, P92) | metal-cpp over Metal 4 (`MTL4*`, macOS 26+, Apple7+, P113); one `MTL4CommandQueue`; nothing is resident on its own: heaps, command memory, texture views and acceleration structures join the device's residency set |
| Memory kinds (P32) | One memory type per kind, chosen at device creation, plus other types of the kind for resources that need them (`allocateMemory(..., &requirements)`) | One heap type per kind; resource heap tier 2 holds everything; CPU kinds mapped through one buffer over the whole heap | One untracked placement heap per allocation, storage and cache mode from the kind; CPU kinds mapped through one buffer over the whole heap, as on D3D12; textures in eGPU memory only |
| Descriptor heaps (P94, P99) | Two descriptor sets: set 0 one mutable binding for every resource view (as DXC's `ResourceDescriptorHeap`), set 1 samplers (a `VkSampler` cached per description); update-after-bind; heap size limited by what the driver accepts | Native shader-visible heaps, directly indexed | Buffers of MSC's 24-byte `IRDescriptorTableEntry`, written by RI, at argument-table indices 0 and 1 (P114 (3)); samplers are `MTLSamplerState` objects cached per description, their resource ID in `gpuVA` |
| Root data (P44, P45) | Push constants, one range for all stages | Root constants at root parameter 0 (`b0, space999`) | One fixed root signature for every shader (64 root constants, P114 (1)); `cmdSetRootData` copies the bytes into command memory (Metal 4 has no `setBytes`, P115) and binds their address at argument-table index 2 |
| Shaders (P93, P94, P67) | SPIR-V from DXC with `-fvk-use-dx-layout -fvk-bind-sampler-heap 0 1 -fvk-support-nonzero-base-vertex` (P117); SPIRV-Reflect (also the values passed to the pixel shader, P101) | DXIL; RI's own DXIL reader (also the signatures, P101) | DXIL converted by Metal Shader Converter at build time, the metallib packed in the container as an `MTLB` part (`.mtl.dxil`); RI's DXIL reader; vertex shaders also get MSC's stage-in function and emulation reflection (`MTLS`, `MTLR`) for geometry and tessellation (P114) |
| Layouts (P95, P97) | Modes: unified (`GENERAL` everywhere), T (resting layouts per usage), G; RI switches around rendering, clears, copies | Enhanced barriers; textures rest in `COMMON` or their attachment layout | None: Metal has no layouts |
| Barriers (P36) | One `VkMemoryBarrier2` from stage masks | One global barrier; attachment stages left to RI's switches | Encoder barriers inside an encoder (one compute encoder for dispatches, copies and builds, one render encoder per section); queue barriers across encoders, carried to the next encoder when recorded with none open |
| Rendering (P40, P105) | Dynamic rendering; negative viewport height so clip space is y-up; a section without attachments is a zero-attachment section with the size as render area | `OMSetRenderTargets` with RTV/DSV in RI's CPU heaps; a section without attachments binds none, and the viewport and scissor bound it | One render encoder per section, its pass built from the attachments; occlusion queries through a visibility page of command memory per section (P115) |
| Texture shapes (P102) | Cubes: `CUBE_COMPATIBLE` images, a cube view to read and a second 2D-array view (`internal.arrayView`) to write and render; 3D: slices are depth, barriers take every array layer, render targets are 2D-array views of `2D_ARRAY_COMPATIBLE` images | Cubes are 2D arrays with cube SRVs; 3D: `TEXTURE3D`, one subresource per mip, RTVs pick `WSlice`s | Views are Metal texture views of the type the shader declares (array, cube, 3D, multisampled); whole cubes keep a 2D-array view for storage writes, as Vulkan |
| Clears (P65, P95) | Native clear commands | `Clear*View`, UAV clears through RI's small shader-visible heap, then the user's heaps and root signature again | Render targets and depth by a render pass of one load action per mip and layer; storage textures by blits from command memory |
| Timeline (P20) | Timeline semaphore | Fence | `MTLSharedEvent`, plus commit feedback (GPU errors) |
| Indirect (P48) | Level 1 native; Level 2 `VK_EXT_device_generated_commands` with the user's scratch | `ExecuteIndirect`, command signatures cached per root signature | Level 1 and Level 2 without a count need no GPU work (MSC's top-level argument buffer is a record's root data); counts through RI's built-in MSL kernel, and `maxCount` commands encoded on the CPU |
| Ray tracing (P49) | `VK_KHR_acceleration_structure` + ray query; acceleration structures must be a mutable descriptor type (off on AMD Windows) | DXR tier 1.1 | Metal 4 acceleration structures; `gpuAddress` is a bottom level's resource ID; `cmdBuildTlas` repacks 64-byte instances into Metal's 72-byte ones with a built-in kernel and writes MSC's header after the structure (P116 (4)) |
| Work graphs (P50, P62) | Unsupported | State object, `SetProgram`, `DispatchGraph` | Unsupported |
| Swapchain (P43, P63, P64) | `VK_KHR_swapchain`; prerecorded layout switches per image | DXGI flip model; back buffers rest in `COMMON` (= `PRESENT`), so acquire and present submit nothing; sRGB views on UNORM buffers | `CAMetalLayer` on the window's `NSView` (through the Objective-C runtime); `acquire` takes `nextDrawable` and makes the queue wait for it; `present` signals and presents the drawable |
| Debugging (P53, P106) | Debug utils names and labels; device fault; names given at creation go through the same `applyName` | `SetName`, PIX events; DRED; the same `applyName` for names at creation (views have none) | Labels; markers are debug groups on the command buffer (outside sections they end the open compute encoder); the same `applyName` |
| Validation (P21) | Validation layer, GPU-AV, printf | Debug layer, GBV | Metal API validation (`MTL_DEBUG_LAYER`) and shader validation (`MTL_SHADER_VALIDATION`), set by the environment (P113 (5)) |

## 6. Keeping this true

Every change updates the comments, file headers and sections of this document it touches. A
check run with the docs build fails when a source file has no header comment, or when a
comment names a decision that is not in `CONTEXT.md`.

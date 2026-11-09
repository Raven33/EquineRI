# Metal backend: handoff (2026-11-09)

State of branch `metal-backend` after stages 1 to 3 and ray tracing, and what the next session
does first. It replaces `MetalBackend-Handoff-2026-11-07.md` as the starting point; that file
stays as the stage-by-stage log (§5 to §10 there hold details and probe paths cited below).

Read in this order: `ARCHITECTURE.md`, `CONTEXT.md` (P113 to P116 are the Metal decisions),
`docs/FeatureProposal-Metal-2026-11-07.md` (the design, with the agreed answers to Q1 to Q8),
then this.

## 1. Where things stand

Branch `metal-backend`, 8 commits over `master`, **not pushed**:

| Commit | Content |
|---|---|
| `808498d` | the first handoff: macOS build state, decisions to settle |
| `eb983e8` | the proposal and decisions P113 to P116 (P31 amended for command memory) |
| `733b955` | stage 1: metal-cpp pinned, MSC in the build, `.mtl.dxil` shaders |
| `5ef72e6` | stage 2: device, queue, memory, buffers, descriptors, compute, readback |
| `9fbed79` | stage 3a: textures, views, samplers, texture copies and clears, timestamps |
| `dfabe46` | stage 3b-1: graphics and mesh pipelines, rendering sections, draws; 3a review fixes |
| `82f5bb7` | stage 3b-2: resolves, occlusion queries, indirect execution (Level 1 and 2, GPU counts) |
| `6b723ba` | inline ray tracing, `DeviceLimits::rayTracingHardware`, depth and stencil resolve modes |

Tests on the M4 (macOS 27): **113/113** with `ctest --preset macos-debug` (ASan, UBSan, RI
validation, `MTL_DEBUG_LAYER=1`, `MTL_SHADER_VALIDATION=1`), **99/99** in a build with RI
validation off. No compiler or clang-tidy warnings. Every portable GPU test runs on Metal except
`texture.occlusion_and_statistics` and `texture.mesh_statistics` (pipeline statistics do not
exist on Metal, P116 (1)), `helpers.shader_compiler` (plain DXIL; Metal takes `.mtl.dxil`, P114)
and the swapchain tests (not built). `texture.geometry_shader`, `texture.tessellation` and
`texture.cube_layered_render` pass by skipping (their limits are false on Metal).

**Not done on Metal:** swapchains and presentation, geometry and tessellation (MSC emulation,
later), work graphs (D3D12 only), pipeline statistics, the Metal 3 path (P113), native import
and native sections (P27, P70, P77). `backends/metal/Unsupported.cpp` lists what still reports
`eUnsupported`.

## 2. Environment (the borrowed Mac)

`~/dev/CLAUDE.md` holds the rules; the essentials:

- Work only in `/Users/arthur` and `/opt/homebrew`. Anything outside goes in
  `/Users/Shared/arthur-dev/manifest.log` with its removal in `uninstall.sh` there. No sudo:
  write a script there and the user runs it. Never touch `/Users/micheldelon`.
- `~/dev/tools/env.sh` (sourced by login shells; run `. ~/dev/tools/env.sh` in others) sets
  `DEVELOPER_DIR` (Xcode 27.2 in `~/Applications`), `SDKROOT`, `EQUINE_DXC_DIR` (DXC v1.9.2609
  built from source) and `EQUINE_MSC_DIR=~/dev/tools/msc` (Metal Shader Converter **4.0 beta 3**,
  extracted from Apple's pkg, not installed system-wide; CMake checks version 4.0.3).
- Build and test: `cmake --preset macos-debug && cmake --build --preset macos-debug && ctest
  --preset macos-debug`. The validation-off tree used for checks is in the session scratchpad
  (`build-noval`, configured with `-DEQUINE_VALIDATION=OFF` and `EQUINE_METAL_BACKEND=ON`); make
  a new one under `build/` if the scratchpad is gone.
- One case: `build/macos-debug/tests/EquineRIMetalTests <name>`, e.g. `texture.indirect_level1`
  (set `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` as the preset does).
- The user connects over Tailscale SSH and Michel may be using the screen: keep tests offscreen.
  Windowed tests need the `arthur` GUI session in front; ask first.
- Commit as `Raven33` (configured). Push only when asked.

## 3. Map of `backends/metal/`

| File | Owns |
|---|---|
| `DeviceState.hpp` | the internal contract: `DeviceState`, `CommandListState`, argument-table indices, `DescriptorEntry`, shared helpers, `EQ_MTL_*` macros |
| `Device.cpp` | device, queue, residency set, timeline, commit feedback (GPU errors), `DeviceLimits` |
| `Resources.cpp` | memory (placement heaps), buffers, descriptor heaps, `writeDescriptors`, samplers |
| `Textures.cpp` | formats, `formatSupported`, textures, views |
| `TextureCommands.cpp` | texture copies (staged through command memory when Metal refuses a direct copy), clears, discard |
| `Pipeline.cpp` | compute, graphics and mesh pipelines from `.mtl.dxil` |
| `Commands.cpp` | command lists, command memory, encoders, barriers, dispatch, submit |
| `Rendering.cpp` | sections, draws, dynamic state, resolves, visibility pages |
| `Indirect.cpp` | indirect execution, and **RI's built-in MSL** (count copy, instance repack) |
| `RayTracing.cpp` | acceleration structures, builds, the TLAS heap entry |
| `Queries.cpp` | timestamps, occlusion queries |
| `Debug.cpp` | names and markers |
| `Unsupported.cpp` | what still reports `eUnsupported` |
| `RI/Metal/Backend.hpp`, `Native.hpp` | the public types (same declarations as D3D12's) and native access |

Each file opens with an Owns / How / Watch out header; read it before changing the file.

## 4. Mechanisms worth knowing before touching anything

- **Shaders:** `equine_add_shaders` builds DXIL, runs MSC with RI's one fixed root signature
  (`Flags` must be the number 3072; the `"A | B"` string form crashes MSC 4.0b3), and packs the
  metallib into the DXIL container as an `MTLB` part (`<name>.mtl.dxil`). RI reads facts from the
  DXIL and the library from `findDxilPart`. `NO_METAL` skips shaders MSC refuses.
- **Binding:** argument-table indices 0 resource heap, 1 sampler heap, 2 root data (256 bytes),
  4 draw arguments, 5 index type. Heap entries are 24 bytes. Samplers put their resource ID in
  `gpuVA` (MSC's runtime header does; its manual says `textureViewID` and is wrong). A top
  level's entry points at MSC's 64-byte header.
- **Command memory (P115, amends P31):** shared-storage chunks per command list (64 KiB, then
  doubling, recycled at reset) for root data, draw arguments, visibility pages, staged copies
  and count copies. Metal 4 has no `setBytes`.
- **Encoders and barriers:** at most one encoder open; one compute encoder for dispatches,
  copies and builds; one render encoder per section. Barriers recorded with no encoder open
  are carried to the next one. Nothing inside a render pass can wait for its own earlier
  fragment work (validation builds report such a barrier).
- **Residency:** heaps, command memory, every texture view and every acceleration structure go
  in the residency set. With `MTL_SHADER_VALIDATION`, a texture view missing from it reads 0
  and drops writes **without any message**.
- **Built-in MSL:** one source string in `Indirect.cpp`, compiled once per device at first use
  (`builtinPipelines`). The count copy runs as a kernel outside sections and as a vertex
  function with rasterization off inside them; the repack is a kernel. The proposal planned an
  embedded metallib compiled at build time; runtime compile was simpler for small kernels.
- **Ray tracing:** `AccelerationStructure::gpuAddress` is the resource ID. `cmdBuildTlas`
  repacks 64-byte instances into Metal's 72-byte ones at the start of the scratch, then writes
  the header with a GPU copy into a buffer placed after the structure (eGPU memory is not CPU
  visible). `Stage::eAccelerationStructure` therefore also covers Dispatch and Blit on Metal.
- **Divergences from D3D12, documented in `Backend.hpp`:** `SV_VertexID` excludes the first
  vertex and vertex offset; indexed strips always restart; occlusion queries begin and end
  inside one section, one at a time, with the pool created before the section; indirect calls
  encode `maxCount` commands on the CPU.

## 5. Known gaps (what is unchecked)

- **No second review** of stage 3b-2 or ray tracing. Stages 3a and 3b-1 were reviewed by two
  agents each and their findings fixed.
- **Not compiled:** the D3D12 and Vulkan edits for `rayTracingHardware`
  (`backends/d3d12/Device.cpp`, `backends/vulkan/Device.cpp`), and the new portable test
  `texture.occlusion` on those backends. Check on Windows and Linux.
- **Untested on Metal:** indirect mesh draws, Level 2 dispatches with a count, many records;
  indexed or multiple BLAS geometries, several instances, instance masks, IDs and flags,
  `ePreferFastBuild`, TLAS refits; texture copies between different formats of one texel size;
  sampling a read-only depth attachment inside its section; `formatSupported` format by format
  (the table follows Apple's documents).
- **Open behaviour:** a pipeline whose stencil writes, in a read-only stencil section, is not
  reported (RI has no portable check). An indirect draw counts as a draw for timestamps even if
  every record is empty. The ICB variant of indirect counts (proposal §7 (a)) is not measured.
- **Other GPUs:** only the M4 was used. Whether M1 and M2 report `supportsRaytracing`, and the
  `rayTracingHardware` family check, are unverified. The GPU-error to `eDeviceLost` path has no
  test.
- **Small leftovers:** `cmdClearBuffer` does not reject eUpload or eReadback memory; the
  `writeDescriptors` usage check lives in the backend, not `src/RI/Validation`; `samplerMinMax`
  stays false (the debug layer refuses it on the M4).

## 6. Next steps, in order

1. **Review** stage 3b-2 and ray tracing (`Indirect.cpp`, `RayTracing.cpp`, the occlusion and
   resolve parts of `Queries.cpp` and `Rendering.cpp`), and add tests for the untested cases in
   §5 that need no new feature (indirect mesh draws, Level 2 dispatch counts, instance masks
   and IDs: extend `ray_query.hlsl` or add a shader).
2. **Swapchains** on `CAMetalLayer`: `surfaceFormats`, present modes, `createSwapchain`,
   acquire and present on the MTL4 queue. `window.swapchain` uses GLFW and skips without a
   window server, so a real run needs the `arthur` GUI session in front (ask the user).
3. **Push** `metal-backend` and run the D3D12 and Vulkan builds on their platforms (the
   `rayTracingHardware` and `texture.occlusion` changes), when the user asks.
4. Later, per P113 and P116: geometry and tessellation through MSC's emulation, the Metal 3
   path, measuring indirect counts against an ICB, native import.

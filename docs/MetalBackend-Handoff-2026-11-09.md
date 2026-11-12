# Metal backend: handoff (2026-11-09)

State of branch `metal-backend` after stages 1 to 3 and ray tracing, and what the next session
does first. It replaces `MetalBackend-Handoff-2026-11-07.md` as the starting point; that file
stays as the stage-by-stage log (§5 to §10 there hold details and probe paths cited below).

Read in this order: `ARCHITECTURE.md`, `CONTEXT.md` (P113 to P116 are the Metal decisions),
`docs/FeatureProposal-Metal-2026-11-07.md` (the design, with the agreed answers to Q1 to Q8),
then this.

## 1. Where things stand

Branch `metal-backend`, 17 commits over `master` at `aa641ce`, all pushed (`origin/metal-backend`
is at `aa641ce`, checked 2026-11-12). The feature commits:

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

Tests on the M4 (macOS 27), after the review fixes, the probe tests and swapchains: **126/126** with `ctest
--preset macos-debug` (ASan, UBSan, RI validation, `MTL_DEBUG_LAYER=1`, `MTL_SHADER_VALIDATION=1`),
**110/110** in a build with RI validation off (`build/noval`). No compiler or clang-tidy
warnings. Every portable GPU test runs on Metal except `texture.occlusion_and_statistics` and `texture.mesh_statistics` (pipeline statistics do not
exist on Metal, P116 (1)), `helpers.shader_compiler` (plain DXIL; Metal takes `.mtl.dxil`, P114)
and `window.swapchain_x11`/`_wayland` (Linux only). `texture.geometry_shader`, `texture.tessellation` and
`texture.cube_layered_render` pass by skipping (their limits are false on Metal).

**Not done on Metal:** geometry and tessellation (MSC emulation,
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
  --preset macos-debug`. The validation-off tree used for checks is `build/noval`
  (`cmake --preset macos-debug -B build/noval -DEQUINE_VALIDATION=OFF -DEQUINE_METAL_BACKEND=ON`;
  run its `ctest` with `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`).
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
- **Divergences from D3D12, documented in `Backend.hpp`:** indexed strips always restart;
  occlusion queries begin and end inside one section, one at a time, with the pool created
  before the section; indirect calls encode `maxCount` commands on the CPU. `SV_VertexID`
  without the first vertex and vertex offset is **not** a divergence: it is D3D12's rule and,
  since P117 (master, 2026-11-12), RI's rule on every backend.
- **Decided 2026-11-12 (user), to do when master and P112 are merged in:** add
  `texture.vertex_id_excludes_offsets` (P117's test, on master) to `EQUINE_METAL_TEST_NAMES`
  and run it; bindful constant buffers stay unsupported on Metal (P116 (6)):
  `DeviceLimits::constantBuffers` false, `cmdSetConstantBuffer` reporting `eUnsupported`,
  and the declared-resource message of `reflectStage` naming the flag for space 998.

## 5. Known gaps (what is unchecked)

- **Reviewed 2026-11-09** (the whole backend, with probes): see §7 for the bugs found; all five
  are fixed, each with a registered test (§7.1). Stages 3a and 3b-1 had been reviewed by two agents each and their findings fixed.
- **Checked on Windows (2026-11-09, at `15b79e1`):** 229/229 with `clangcl-debug` (D3D12 and
  Vulkan tests, validation on), on the RTX 3080 and on the AMD iGPU, with no new warnings
  (clang-tidy not run there). This covers the `rayTracingHardware` edits, the
  `WindowSystem::eCocoa` case and every new portable test. `texture.indirect_mesh` skips on
  the AMD iGPU (D3D12 MeshShaderTier 0). Two test fixes came out of it (`19df9c3`,
  `15b79e1`): upload arenas sized for D3D12's 64 KiB buffer alignment, and
  `compute.int64_shared_atomics_reports` compiled on Metal only.
- **Checked on Linux (2026-11-12, at `aa641ce`):** 140/140 with `clang-debug` (Vulkan, validation
  on) on the Radeon 680M (RADV), `window.swapchain_x11` and `_wayland` included. From that run:
  `texture.layered_vertex_output` was compiled out on Vulkan on the claim that RI does not turn
  on `shaderOutputLayer`; RI turns on every Vulkan 1.2 feature the GPU has, and the test passes
  on RADV, so it is now a shared test (141/141 with it; not yet run on Windows Vulkan).
- **Probed but not registered:** `formatSupported` format by format (M4 only; the sweep is one
  process per case). Every other probe of §7.3 is now a registered test (§7.4).
- **Untested on Metal:** texture copies between different formats of one texel size; sampling
  a read-only depth attachment inside its section.
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

1. ~~Fix the review's bugs and register the probes~~: done 2026-11-09 (§7.1, §7.4).
2. ~~**Swapchains** on `CAMetalLayer`~~: done 2026-11-09 (§8). Before that, the plan was:
   `surfaceFormats`, present modes, `createSwapchain`,
   acquire and present on the MTL4 queue. `window.swapchain` uses GLFW and skips without a
   window server, so a real run needs the `arthur` GUI session in front (ask the user).
3. **Push** `metal-backend` and run the D3D12 and Vulkan builds on their platforms (the
   `rayTracingHardware` and `texture.occlusion` changes), when the user asks.
4. Later, per P113 and P116: geometry and tessellation through MSC's emulation, the Metal 3
   path, measuring indirect counts against an ICB, native import.

## 7. Review (2026-11-09)

The whole backend was read (all of `backends/metal/`, 5,800 lines), every public function was
mapped to the registered Metal tests that reach it, and what no test reaches was probed on the M4
with `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`. At `708ebd7`: 113/113 with the preset, no
warnings. Nothing was changed in the repository. The probes (not part of the repository) are in
`/private/tmp/claude-502/-Users-arthur-dev-EquineRI/5e5705f3-dabf-4450-a2d3-a97354f73767/scratchpad/probe/`
(`Probe.cpp`, `Probe2.cpp`, shaders in `src/`, built by `mkshaders.sh`, `build.sh` and
`build2.sh`; `sweep.log` holds the format sweep).

### 7.1 Bugs found (all fixed 2026-11-09)

Fixes and tests: (1) pages from fixed 256 KiB chunks of their own (`allocateVisibilityPage`,
`Commands.cpp`; `allocateCommandMemory` lost its `limit`), tests `metal.visibility_pages_bounded`
(residency-set size after 64 sections, twice) and `texture.occlusion_many_sections` (portable);
(2) `int64SharedAtomics` false, the user's decision, P116 (2) amended, test
`compute.int64_shared_atomics_reports` (shader `atomic64_shared.hlsl`); (3) one 72-byte record
kept for 0 instances (`instanceRecords`, `RayTracing.cpp`), test `raytracing.empty_top_level`
(portable); (4) `native(const QueryPool &)` returns `NativeQueryPool{counters, results}`, and
`applyName` labels an occlusion pool's buffer as a buffer, test `metal.native_access`; (5) both
texts updated. Tests (1), (2) and (3) fail on the unfixed backend.

1. **Visibility pages make command memory grow exponentially.** While any occlusion pool exists,
   each section takes a 32 KiB page that must end within the first 256 KiB of a chunk
   (`attachVisibilityPage`, `Rendering.cpp`); when the chunk is full, `allocateCommandMemory`
   (`Commands.cpp`) adds a chunk twice the last one's size, of which pages can again use only
   256 KiB. The chunk size doubles about every 8 sections. Probed with one pool and empty
   sections in one list: 8 sections 3 chunks (largest 256 KiB); 32 sections 6 chunks (2 MiB,
   3 MiB in all); 64 sections 10 chunks (32 MiB, 63 MiB in all). Resets keep the chunks. Fix:
   pages from their own fixed 256 KiB chunks (8 pages each), or no doubling when the miss came
   from `limit`. Test: many sections in one list with a live pool, command memory bounded.
2. **64-bit groupshared atomics do not compile on Metal**, yet `DeviceLimits::int64SharedAtomics`
   is true (`Device.cpp`, P116 (2)). MSC 4.0b3 converts the shaders, then
   `newComputePipelineState` fails: "Compilation failed due to an interrupted connection:
   XPC_ERROR_CONNECTION_INTERRUPTED". Every operation fails (add, max, min, or, exchange,
   compare-exchange, one shader each), with validation off too, and with MSC's
   `--minimum-gpu-family=Apple9`. A plain 64-bit groupshared variable and a 32-bit groupshared
   atomic work. No registered test covers it (`atomic64.hlsl` is `NO_METAL` and also uses
   buffer atomics). Making the flag false amends P116 (2): the user decides.
3. **A top level of 0 instances aborts under the debug layer**: "instance descriptor buffer
   length (0) must not be 0" (`tlasDescriptor`, `RayTracing.cpp`, gives a range of
   `72 x instanceCount` bytes). Without the debug layer it builds and every ray misses, as on
   D3D12. Fix: a range of one 72-byte record (in the scratch's instance area, which then needs
   `instanceArea(max(count, 1))`) while the count stays 0.
4. **`native(const QueryPool &)` returns an occlusion pool's buffer as an `MTL4::CounterHeap *`**
   (`Native.hpp`; `internal.heap` holds an `MTL::Buffer` for occlusion pools). Needs a separate
   accessor or a variant; `Backend.hpp`'s comment on `internal.heap` says timestamps only.
5. **Stale text:** `Resources.cpp`'s header says acceleration-structure views come later;
   `notYet`'s message (`Unsupported.cpp`) names stages 2 to 3b-2 and not ray tracing.

### 7.2 Smaller observations

- `cmdBeginMarker` / `cmdEndMarker` outside sections end the open compute encoder each time
  (`Debug.cpp`), so markers split encoders (a cost, not a bug).
- Every section pays a 32 KiB visibility page while any occlusion pool exists, even without
  queries (inherent to Metal taking the buffer at pass creation; bounded once 7.1 (1) is fixed).

### 7.3 Probed and correct (M4)

- **Native access:** every accessor returns RI's object (`native(buffer)->gpuAddress()` matches).
- **Point lists** without a geometry shader: 256 points light 256 pixels, as on D3D12.
- **Layered rendering** (`SV_RenderTargetArrayIndex` from the vertex shader, 2 layers): right in
  both layers, single-sample and 4x with the resolve at `cmdEndRendering` (resolve slice 0 with
  `renderTargetArrayLength` resolves every layer).
- **Indirect:** Level 2 dispatches, 100 records with a GPU count of 37: all 100 slots right;
  Level 2 draws with 36-byte records (root data at 4-byte-aligned addresses), with and without
  a count and from offset 36; mesh draws, Level 2 with counts 1 and 0, and Level 1; the count
  copy inside a 4x multisampled section.
- **Ray tracing:** two geometries (one indexed with 16-bit indices and not opaque, committed
  through `CommitNonOpaqueTriangleHit`), two instances: instance IDs and indices, geometry and
  primitive indices, instance masks against the ray mask, the counter-clockwise instance flag
  (front faces flip as on D3D12), `ePreferFastBuild`, a top-level refit moving an instance.
- **`formatSupported`:** each of the 135 format, usage and sample-count combinations it accepts
  was created and used without a debug-layer message: storage textures read back the expected
  value after an `RWTexture2D` load + 0.5 + store (unorm8, RGB10A2, R11G11B10, half, float;
  uint + 5), 28 multisampled color resolves including the 32-bit float formats, clears of the
  rest. M4 only: Apple7 and Apple8 may allow fewer read-write formats.

### 7.4 Test coverage

Every function of `Backend.hpp` is reached by a registered Metal test except swapchains (since
built, §8), work
graphs (expected) and `native()`. Reached by one test only: the ray-tracing functions,
`cmdDrawIndexed`, `cmdDrawMesh`, `cmdWriteTimestamp`, `cmdSetScissor`, each indirect entry point.

**Since then (2026-11-09):** the probes are registered tests: `metal.native_access` (every
`native()` accessor), `texture.indirect_dispatch_count` (Level 2 dispatches, 100 records, GPU
count 37), `texture.indirect_draw_records` (36-byte Level 2 records with and without a count and
from an offset; Level 1 with a count in a 4x section with a resolve), `texture.indirect_mesh`
(Level 2 counts 1 and 0, Level 1), `texture.point_list`, `texture.layered_vertex_output` (2
layers from the vertex shader, 1x and 4x with resolve) and `raytracing.scene` (two geometries,
one indexed and not opaque; two instances with IDs, masks and the counter-clockwise flag;
`ePreferFastBuild`; a top-level refit). New shaders: `indirect_write`, `grid_points_vs`,
`layer_vs`, `layer_ps`, `ray_query_scene`. The ray-tracing and indirect functions are now
reached by two tests or more.

## 8. Swapchains (2026-11-09)

`backends/metal/Swapchain.cpp`. `RI::Window` gained `WindowSystem::eCocoa` (`handle` is the
`NSView*`; P43 amended by the assistant, for the user's review). `createSwapchain` gives the
view a `CAMetalLayer` (or keeps the one it has) through the Objective-C runtime (`objc_msgSend`;
metal-cpp wraps no AppKit), not framebuffer-only, 2 or 3 drawables (3 by default),
`displaySyncEnabled` for `eFifo`, no `eMailbox`; the layer's residency set joins the queue.
`acquire` takes `nextDrawable` (retried on its 1 s timeout) and calls
`MTL4::CommandQueue::wait(drawable)`; `present` calls `signalDrawable` then `present`. Each
drawable texture is wrapped once as an `RI::Texture` (the texture is its own view) and kept
until resize or destroy, which wait for the queue. Formats: BGRA8 sRGB and UNORM, RGB10A2 (sRGB
color space), RGBA16Float (scRGB), and RGB10A2 HDR10 when the screen has EDR headroom. Storage:
every listed format but `eBGRA8Srgb`.

Tests: `window.swapchain` (the portable test, now compiled on macOS with `glfwGetCocoaView`) and
`metal.screen_contents` (`tests/metal/SwapchainTests.cpp`: a clear and a compute write to the
screen texture, each read back with a native blit before present; capabilities; resize). Both
pass 10 runs in a row. **Over SSH there is no GUI session and no display**
(`CGSessionCopyCurrentDictionary` is null), yet GLFW opens the window and the layer still gives
drawables: the tests run and nothing shows on Michel's screen. Not seen yet: a real on-screen
run in the `arthur` GUI session, and HDR10.


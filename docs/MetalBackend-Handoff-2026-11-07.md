# Metal backend: handoff (2026-11-07)

> Superseded as the starting point by `MetalBackend-Handoff-2026-11-09.md`; this file stays as
> the stage-by-stage log (§5 to §10).

State of branch `metal-backend` and what the next session does first. Read `ARCHITECTURE.md`,
then `CONTEXT.md`, then this. Nothing about Metal is decided yet: `CONTEXT.md` has no Metal
decision, and the invariants in `ARCHITECTURE.md` §3 were written for two backends.

## 1. What exists

- **RI builds and its GPU-free tests pass on macOS** (Apple M4, macOS 27, Xcode 27.2,
  AppleClang 21): `cmake --preset macos-debug`, `cmake --build --preset macos-debug`,
  `ctest --preset macos-debug`: 45/45 with ASan and UBSan, clang-tidy clean (commit `3e3547e`).
- **DXC on macOS:** Microsoft ships no macOS DXC, so `cmake/EquineDXC.cmake` takes
  `EQUINE_DXC_DIR`, a local build of the pinned `v1.9.2609` laid out like the Linux release
  (`bin/dxc`, `lib/libdxcompiler.dylib`, `include/dxc`); the presets read it from the
  environment. DXIL built on macOS is unsigned (no libdxil), which matters only on D3D12.
  Building DXC: `cmake -C cmake/caches/PredefinedParams.cmake` with tests off, and
  `-DCLANG_FORMAT_EXE=CLANG_FORMAT_EXE-NOTFOUND` when the `clang-format` on `PATH` is not the
  version DXC's generated headers were formatted with (the source-generation check fails on
  line wrapping otherwise).
- **clang-tidy on macOS:** a standalone LLVM clang-tidy needs `-isysroot <SDK>` and
  `-stdlib++-isystem <SDK>/usr/include/c++/v1` (AppleClang's `/usr/bin/c++` adds the sysroot
  implicitly, so `compile_commands.json` carries none). On the dev Mac a wrapper on `PATH` adds
  them; RI's CMake is unchanged.
- **The Metal compiler** (`xcrun metal`, Metal Toolchain 27B47) is installed. **Metal Shader
  Converter is not**: it is a separate Apple download, needed if shaders go DXIL → Metal IR.
- No Metal code yet: no `backends/metal/`, no `EQUINE_METAL_BACKEND` option.

## 2. Decisions to settle with the user before code

Each becomes a decision row in `CONTEXT.md` (next free number after the open branches
`p110`–`p112`; check). Signatures change only with the user's agreement (§3), so a Metal
backend that cannot match a signature reports unsupported behind a `DeviceLimits` flag, as
D3D12 does for Vulkan-only features and the reverse.

1. **Metal 4 only, or Metal 3 too.** Metal 4 (`MTL4CommandBuffer`, command allocators,
   argument tables, explicit residency sets, explicit barriers, `MTL4Compiler`) maps onto
   RI's model (explicit barriers P36, user-placed memory P31, immediate recording P34) far
   better than Metal 3's hazard tracking. The dev Mac (M4, macOS 27) supports it. Recommended:
   Metal 4 only, Apple silicon only; measure before deciding otherwise (P18).
2. **Shader path** (P29, P54, P93: RI consumes bytecode, shaders are the source of truth).
   (a) DXIL → Metal Shader Converter → `.metallib`: keeps DXIL semantics, gives
   `ResourceDescriptorHeap`-style bindless through its top-level argument buffer, and has a
   reflection JSON; Apple-only tool, closed source. (b) SPIR-V → SPIRV-Cross → MSL → `metal`:
   open, but adds a translation RI does not control. Either way `equine_add_shaders` gains a
   third output, and RI reads root data and bindings from DXIL (P67) or SPIR-V as now.
3. **Backend namespace and library** following P83/P90: `RI::Metal::`, `EquineMetalBackend`,
   `EQUINE_BACKEND_METAL`, `backends/metal/RI/Metal/{Backend,Native}.hpp`. Language:
   Objective-C++ (`.mm`) or C++ with `metal-cpp`; P82's style (plain structs, free functions)
   holds either way.
4. **The mapping, area by area** (`ARCHITECTURE.md` §5 gets a third column):

   | Area | Likely Metal mechanism | Question |
   |---|---|---|
   | Memory kinds (P32) | `MTLHeap` with placement (`MTLHeapTypePlacement`) | Which kinds map to which storage modes on unified memory |
   | Descriptor heaps (P94, P99) | Argument buffers (tier 2) as the resource and sampler heaps | Layout must match the shader path's (MSC's is fixed) |
   | Root data (P44, P45) | Argument table bytes / `setBytes` equivalent | 256-byte cap (`kMaxRootDataSize`) fits |
   | Barriers (P36) | Metal 4 stage barriers | How `Stage::*` maps; RI's own layout switches have no Metal meaning (no layouts, P16) |
   | Timeline (P20) | `MTLSharedEvent` | Direct match |
   | Rendering (P40, P105) | Render pass descriptors, render encoders | Encoder boundaries vs RI's `cmdBeginRendering` sections |
   | Geometry, tessellation (P102) | No geometry shaders; tessellation is post-tessellation vertex functions | Likely unsupported behind `DeviceLimits` |
   | Mesh shaders | Object and mesh functions | Payload and limits |
   | Ray tracing (P49) | Acceleration structures, intersection queries | Ray query only, as now |
   | Work graphs, DGC (P48, P50) | Indirect command buffers (Level 2?); no work graphs | Unsupported or partial |
   | Swapchain (P43) | `CAMetalLayer`, drawables | The tests' GLFW window (P109) gives an `NSWindow` |
   | Validation (P21) | `MTL_DEBUG_LAYER`, shader validation (`MTL_SHADER_VALIDATION`) | Turned on through the environment at device creation, or documented |
   | Debugging (P53) | Labels, debug groups, GPU capture | `applyName` equivalent |

5. **Verification and sessions.** A macOS session builds and runs the suite on the Mac, as
   the `windows-ri` and `linux-ri` sessions do for theirs; logs go to
   `docs/measurements/macos/verify/<commit>/`, never to master. `tests/conformance/` gains
   the third backend.

## 3. Suggested first steps (after §2.1–§2.3 are agreed)

1. Write `docs/FeatureProposal-Metal-<date>.md` with the §2 answers, for the user's review.
2. Add `EQUINE_METAL_BACKEND` (Apple only), `backends/metal/` with device creation, a
   buffer, a compute pipeline from one test shader, dispatch and readback:
   `samples/compute_readback` and `tests/gpu/ComputeTests.cpp` are the target, the same path
   Milestone 2 took (`docs/Plan.md`).
3. Extend `equine_add_shaders` for the chosen shader path, then `tests/conformance/`.
4. Then area by area, in the order of `docs/Plan.md`'s milestones.

## 4. User's answers (2026-11-07, before the proposal)

Not yet decision rows: they go into `docs/FeatureProposal-Metal-2026-11-07.md` for review, then
`CONTEXT.md` as P113 onwards (`p110`–`p112` are taken by open branches).

- **API:** Metal 4 first, Apple silicon (M1+); a Metal 3 path later, so keep it possible.
- **Platform:** macOS only for now.
- **Shaders:** DXIL → Metal Shader Converter → `.metallib`. MSC is extracted into `~/dev/tools`
  (`pkgutil --expand-full`), not installed system-wide. Its download needs an Apple ID, so the
  user fetches the pkg. Pinned: **MSC 4.0 beta 3** (`metal-irconverter` 4.0.3, pkg SHA-256
  `449833211e4ef0d46bfaa94848e801acff49e226d3c348bba0673f13716be622`), the user's choice over
  the last stable 3.x (2025-12-05); laid out in `~/dev/tools/msc/{bin,include,lib,docs}`.
- **Language:** C++ with metal-cpp (pinned `release/metal-cpp_macOS27.0.1_iOS27.0.1`, on GitHub,
  so a submodule like Vulkan-Headers; it has all 30 `MTL4*` headers). Objective-C++ only to fill
  an API that metal-cpp lacks. `Native.hpp` exposes `MTL::` pointers.
- **Naming:** as §2.3 (`RI::Metal::`, `EquineMetalBackend`, `EQUINE_BACKEND_METAL`,
  `EQUINE_METAL_BACKEND`).
- **Validation:** documented only. RI sets no environment; presets or ctest set
  `MTL_DEBUG_LAYER` and `MTL_SHADER_VALIDATION`.
- **Geometry, tessellation:** unsupported (`DeviceLimits` false) for now; emulation revisited
  later.
- **Indirect Level 2 (P48):** the proposal investigates a mapping onto indirect command buffers.
- **Ray queries:** supported on every Apple-silicon GPU, plus a fact that says whether they are
  hardware-accelerated (M3+).
- **Swapchain tests:** skipped when no usable window server (GUI session) is available; the
  other GPU tests run offscreen.
- **Next:** check MSC's argument-buffer layout and root constants against P94/P99 and the
  256-byte root data, then write the proposal and stop for review.

## 5. Stage 1 done (2026-11-08): pins and build

- metal-cpp submodule at `third_party/metal-cpp` (`8cdde68`); `backends/metal` builds
  `EquineMetalCpp` (the private implementation, once) under `EQUINE_METAL_BACKEND`.
- `cmake/EquineMSC.cmake`: `EQUINE_MSC_DIR` (presets read the environment; `~/dev/tools/env.sh`
  exports it), version 4.0.3 checked at configure time, the fixed root signature written to
  `<build>/equine/msc_root_signature.json`, and the `equine_metal_pack` tool
  (`tools/metal_pack/MetalPack.cpp`) that appends the `MTLB` part.
- `equine_add_shaders` writes `<name>.mtl.dxil` (metallibs in `<build>/shaders/metal/`), with
  `NO_METAL` for the misuse inputs and `atomic64`.
- `macos-debug` turns on the Metal backend and sets `MTL_DEBUG_LAYER` and
  `MTL_SHADER_VALIDATION` for ctest. GPU-free suite: 46/46, with the new `dxil.metal_container`.
- **Next (stage 2):** `EquineMetalBackend` with device, memory, buffers, heaps, a compute
  pipeline from `.mtl.dxil`, dispatch and readback (`samples/compute_readback`,
  `tests/gpu/ComputeTests.cpp`), then `tests/conformance/` with the third backend.

## 6. Stage 2 done (2026-11-08): compute on Metal

- `EquineMetalBackend` (`backends/metal/`): device and GPU list (Metal 4 + Apple7), placement
  heaps and residency, buffers, descriptor heaps (buffer views), compute pipelines from
  `.mtl.dxil`, command lists with command memory (P115), root data, dispatch, barriers
  (encoder and queue barriers, carried across encoders), buffer copies, clears, small writes,
  markers, timestamp query pools, submit and the timeline (commit feedback waited for, so a
  GPU error is reported by the `wait` of its submission). Everything else reports
  `unsupported: ... not built yet on the Metal backend`; capability probes return false.
- `RI.hpp` selects it with `EQUINE_BACKEND_METAL`; helpers, the sample and conformance build for
  Metal. `EquineRIMetalTests`: the compute tests and `threads.call_from_other_thread_reports`.
- macOS: 63/63 (validation on, preset), validation off 54/54 with samples, sample
  `compute_readback_Metal` 0 of 4096 wrong with validation on and off; the Metal debug layer
  and shader validation report nothing.
- Known gaps: the GPU-error → eDeviceLost path is untested (no way found to fault the M4);
  cmdClearBuffer does not reject eUpload/eReadback memory (Buffer has no memory kind on Metal);
  the usage check in `writeDescriptors` is a static check in Resources.cpp, to move into
  `src/RI/Validation` for every backend; `DeviceState` has no ray-tracing flag yet.
- **Next (stage 3):** textures, views and samplers, then rendering (`docs/Plan.md` order), with
  `names_at_creation` and the texture tests joining `EquineRIMetalTests`.

## 7. Stage 3a done (2026-11-08): textures without rendering

- `backends/metal/Textures.cpp`: the format table (RI `Format` → `MTLPixelFormat`),
  `formatSupported` from Apple's Apple7+ feature tables plus Metal's runtime queries (no D24S8
  on the M4, sample counts 1, 2, 4; no sRGB storage, no multisampled storage),
  `textureRequirements` (`heapTextureSizeAndAlign`, eGPU memory only, as D3D12), textures of every
  shape (P102) placed in the Memory heap, views as `MTLTexture` views of the type MSC expects
  (arrays when the texture has layers > 1, cubes for whole cubes plus a 2D-array view for storage
  writes, 3D, multisampled), stencil views (`X32_Stencil8`), view formats (P74), names.
- `Resources.cpp`: texture heap entries (read and read-write) and `writeSamplers` with a sampler
  cache per `SamplerDesc` (P99); MSC's runtime header puts a sampler's resource ID in `gpuVA`
  (not `textureViewID` as its manual says); the LOD bias lives in the sampler (MSC reads the
  entry's bias only with `--samplerLODBias`).
- `TextureCommands.cpp`: `copyLayout` with D3D12's rule (rows on 256 bytes, layers on 512; Metal
  needs only whole texels, so one buffer layout suits every backend), the three copies with regions
  (blits on the compute encoder; depth and stencil through blit options), `cmdClearTexture`
  (render targets and depth: a load-action render pass per mip and layer, ordered like a copy by
  `runClearPass`; storage textures: one row from command memory doubled by blits), `cmdDiscard`
  (bookkeeping plus a full barrier).
- `Queries.cpp`: `cmdWriteTimestamp` (precise, after barriers on all earlier work),
  `cmdResetQueries` (nothing to do, as D3D12), `cmdResolveQueries` (`resolveCounterHeap`, ordered by
  a device fence that later encoders wait for).
- Registered: `compute.names_at_creation`, `threads.pipelines_on_other_threads`,
  `texture.create_and_views`, `storage_write_then_sample`, `clear_commands`, `copy_round_trip`,
  `copy_regions`, `copy_depth_upload`, `samplers`, `cube_storage_then_sample`,
  `unlisted_view_format_reports`, `copy_without_usage_reports`,
  `read_only_depth_sampling_on_color_reports`, and `geometry_shader`, `tessellation`,
  `cube_layered_render` (they skip: P116 (1)). macOS: 79/79 (preset), 68/68 validation off.
- Found: Metal's shader validation (`MTL_SHADER_VALIDATION`) silently blocks textures that are in
  no residency set, even placed in a resident heap (reads 0, writes dropped, nothing printed): RI
  adds every view to the residency set.
- Known gaps: `samplerMinMax` stays false (the debug layer refuses min/max reductions on the M4,
  and Metal has no query for them); timestamps and 3D, cube-array, D32S8 stencil and R11G11B10
  clears were checked by a probe, not by a registered test (`texture.timestamps` and
  `volume_copy_render_sample` render); texture lossless compression over aliased
  placement memory is left to Metal (no reset on create or discard); the formatSupported table is
  read from Apple's tables, not probed per format.
- Review fixes (stage 3a): `cmdCopyTexture` goes through command memory (texture → buffer →
  texture, each side with its blit option) when the two Metal pixel formats differ (Metal's copy
  needs equal formats: unorm/sRGB, float/uint, D32Float into D32S8's depth) or when either view
  is one aspect of a depth-stencil format (Metal's copy moves both aspects); stencil views of
  depth-stencil textures without `eSampled` keep the texture's format (only sampled ones get
  `PixelFormatView` and an `X32_Stencil8` view, which can cost depth buffers their lossless
  compression); `cmdBegin` gives a list a fresh `MTL4CommandBuffer` once its last submission
  completed (shader validation faulted at commit, in `HeapUsageTable::updateHeapUsage`, after
  `freeMemory` of a heap an earlier recording of the same command buffer used). Registered
  `helpers.allocators`, `helpers.transfers`, `helpers.group_grid` (HelperTests.cpp now built for
  Metal); `helpers.frame_ring_and_profiler` renders (3b), `helpers.shader_compiler` yields plain
  DXIL, which Metal refuses (P114). The new copy paths and the free-then-reuse case were checked
  by probes, not registered tests (no portable test copies across formats or aspects). A list
  re-begun without a reset while still in flight keeps its command buffer (Metal allows it), so
  the shader-validation fault can still occur there. macOS: 82/82 (preset), 71/71 validation off.
- **Next (stage 3b):** graphics pipelines, rendering sections, draws, resolves and occlusion
  queries; the remaining `texture.*` cases.

## 8. Stage 3b-1 done (2026-11-08): graphics pipelines, sections and draws

- `Pipeline.cpp`: `createGraphicsPipeline` for vertex + pixel and mesh (+ task) pipelines
  (`MTLRenderPipelineDescriptor` / `MTLMeshRenderPipelineDescriptor`: formats, blend, samples,
  topology class), an `MTLDepthStencilState` per pipeline, and the encoder state Metal keeps
  outside pipelines (cull, winding, fill mode, depth bias, depth clip mode, P102) kept for
  `cmdSetPipeline`. P101's stage-link check from the DXIL signatures, as D3D12; geometry, hull,
  domain and patch lists report eUnsupported (Vulkan's messages, P116 (1)). `meshShaders` true.
- `Rendering.cpp` (new): `cmdBeginRendering` (both forms; `MTL4RenderPassDescriptor`, color
  attachments through the view's texture, depth and stencil through the whole texture at the
  view's mip and layer; read-only aspects load and store, P75; P105 through
  `renderTargetWidth/Height` and one raster sample), `cmdEndRendering`, viewport, scissor
  (clipped to the section, as Metal requires), stencil reference (P104), `cmdDraw`,
  `cmdSetIndexBuffer`, `cmdDrawIndexed` (draw arguments in command memory at index 4, the
  index type at 5, as MSC's helpers bind them), `cmdDrawMesh` (object and mesh threadgroup
  sizes from the DXIL).
- `Commands.cpp`: one render encoder per section, its argument table set for every render
  stage. A section's fragment work waits for all earlier work and every later encoder waits
  for the section (D3D12's layout switches, P68), so no user barrier is needed around
  attachments; clear passes share this. Pending barriers are taken over by the next encoder
  and passed on by its producer barrier (which covers earlier encoders too, per
  `MTL4CommandEncoder.h`). Commands that run outside sections report an error inside one.
- `Queries.cpp`: timestamps outside sections are `MTL4CommandBuffer::writeTimestampIntoHeap`
  (encoder timestamps on compute encoders resolved as 0 or out of order once a list had
  several encoders: a 3a bug, found by `texture.timestamps`); inside a section, the render
  encoder's, after the fragment stage (after the vertex stage before any draw: a fragment
  timestamp before a draw stays 0).
- `Debug.cpp`: markers inside a section are the render encoder's debug groups; one still open
  at `cmdEndRendering` continues on the command buffer.
- `src/RI/Reflection/Dxil.cpp`: the PSV0 stage numbers of mesh and amplification shaders were
  wrong (8 and 9; DXC writes 13 and 14), so their numthreads read as 0. Only Metal uses them.
- Registered: `texture.render_then_sample`, `msaa_render_then_sample`, `clear_load_op`,
  `depth_write_then_sample`, `read_only_depth_section(_sampling)`, `discard_after_aliasing`,
  `timestamps`, `indexed_draw_with_blending`, `stencil_test`, `stencil_reference_command`,
  `mesh_shaders`, `stage_link`, `line_strip`, `wireframe`, `depth_bias`, `depth_clamp`,
  `volume_copy_render_sample`, `viewport_and_scissor`, `rendering_without_attachments`,
  `rendering_desc_without_attachment_reports`, `helpers.frame_ring_and_profiler`, and in
  validation builds `depth_write_in_read_only_section_reports`, `missing_discard_reports`,
  `stage_link_mismatch_reports`. macOS: 107/107 (preset), 93/93 validation off.
- Not registered: `texture.resolve_color`, `resolve_depth` (3b-2), `occlusion_and_statistics`
  (occlusion is 3b-2; statistics unsupported, P116), `mesh_statistics` (statistics
  unsupported), `indirect_level1/2` (later stage), `helpers.shader_compiler` (plain DXIL, P114).
  `cube_layered_render` still passes by skipping: it needs a geometry shader (P116 (1)); RI sets
  `renderTargetArrayLength` for views of several layers, untested.
- Known gaps: a barrier inside a section cannot make work wait for the section's earlier
  fragment work (Metal accepts only Vertex, Object and Mesh before an in-pass barrier): validation
  builds report it (proposal §6.2), other builds order what Metal can. MSC computes
  `SV_VertexID = vertex_id - base_vertex` (probed), so first vertex and vertex offset never
  show in SV_VertexID on Metal, unlike D3D12; `SV_InstanceID = instance_id - base_instance`
  matches D3D12 (it excludes the first instance). Indexed strips always restart at 0xFFFF /
  0xFFFFFFFF on Metal (D3D12 and Vulkan leave restart off; documented in `Backend.hpp`). A
  pipeline whose stencil ops write, in a section with read-only stencil, is not reported
  (RI has no portable check; D3D12 and Vulkan leave it to their native layers); depth
  writes are checked at `cmdSetPipeline` and when a section opens with a pipeline set. Sampling a read-only depth attachment
  inside its section is untested on Metal. Pipeline and pass format mismatches are left to
  Metal's debug layer. Probes (not registered): IDs, in-section barriers and markers,
  attachments at other mips and layers, render timestamps:
  `/private/tmp/claude-502/-Users-arthur-dev-EquineRI/a8d0c79c-76bc-4da5-80ab-4d17527aad85/scratchpad/probe-3b1/`.
- Stage 3b-2 followed (§9).

## 9. Stage 3b-2 done (2026-11-09): resolves, occlusion, indirect execution

- **Resolves** (`Rendering.cpp`): at `cmdEndRendering` as store actions with a resolve texture;
  `cmdResolve` as a render pass per layer that loads, stores and resolves. Depth: sample 0, min,
  max; stencil: sample 0.
- **Occlusion** (`Queries.cpp`, P115): each section points its visibility buffer at a page of
  command memory while occlusion pools exist; `cmdEndRendering` copies the used slots into the
  pools. A query begins and ends inside one section and one is open at a time. Pools must exist
  before the section that uses them.
- **Indirect** (`Indirect.cpp`, proposal §7): each record is one Metal indirect draw or dispatch
  reading the record in place; index 4 points at its arguments, and Level 2's index 2 at its
  root data. `indirectScratchRequirements` is 0 and `DeviceLimits::indirectRootData` is true.
  A GPU count (Q4): RI's **first built-in MSL**, compiled from source once per device at first
  use (not at build time, as §7 planned: one source string, no build step), copies the records
  into command memory and zeroes the instance count (draws) or group count x (dispatches, mesh
  draws) of records at or beyond the count. Outside sections it is a compute kernel; inside one,
  a vertex function with rasterization off (a render encoder cannot dispatch), followed by an
  encoder barrier Vertex -> Vertex/Object/Mesh. The debug layer accepts that pipeline in passes
  of any attachment formats.
- Registered: `texture.resolve_color`, `resolve_depth`, `indirect_level1`, `indirect_level2`,
  and a new portable `texture.occlusion` (occlusion without statistics; also in the shared list,
  so the D3D12 and Vulkan runs need it checked on Windows/Linux). macOS: 112/112 (preset),
  98/98 validation off.
- Known gaps: not reviewed by a second pair of eyes (resolves and occlusion were drafted by the
  interrupted 3b-2 agent, indirect written inline). Untested: indirect mesh draws, Level 2
  dispatches with a count, records of more than a few. `maxCount` encodes `maxCount` commands
  on the CPU; an ICB variant (proposal §7 (a)) is not measured. An indirect draw counts as a
  draw for timestamps even if all its records are empty.
- Found afterwards: `depthResolveModes` and `stencilResolveModes` were left 0, so
  `texture.resolve_depth` passed by skipping; fixed with ray tracing (§10), and it now runs.
- Ray tracing followed (§10).

## 10. Ray tracing done (2026-11-09): acceleration structures and ray queries

- `backends/metal/RayTracing.cpp`: bottom levels (triangle geometries) and top levels (Metal's
  72-byte indirect instances, row-major transforms) placed in Memory heaps; builds and refits on
  the compute encoder. `AccelerationStructure::gpuAddress` is the structure's resource ID.
- `cmdBuildTlas` repacks RI's 64-byte instances with a second built-in kernel
  (`equine_repack_instances`, in `Indirect.cpp`'s library) into the first
  `alignUp(72 x count, 256)` bytes of the scratch, which `tlasSizes` adds (Q5). The flags copy
  as Metal's options (same bits), the instance ID becomes the user ID, the hit-group
  contribution is dropped.
- MSC's 64-byte header stays in the structure's range (P116 (4)):
  `accelerationStructureRequirements` adds a 128-byte buffer after the structure, and since
  `eGPU` memory is GPU-only, `cmdBuildTlas` writes the header (resource ID and zeroed
  contributions) with a copy. A top level is readable by shaders once built.
  `writeDescriptors` points the entry's `gpuVA` at the header.
- On Metal, `Stage::eAccelerationStructure` also covers Dispatch and Blit (the repack and the
  header copy). Structures join the residency set, as texture views do.
- New `DeviceLimits::rayTracingHardware` (P116 (4), Q8): Metal reports
  `supportsRaytracing && supportsFamily(Apple9)` (true on the M4); D3D12 and Vulkan copy
  `rayTracing`. Documented in `reference/DeviceLimits.md`. The D3D12 and Vulkan edits are
  compiled on Windows and Linux only: check them there.
- `raytracing.build_trace_update` registered on Metal and checked to really trace (t = 1, then
  1.5 after the refit). A temporary probe with a translated instance gave t = 1.25 and the
  expected x shift, so the row-major transform holds. macOS: 113/113 (preset), 99/99
  validation off.
- Known gaps: untested on Metal: indexed geometry, several geometries or instances, instance
  masks, IDs and flags (the test shader reads only t), `ePreferFastBuild`, top-level refits.
  Whether Apple7 and Apple8 (M1, M2) report `supportsRaytracing` and pass is unchecked (no
  such Mac here). Not reviewed by a second pair of eyes.
- **Next:** swapchains (`CAMetalLayer`; their tests skip without a window server), then a
  review pass over stages 3b-2 and ray tracing.

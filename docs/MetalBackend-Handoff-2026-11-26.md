# Metal backend: handoff (2026-11-26)

State of branch `metal-backend` after the session of 2026-11-26, and what the next session does
first: **native sections and native import on Metal** (P27, P70, P77, P84). It replaces
`MetalBackend-Handoff-2026-11-09.md` as the starting point; that file stays as the detailed log
(its §1 to §9 for stages 1 to 3, ray tracing, swapchains, geometry and tessellation; §10 to §14
for this session). The 2026-11-07 file is the stage-by-stage log before that.

Read in this order: `ARCHITECTURE.md` (now covers three backends), `CONTEXT.md` (P113 to P117 are
the Metal decisions and the vertex-ID rule; P27, P70, P77, P84 the native ones),
`docs/FeatureProposal-Metal-2026-11-07.md` §11, this file, then the 2026-11-09 handoff as needed.

## 1. Where things stand

- Branch `metal-backend`, **pushed** (`origin/metal-backend` = `5750962`), `master` merged in
  (P117 and the 2026-11-12 review). Nothing local is unpushed. A backup of the branch before
  the rebase of 2026-11-26 is the local branch `backup/metal-backend-pre-rebase-2026-11-26`
  (never pushed; it can be deleted).
- Tests on the M4 (macOS 27): **151/151** with `ctest --preset macos-debug` (ASan, UBSan, RI
  validation, `MTL_DEBUG_LAYER=1`, `MTL_SHADER_VALIDATION=1`), 10 runs in a row; **133/133** in
  `build/noval` (RI validation off). No warnings; `code_docs_check.py` clean.
- **Not compiled since 2026-11-26 on Windows or Linux.** The session added D3D12 and Vulkan code
  (shared checks: `checkStencilWriteAllowed` with each backend's `stencilWrite` and
  `stencilReadOnly`; `checkBuildLevel` in both build calls) and shared tests
  (`texture.stencil_write_in_read_only_section_reports`,
  `raytracing.blas_build_into_top_level_reports`, `validation.writes_stencil`,
  `validation.build_level`), and compiled the statistics tests and `helpers.shader_compiler` out
  on Metal only. The user has agents testing Windows and Linux; check their results (they push
  `win-verify-*` style branches, or report to the user) before changing shared code again.
- **Not done on Metal:** native sections and native import (next), the Metal 3 path (after),
  work graphs (D3D12 only), pipeline statistics (none on Metal). `backends/metal/Unsupported.cpp`
  lists what reports `eUnsupported`; native sections and import are not declared at all yet
  (`RI/Metal/Native.hpp` says so).

### What 2026-11-26 did (details in the 2026-11-09 handoff, §10 to §14)

| Commit | Content |
|---|---|
| `252e197`, `45fc25f` | geometry and tessellation rebased on the Linux session's commits; master merged |
| `6796460` | coverage tests of what was untested; **geometry-shader instancing ran instance 0 only** (MSC's helpers pass the draw's instance count as the GS's): fixed |
| `07b2d3d` | review fixes: command memory capped (4 MiB chunks, oversized buffers released at recycle, P115); multisampled staged copies report `eUnsupported`; shared checks for stencil writes in read-only stencil sections and for build levels |
| `10b1ba8` | the Metal test list is the shared list minus `EQUINE_METAL_SKIPPED_TESTS`; `mtl.registry_matches` |
| `a33c3b7` | `ARCHITECTURE.md` for three backends; `HANDOFF.md` points here; site pages |
| `9abe216` | **indirect draws of geometry and tessellation pipelines** (a built-in kernel computes MSC's threadgroup counts per record) |
| `e458da5` | `tools/metal_view_bench`: `MTLTextureUsagePixelFormatView` costs 2 to 3x on color targets; copies between formats go through a view when a texture lists `viewFormats` |
| `18589b1`, `5750962` | `tools/indirect_count_bench`: RI's indirect counts against ICBs; **the user kept RI's path**, and native sections must run user ICBs |

## 2. Environment and rules (the borrowed Mac)

`~/dev/CLAUDE.md` holds the rules; the 2026-11-09 handoff §2 the details. Essentials:

- Work only in `/Users/arthur` and `/opt/homebrew`; anything else is logged in
  `/Users/Shared/arthur-dev/manifest.log` with its removal in `uninstall.sh`. No sudo. Never
  touch `/Users/micheldelon`.
- `. ~/dev/tools/env.sh` in non-login shells. Build and test: `cmake --preset macos-debug &&
  cmake --build --preset macos-debug && ctest --preset macos-debug`. One case:
  `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 build/macos-debug/tests/EquineRIMetalTests <name>`.
  Validation-off tree: `build/noval` (`cmake build/noval && cmake --build build/noval`, then its
  `ctest` with both `MTL_*` variables). Bench tree: `build/bench` (Release, samples on, tests
  off) for `tools/metal_view_bench` and `tools/indirect_count_bench`.
- **The user cannot see the Mac's screen and will not risk crashing it:** no tests that need the
  `arthur` GUI session (on-screen swapchain, HDR10), nothing that could stall the GPU (endless
  kernels, GPU timeouts, unclamped thread counts). Treat those as not testable here.
- Commit as `Raven33` (configured), with the attribution lines the session gives. **Push only
  when the user asks.** Fetch first when a session starts: other sessions (Linux, Windows) push
  to this branch.
- The user decides API and design questions (one question at a time, plain words); the
  assistant decides implementation details and records them, marked for review, in `CONTEXT.md`.
  Performance-affecting choices are measured first (P18), as the two benches did.

## 3. Next: native sections and import on Metal

D3D12 and Vulkan have both (`RI/D3D12/Native.hpp`, `RI/Vk/Native.hpp`; implementations
`backends/d3d12/Rendering.cpp` `beginNative`/`endNative`, `Resources.cpp`, `Textures.cpp`,
`Pipeline.cpp`; Vulkan `Commands.cpp`, `Resources.cpp`, `Textures.cpp`, `Pipeline.cpp`; tests
`d3d12.native_section`, `d3d12.import_objects`, `vk.native_section`, `vk.import_objects`; pages
`docs/site/docs/native/sections.md`, `import.md`). Native declarations are per backend (P84),
not in `tests/conformance/`, so Metal's may differ where Metal differs.

### 3.1 Native sections (P27, P70), what Metal changes

- **No layouts:** P27's in/out layout map has nothing to carry. `endNative(cmd)` likely takes no
  list (or keeps an empty-only one for symmetry: the user's call).
- **Encoders are the hard part.** RI keeps at most one encoder open (a compute encoder for
  dispatches, copies and builds outside sections; the section's render encoder inside), carries
  barriers across encoder boundaries (`pendingBefore/After`, `encoderBefore/After`,
  `Commands.cpp`), and opens encoders on demand. Options to put to the user:
  (a) outside sections, `beginNative` ends RI's open encoder and hands over the
  `MTL4::CommandBuffer`; native code opens and ends its own encoders; `endNative` makes RI's next
  encoder start with a queue barrier against everything before (or leaves ordering to
  `cmdBarrier`); (b) also hand over RI's open compute encoder, opened on demand, for native code
  that only dispatches; (c) both. Inside a rendering section: hand over the open render encoder;
  native code must not end it or change attachments (P70 (2)).
- **What `endNative` restores (P70 (1)):** the argument table bound on the encoder
  (`setArgumentTable`, every stage RI uses) and its entries (heaps at 0 and 1, root data at 2 and
  3, draw arguments 4, index type 5, emulation tables 6 and 7, `DeviceState.hpp`), the render or
  compute pipeline state, depth-stencil state, cull mode, winding, fill mode, depth bias, depth
  clip mode, viewport, scissor, stencil reference; the threadgroup memory length of emulated
  tessellation (`Emulation.cpp`). RI's argument table is RI's own object, so native code that
  binds its own table changes only the encoder's binding, not RI's entries.
- **Residency:** native code adds its allocations to `native(device).residencySet` (or its own
  set on the queue). RI commits the set at `submit` only when it changed something itself
  (`residencyDirty`): `endNative` should mark it dirty, or the docs tell native code to commit.
- **ICBs (the user's requirement, P116 (3), 2026-11-26):** native sections must let user code run
  its own indirect command buffers: the open render and compute encoders
  (`executeCommandsInBuffer`), residency for the ICB, RI's state set again after. The native
  section test should run a user ICB inside a rendering section (see
  `tools/indirect_count_bench` for working Metal 4 ICB code: descriptor, `gpuResourceID`,
  `supportIndirectCommandBuffers` on the pipeline, residency).
- **Occlusion queries** (P115): a section's visibility page is bound at pass creation; native
  code inside a section must not change the visibility result mode while an RI query is open.
  Decide whether `beginNative` inside a section with an open query is refused.
- **Markers** (`Debug.cpp`) and the `sectionDrawn` flag (timestamps) need a rule too.

### 3.2 Import (P77) on Metal

- `importMemory(device, MTL::Heap *, kind, size)`: an MTLHeapTypePlacement heap of the kind's
  storage and cache mode (check), joined to the residency set; RI maps CPU kinds through one
  buffer placed over the whole heap (`Resources.cpp`), so it would place that buffer in the
  imported heap too. Not released on destroy (P77), only removed from the residency set.
- `importBuffer(device, MTL::Buffer *, desc)`, `importTexture(device, MTL::Texture *, desc)`:
  check what RI needs (texture type, pixel format, sample count, `MTLTextureUsage` from
  `usageOf` in `Textures.cpp`, storage mode private), join residency, make RI's views as for its
  own textures. No resting layout to report.
- `importPipeline`: RI needs facts it reads from DXIL today (root data size, threadgroup sizes,
  mesh and task sizes, primitive type, depth-stencil state, the encoder state of
  `bindGraphicsPipeline`), and the pipeline must follow RI's binding model (MSC-converted with
  RI's fixed root signature, P114). A description struct, probably; emulated (geometry or
  tessellation) pipelines likely not importable.
- `writeNativeDescriptor(device, heap, slot, entry)`: a raw 24-byte `IRDescriptorTableEntry`
  (P114 (3)); residency of what it points at is the caller's.

### 3.3 Order of work

1. Ask the user the encoder hand-over question (3.1) and the `endNative` signature; record the
   answers in `CONTEXT.md` (amend P27, P70, P77 for Metal, or a new row).
2. Build native sections, with `metal.native_section`: outside a section (native compute and
   blit work, RI state intact after), inside a section (a native draw and a user ICB, RI's
   pipeline, root data and viewport intact after), and the misuse reports (nested `beginNative`,
   `endNative` without one).
3. Build import, with `metal.import_objects` (a native heap, buffer and texture used by RI's
   commands; RI forgets them; the native objects stay valid), then `writeNativeDescriptor`.
4. The site pages (`docs/site/docs/native/sections.md`, `import.md`) gain Metal; the proposal's
   §11 and this file's §1 updated; `Native.hpp`'s "not yet" comment removed.

## 4. Known open items (unchanged by 2026-11-26)

- Behaviour not reached by a test on this Mac: GPU error to `eDeviceLost` (no fault could be
  provoked on the M4; a timeout would stall the GPU), `acquire` with no drawable (needs the GUI
  session), on-screen swapchain and HDR10.
- Only the M4 was used: M1 and M2 (`supportsRaytracing`, `rayTracingHardware`, the format
  table) unverified.
- Acceleration structures take 64 KiB each at least (`kHeapGranularity`); left as is by the user.
- Multisampled copies of one aspect of a depth-stencil format report `eUnsupported` (no
  depth-only view on Metal).
- Under `MTL_SHADER_VALIDATION`, an ICB's GPU-written execution range is not honoured
  (`docs/measurements/metal_indirect_count/`): matters for native ICB tests run with the preset.

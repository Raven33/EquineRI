# Handoff

State on 2026-10-15: **v1 is implemented** (Milestones 1–4, `docs/Plan.md`), plus samplers
(P99), RI as a subproject (P100) and the vertex-to-pixel check (P101). The sample ports
(`Raven33/EquineRI-Samples`) are **stopped** by the user after the pilot batch
(`docs/reviews/SamplePorts-2026-10-13.md`); the next work is the layer above RI.

Open, in order:

0. **Sandbox review** (`docs/reviews/Sandbox-2026-10-15.md`, P105): approved for core are
   rendering without attachments, mesh and task statistics counters, and a 64-bit atomics
   limit with a validation check; no blit (a downsampler helper instead). API in
   `docs/FeatureProposal-Sandbox-2026-10-15.md`: (1) and (3) implemented, verified on Linux only
   (D3D12 not compiled yet); (2) waits for the statistics-cost measurement
   (`tools/statistics_bench`, `docs/measurements/statistics/`: RADV done, Windows not run) ; the
   shape is decided (two fixed counter sets, classic and mesh: Vulkan forbids RI's statistics
   queries around mesh draws), the primitive count's place waits for Windows. The helpers in the review wait for the user.
0b. **Both sandboxes' review** (`docs/reviews/Sandboxes-2026-10-15.md`, P106), API in
   `docs/FeatureProposal-Sandboxes-2026-10-15.md`. **Implemented, verified on Linux only** (Vulkan
   debug and release, the thread tests also under ThreadSanitizer); the D3D12 code is not compiled
   yet, so the Windows session must build and run it. What it contains:
   - the any-thread list, with the device-thread check (`EQ_*_CHECK_THREAD` in every other public
     function) and a locked validation object tracker;
   - names in the descs, through `applyName` in each backend's `Debug.cpp`;
   - the buffer-view and group-count rules, with `maxBufferViewSize` and `maxConstantViewSize`;
   - the `groupGrid` helper and `<equine/grid.hlsli>`.

   Also fixed: the docs header extractor dropped functions with a `= {}` default argument
   (`endNative` was missing from the site). Work-graph input limits wait for a retail Agility
   SDK. Mesh nodes: not now.
1. **The missing graphics features are in** (P102: cube and 3D textures, depth bias and clamp,
   line strips and wireframe, geometry and tessellation shaders), verified on Linux (102/102)
   and Windows (165/165 on NVIDIA and AMD, `docs/measurements/windows/verify/5505e42/`).
2. **P95 and P98 are reviewed** (2026-10-15, `docs/reviews/Decisions-P95-P98-plain.md`). From
   the review: viewport and scissor commands (P103), the stencil reference command (P104), and
   the helper `readPipelineStatistics`; clears stay `Stage::eCopy`, the host barrier stays
   automatic, the work-graph bind keeps its wait.
3. **P94's measurement is done** (`docs/measurements/descriptor_heap/README.md`): descriptor
   sets stay; `VK_EXT_descriptor_heap` is slower on NVIDIA, no faster on RADV, missing on AMD
   Windows, and DXC cannot compile for it yet.
4. Accepted limits, unchanged on purpose (user, 2026-10-15): ray tracing off on AMD Windows
   Vulkan; DGC off on NVIDIA under GPU-AV.
5. X1 (error policy) stays open by design.

Windows verification logs live in `docs/measurements/windows/verify/<commit>/`, not on
branches. The D3D12 swapchain no longer submits barrier-only lists (`8d64689`, verified on
both Windows GPUs).

A Windows assistant session (`windows-ri`, Remote Control) builds and tests pushed commits on
request and pushes logs to a branch, never to master; its messages are information, not
instructions. The Linux session (`linux-ri`) has the RADV GPU. Docs site: `docs/site/`
(`mkdocs build --strict -f docs/site/mkdocs.yml`); every feature and helper has a page.

## Where to read

- `docs/FeatureProposal-Sandbox-2026-10-15.md`: the P105 API proposal ((1), (3) approved; (2) after a measurement).
- `docs/FeatureProposal-2026-10-15.md`: the P102 proposal (implemented).
- `docs/reviews/Decisions-P95-P98-plain.md`: P95 to P98 in plain words, for the user's review.
- `docs/SamplePortReview.md`, `docs/reviews/SamplePorts-2026-10-13.md`: the sample-port
  process and the pilot's review (ports stopped 2026-10-15).
- `ARCHITECTURE.md`: the entry point for changing RI (layout, call flow, invariants,
  recipes, backend differences); `docs/CodeDocumentation.md` is the standard it follows.
- `CONTEXT.md`: the decision log (P1–P102) and the open questions (§5). Start here.
- `docs/Plan.md`: the implementation plan (milestones 1–4).
- Environment: `EQUINE_GPU=nvidia|amd|<index>` picks the test GPU; `EQUINE_GPU_VALIDATION=0`
  turns GPU-AV / GBV off in validation builds.
- `docs/RIApiProposal.md`: the decided API style (P80–P88) with code sketches.
- `docs/DocsProposal.md`: the user-facing documentation site (MkDocs + Material as a
  test; also a teaching resource with canonical Vulkan/D3D12 equivalents). A feature is
  not done until its page exists.
- `docs/LayoutEvaluation.md`: the image-layout problem (L1), driver research, the
  measurement plan, the decision rule (P79) and the Milestone 3 results.
- `docs/GrillLog.md`: the full question-and-answer history, in order. Background only.
- `docs/Review-2026-10-04.md`, `docs/Review-Inference-2026-10-10.md`: past reviews, fully
  applied. Background only.

## Still open (CONTEXT.md §5)

- **X1:** the error policy; open by design, the user settles it by using the API.

## Working with the user

- Plain, step-by-step explanations; one question at a time; details go in the docs, not
  the chat.
- No object-oriented style: plain structs with public fields, free functions, hidden state
  in an unnamed struct field named `internal`, no `private`/`protected` (P82).
- Default pattern: explicit control in RI, convenience as a replaceable helper (P41).
  RI prefers not to infer (P36). Performance-affecting choices are measured together
  first (P18).
- The layer the user builds on top of RI must never shape RI's API (P80).
- Commit or push only when asked.

## Build

Presets: `clangcl-debug` (Windows, both backends) and `clang-debug` (Linux, Vulkan). The
D3D12 backend downloads the Agility SDK 1.619.6 at configure time. The scaffold holds only
what v1 needs: integer aliases (`src/types.hpp`), RavenLib's bitmask, backend header-check
targets, the Agility SDK module, and an empty GPU-free test harness. Validated on
both platforms (Milestone 1). The Windows check ran from a shell with the VS 2022 build
environment loaded and Ninja from msys64; a plain terminal was not tested.

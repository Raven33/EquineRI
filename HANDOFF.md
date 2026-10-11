# Handoff

State on 2026-10-11: **Milestones 1–4 are implemented** (`docs/Plan.md`). Milestone 3 measured
L1 and confirmed it with P16 and P36 (P97). Milestone 4 added the rest of v1 (P98): debugging,
copies and queries, graphics completion, indirect execution, swapchain, native sections and
import, inline ray tracing, work graphs (D3D12) and the helpers. All Linux tests pass.
Windows at `f63f7cd`: 136/136 on AMD and NVIDIA (work graphs included, on NVIDIA D3D12);
ray tracing is off on AMD Windows Vulkan (P98 (8)). Pending review: P94 (partly reviewed), P95, P97, P98 reviewed 2026-10-11 (DGC off on
NVIDIA under GPU-AV until the user's own investigation).

A Windows assistant session (`windows-equineri`, Remote Control) builds and tests pushed
commits on request; the Linux session has the RADV GPU. Docs site: `docs/site/`
(`mkdocs build --strict -f docs/site/mkdocs.yml`); every feature and helper has a page.

## Where to read

- `ARCHITECTURE.md`: the entry point for changing RI (layout, call flow, invariants,
  recipes, backend differences); `docs/CodeDocumentation.md` is the standard it follows.
- `CONTEXT.md`: the decision log (P1–P98) and the open questions (§5). Start here.
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

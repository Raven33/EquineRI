# Handoff

State on 2026-10-10: **Milestones 1 and 2 are done** (`docs/Plan.md`). Both backends list
GPUs, create devices, allocate memory kinds, place buffers, write descriptor heaps, build
compute pipelines from shader reflection, record and submit command lists, and wait on
timeline points. The Milestone 2 sample (`samples/compute_readback`, buffer -> compute ->
readback) gives correct results on Vulkan (RADV 680M on Linux, RTX 3080 on Windows) and
D3D12 (RTX 3080), with validation on and off. Decisions taken during implementation:
P89-P94 (P94 settles D1 and was decided by the assistant under the user's goal: review it).
**Milestone 3 in progress (2026-10-10), on branch `m3-wip` (not master):** API contract
and portable tests (`tests/gpu/TextureTests.cpp`), D3D12 implementation (17/27 cases passed
on Windows at 9de8f8a; fixes for the global-barrier COMMON access and the 4 MiB MSAA heap
alignment pushed after), the harness (`tools/layout_bench`: shaders, Bench.hpp, main.cpp;
no CMake target and no baseline_vk/baseline_d3d12 yet), and the harness design in
`docs/LayoutEvaluation.md`. The Vulkan side was delegated to a background agent working in
a worktree; check `git worktree list` / its branch and merge it. Survey item 1 results
(Windows) are in the conversation: only NVIDIA has unified layouts; AMD Vulkan lacks D24S8.

A Windows assistant session (`windows-equineri`, Remote Control) builds and tests pushed
commits on request; the Linux session has the RADV GPU. Docs site: `docs/site/`
(`mkdocs build --strict -f docs/site/mkdocs.yml`); every Milestone 2 feature has a page.

## Where to read

- `CONTEXT.md`: the decision log (P1–P94) and the open questions (§5). Start here.
- `docs/Plan.md`: the implementation plan (milestones 1–4).
- `docs/RIApiProposal.md`: the decided API style (P80–P88) with code sketches.
- `docs/DocsProposal.md`: the user-facing documentation site (MkDocs + Material as a
  test; also a teaching resource with canonical Vulkan/D3D12 equivalents). A feature is
  not done until its page exists.
- `docs/LayoutEvaluation.md`: the image-layout problem (L1), driver research, the
  measurement plan, and the decision rule (P79). Run in Milestone 3.
- `docs/GrillLog.md`: the full question-and-answer history, in order. Background only.
- `docs/Review-2026-10-04.md`, `docs/Review-Inference-2026-10-10.md`: past reviews, fully
  applied. Background only.

## Still open (CONTEXT.md §5)

- **L1:** confirmed only by the Milestone 3 measurement, done together on real hardware.
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
targets, the Agility SDK module, `FindDXC`, and an empty GPU-free test harness. Validated on
both platforms (Milestone 1). The Windows check ran from a shell with the VS 2022 build
environment loaded and Ninja from msys64; a plain terminal was not tested.

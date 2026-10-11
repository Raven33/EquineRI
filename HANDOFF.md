# Handoff

State on 2026-10-11: **Milestones 1–3 are done** (`docs/Plan.md`). Milestone 3 added textures,
views, rendering sections, minimal graphics pipelines, discard, clears and timestamps on both
backends (P95), and measured L1 (P96, P97, `docs/LayoutEvaluation.md` "Results"). Outcome:
**L1 confirmed, with P16 and P36** (P97): A' passes P79 on every tested GPU, with the opt-in
`TextureUsage::eReadOnlyDepthSampling` (sampled depth otherwise rests ready for fast sampling)
and placement-balanced measurement (`--swap`). Next: Milestone 4. Pending review: P94 (partly
reviewed), P95, P97.

A Windows assistant session (`windows-equineri`, Remote Control) builds and tests pushed
commits on request; the Linux session has the RADV GPU. Docs site: `docs/site/`
(`mkdocs build --strict -f docs/site/mkdocs.yml`); every Milestone 2 feature has a page.

## Where to read

- `CONTEXT.md`: the decision log (P1–P97) and the open questions (§5). Start here.
- `docs/Plan.md`: the implementation plan (milestones 1–4).
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
targets, the Agility SDK module, `FindDXC`, and an empty GPU-free test harness. Validated on
both platforms (Milestone 1). The Windows check ran from a shell with the VS 2022 build
environment loaded and Ninja from msys64; a plain terminal was not tested.

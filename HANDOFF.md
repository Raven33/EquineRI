# Handoff

State on 2026-10-10: **the design phase is finished.** Next is implementation, starting
with Milestone 1 of `docs/Plan.md` (the user's build check).

## Where to read

- `CONTEXT.md`: the decision log (P1–P88) and the open questions (§5). Start here.
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
- **D1:** the batching form of `writeDescriptors`, decided in Milestone 2 with the Vulkan
  descriptor mechanism.
- **X1:** the error policy; open by design, the user settles it by using the API.
- Implementation choice: the Vulkan loader (Vulkan::Vulkan vs volk).

## Working with the user

- Plain, step-by-step explanations; one question at a time; details go in the docs, not
  the chat.
- No object-oriented style: plain structs with public fields, free functions, hidden state
  in an unnamed struct field named `internal`, no `private`/`protected` (P82).
- Default pattern: explicit control in RI, convenience as a replaceable helper (P41).
  RI prefers not to infer (P36). Performance-affecting choices are measured together
  first (P18).
- The layer the user builds on top of RI must never shape RI's API (P80).
- The user validates builds themselves. Commit or push only when asked.

## Build

Presets: `clangcl-debug` (Windows, both backends) and `clang-debug` (Linux, Vulkan). The
D3D12 backend downloads the Agility SDK 1.619.6 at configure time. The scaffold holds only
what v1 needs: integer aliases (`src/types.hpp`), RavenLib's bitmask, backend header-check
targets, the Agility SDK module, `FindDXC`, and an empty GPU-free test harness. Not yet validated by the user (Milestone 1).

# Handoff

State on 2026-10-10: design phase, run as a question-by-question "grill". All review
findings and open questions have been handled. The inference review (`docs/Review-Inference-2026-10-10.md`) softened P36 and is applied (P36, P45, P62, P67, P68, P71; S4 settled). **No question is pending.**

## Where to read

- `CONTEXT.md`: the decision log (P1–P79) and the open questions (§5).
- `docs/GrillLog.md`: every question and answer, in order.
- `docs/LayoutEvaluation.md`: the image-layout problem (L1), the driver research, the measurement plan, and the decision rule (P79).
- `docs/Review-2026-10-04.md`: the multi-agent review. Groups A, B and C are all done (B and C settled as P60–P71; R50 deferred with ray tracing).

## What remains

- **L1:** the measurement itself (LayoutEvaluation), done together on real hardware. Rule fixed (P79); RDNA2 Vulkan runs both modes (P78).
- **X1:** open by design. The user settles the error model by using the API; signature churn is fine (P57).
- **U1:** the hello-world, waiting on colleagues.
- **Build:** validation by the user.
- Deferred features: ray tracing compaction (with ray tracing), sparse (F1), VRS rate image (F2), multi-queue texture hand-off (P37).

## How the grill is run

Plain, step-by-step explanations, one question at a time. Apply the "control in RI + convenience helper" pattern (P41) by default. Performance-affecting choices are measured together first (P18).

## Build

Not yet validated by the user. Presets: `clangcl-debug` (Windows, both backends) and `clang-debug` (Linux, Vulkan). Run `git submodule update --init --recursive` first. The D3D12 backend downloads the Agility SDK 1.619.6 at configure time.

# Handoff

State on 2026-10-05: design phase, run as a question-by-question "grill".

## Where to read

- `CONTEXT.md`: the decision log (P1–P59) and the open questions (§5).
- `docs/GrillLog.md`: every question and answer, in order.
- `docs/LayoutEvaluation.md`: the image-layout problem (L1), the driver research, and the measurement plan.
- `docs/Review-2026-10-04.md`: the multi-agent review. Group A is applied. Group B (new topics) is in progress. Group C (details of decided mechanisms) has not started.

## Pending question (review group B, topic 5: device creation)

Proposal awaiting a yes/no:

1. **GPU choice.** RI lists the GPUs (name, vendor, memory) and the user picks one. A helper picks the dedicated GPU.
2. **Features.** The user lists the features they need, and device creation fails clearly if one is missing (P11). A "which features exist?" query is available first. A helper enables everything supported.
3. **Native extras at creation.** Device creation accepts extra Vulkan extension names and feature structures, plus a D3D12 pre-device hook (for example experimental features). This way "a missing feature never blocks you" (P22) also covers creation-time features.

## Remaining review group B topics

- GPU addresses: TLAS instances need BLAS addresses (P49); work graphs and virtual-memory systems need GPU addresses too.
- Work-graph details: GPU-side inputs, and root signatures for library shaders.
- Present/submit: present operation and acquire-semaphore wiring (P43, P20).
- Swapchain format / color space / HDR.
- Clears outside rendering sections.
- Memory budget query.
- Runtime reflection library on each backend (P23).

Then group C, the open items Y4, F1–F3, R10, X1, L1 (the measurement), and U1 (the hello-world, waiting on colleagues).

## How the grill is run

Plain, step-by-step explanations, one question at a time. Apply the "control in RI + convenience helper" pattern (P41) by default. Performance-affecting choices are measured together first (P18).

## Build

Not yet validated by the user. Presets: `clangcl-debug` (Windows, both backends) and `clang-debug` (Linux, Vulkan). Run `git submodule update --init --recursive` first. The D3D12 backend downloads the Agility SDK 1.619.6 at configure time.

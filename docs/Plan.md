# EquineRI implementation plan

Written 2026-10-10, at the end of the design phase. Decisions are in `CONTEXT.md`
(P-numbers); the API style is in `docs/RIApiProposal.md` (P80–P88).

Rule for every milestone: small, testable, and ends with something the user runs on real
hardware. Performance-affecting choices are measured together before they are fixed
(P18). Nothing is committed or pushed without the user asking.

## Milestone 1: build check

- The user configures and builds the scaffold with `clangcl-debug` (Windows, both
  backends) and `clang-debug` (Linux, Vulkan), and runs the GPU-free tests.
- Fix whatever breaks. In particular: the Agility SDK download (P55), the clang-tidy
  header filter, the presets' `EQUINE_VALIDATION` values (P21, provisional).
- Exit: both presets build and `ctest` passes.

## Milestone 2: minimal core, both backends

Goal: buffer → compute → readback on Vulkan and D3D12, in the decided API style.

Shared (portable) part:
- Portable description structs, enums, flags (P86), `RI::Window` (OS handles, not native
  graphics objects).
- The central error channel with categories (P57). The policy stays open (X1); the user
  iterates on it.
- The validation skeleton (P88): `validation/` check functions, `EQ_VALIDATE(...)`,
  tracking state in `internal`.
- The conformance source file compiled against each backend (P83).
- `equine/root.hlsli` with `EQ_ROOT_DATA` (P45).

Per backend (`RI::Vk::`, `RI::D3D12::`, P83), with native access only through the native
headers (P84):
- GPU list and device creation (P60): Vulkan enables everything supported except
  robustness; native additions accepted.
- Memory kinds and allocation (P32), placed buffers (P31), GPU addresses (P61), mapping.
- Descriptor heaps (P59) and descriptor writes (P87). **Decide D1 here**: the Vulkan
  descriptor mechanism (descriptor sets with update-after-bind vs descriptor buffer /
  descriptor heap extensions), which fixes the batching form of `writeDescriptors`.
- Command lists, single-threaded (P56); queue, submit, timeline points, wait (P20).
- Compute pipelines (P38): Vulkan reflection with SPIRV-Reflect; D3D12 root signature
  built by RI's own DXIL reader (SFI0, PSV0, `dx.resources`; P67). Root data (P45).
- Coarse stage → stage barriers (P36), buffer copies (P47).
- Open implementation choice: the Vulkan loader (Vulkan::Vulkan vs volk).

Exit: the same sample (buffer → compute → readback) passes on both backends, with
validation on and off.

## Milestone 3: L1 measurement harness

Goal: settle L1 (how image layouts are hidden) with real numbers, before much code
depends on it.

- Textures with usage flags and view formats (P58, P74); view objects (P73); discard
  (P69); rendering sections with load/store and read-only depth (P40, P75); clears (P65).
- RI's own layout transitions with exact/full halves (P68); on Vulkan without unified
  layouts, both modes G and T (P78) with the force option.
- Timestamp queries with CPU reset (P51, P71).
- The baseline: hand-written optimal per-texture transitions (P79).
- Run `docs/LayoutEvaluation.md` items 1–5 (item 6, multi-queue, optional) on the test
  GPUs, together with the user. Apply the decision rule (P79).

Exit: L1 confirmed (or reopened together), and P36 / P16 confirmed with it.

## Milestone 4: the rest of v1 (order chosen together)

Candidates:
- Swapchain and present (P43, P63, P64).
- Graphics pipelines, mesh shaders, vertex pulling (P38, P39); MSAA resolve (P52, P76).
- Native sections, import, gated native headers in full (P27, P70, P77, P84).
- Copy-layout query and texture copies (P47); remaining query types (P71).
- Indirect execution, levels 1 and 2 (P48).
- Acceleration structures, inline ray tracing (P49; not a main v1 target, compaction
  deferred).
- Work graphs, D3D12 only (P50, P62).
- Debugging: names, markers, crash diagnostics, printf, GPU-AV (P53).
- Helpers (P41), each replaceable by the user: automatic allocation, slot assignment,
  staging upload/readback, frames in flight, profiler, frame graph (P35, authoring shape
  to redesign), shader-compilation helper (P29), GLFW window helper (P43).

## Not in v1

Sparse resources (F1), the VRS rate image (F2), multi-queue and cross-queue texture
hand-off (P37), multithreaded recording (P56), resubmission (P63).

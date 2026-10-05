# EquineRI Architecture Context

> Started 2026-10-03 from the design grill recorded in `docs/GrillLog.md`. This file is
> the decision log: it separates settled contracts from open questions. Nothing here is
> an implementation plan yet.

## 1. Project and design posture

A research rendering interface over D3D12 and Vulkan. Its purpose is research
velocity: sending work to the GPU must be trivial, while users retain roughly 90% of the
control a native API gives them. It sits above D3D12/Vulkan, but it does not hide what
researchers need in order to architect complex systems on top of it.

The yardstick is what users have already built: a D3D12 renderer driven by work graphs
with a custom GPU virtual-memory system, and the systems behind three papers built on
earlier Equine states (§3). The base layer must make that class of system buildable
without escaping to native code for the core loop.

Teaching is no longer a goal. Material built on this project adapts to it, not the
other way round.

**The base makes as few architecture decisions for the user as possible.** Frame
structure, lifetime policy, synchronization strategy, and the like belong to the user.
Opinionated behavior ships as helpers layered on top of the base, and a user must always
be able to write their own replacement for any helper (P15). Because the base has no
concept of frames in flight or double/triple buffering, it is a rendering *interface*,
not an engine: hence the name EquineRI (P13).

**No architecture choice that can affect performance is made without careful joint
evaluation** (P18). A proposal that might cost performance becomes a measurement first.

## 2. Settled decisions

| ID | Decision | Source |
|----|----------|--------|
| P1 | Altitude: higher level than D3D12/Vulkan; trivial work submission; ~90% of native control; do not hide what users need to build complex systems. | Round 1, Q1 |
| P2 | Backends: D3D12 and Vulkan. Platforms: Windows (both backends) and Linux (Vulkan). Vendors: AMD and NVIDIA. | Round 1, Q3 |
| P3 | Hardware floor: NVIDIA Turing and AMD RDNA2. No fallback paths below the floor. | Round 1, Q3 |
| P4 | Shading language: HLSL through one SPIR-V-enabled DXC for both DXIL and SPIR-V. Slang was rejected as incomplete. | Brief |
| P5 | The shader is the single source of truth. The base system introduces no custom DSL, attributes, or annotations in HLSL. What the shader is the source of truth *for* is settled by P54. | Brief; Round 1, Q4 |
| P6 | Bindless first. Bindful support comes only on demand and may be emulated by helpers. | Brief |
| P7 | The base system does not prescribe a shader-side addressing or data-access model (for example fat pointers or typed buffer pointers). Such things may ship later as helpers. | Round 1, Q2 |
| P8 | Sebastian Aaltonen's "No Graphics API" is inspiration, not the target. | Round 1, Q2 |
| P9 | Helpers may ship alongside the base system, but the base must be expressive enough that a user could write any helper themselves. | Brief |
| P10 | Synchronization: both coarse and fine-grained forms exist, and coarse is the default. Barriers are explicit (P36). The fine-grained form is not yet defined now that per-texture access barriers are rejected (P26); see open question Y4. | Round 1, Q6 |
| P11 | Asymmetric features (present on one backend or vendor only) are exposed through a capability query, or the call fails as a hard crash with a clear error. Crashing is acceptable in a research setting. | Round 1, Q7 |
| P12 | Porting existing Equine codebases is not a goal. Those codebases are reference workloads (§3). | Round 1, Q8 |
| P13 | New repository, named **EquineRI**: the base has no frames-in-flight or buffering concept, so it is an interface, not an engine. The working directory is still `EquineNext`; the user handles the directory and repository naming. | Rounds 1–3 |
| P14 | Toolchain: CMake with Ninja, clang-cl on Windows and clang on Linux, C++20. Code style follows EquineEngineV2 (`.clang-format`, naming, `e`-prefixed enumerators, `uint32`-style aliases). | Round 1, Q9 |
| P15 | The base makes as few architecture decisions for the user as possible. Anything opinionated (a universal root layout, a frame concept, deferred destruction, a frame graph, barrier tracking) is a helper on top, and replacing it with the user's own must be possible. | Round 2, Q4; softened in round 3 |
| P16 | Users never see or reason about image layouts / layout transitions. How the base achieves this: direction A' (L1), pending measurement. | Round 2, Q2 |
| P17 | Limit external dependencies. Before pulling one in, evaluate whether a simpler in-house solution would do (for example, containers). glm remains for now. | Round 2; softened in round 3 |
| P18 | Architecture choices that can affect performance are evaluated together, with measurements, before they are made. | Round 3, L1 |
| P19 | Target platforms are desktops and laptops only. No mobile or tile-based-GPU concerns shape the design. | Round 3 |
| P20 | Lifetime: the base exposes per-queue GPU timeline points (Vulkan timeline semaphore / D3D12 fence) and destroys immediately. It has no frame concept. Deferred destruction, frames in flight, and ring buffers are helpers built on timeline points. | Round 3, Q2 |
| P21 | Validation is a compile-time flag that enables both native validation layers and Equine's own checks. Build switch: `EQUINE_VALIDATION`, set explicitly per preset; provisionally ON for debug presets and OFF for release and bench (to confirm). | Round 3, Q2 |
| P22 | Native resources are always reachable, so an unsupported (for example bleeding-edge) feature never blocks the user. | Round 3, Q3 |
| P23 | Pipeline layouts derive from shader reflection by default. Other sources may also be accepted ("a/d"), but none is imposed. | Round 3, Q4 |
| P24 | C++-side data that the shader consumes (root data size and layout) is checked against reflection only when validation is enabled (P21). | Round 4, Q3 |
| P25 | Namespace: `RI::`. The user has no preference, so this was chosen for continuity with V2. | Round 4, Q4 |
| P26 | Rejected for L1: per-texture access barriers (candidate B). Too verbose. | Round 4 |
| P27 | Native commands mid-list use an in/out map. On entry (`beginNative`), Equine gives the native command buffer and each texture's native resting layout. On exit (`endNative`), the user lists only the textures they left in another native layout, and Equine transitions them back to rest. Native layouts appear only inside native sections. Validation (P21) reports textures not declared. | Round 6 |
| P28 | Backend selection: one backend per compiled library, with direct calls and no runtime dispatch. A wrapper executable that owns the window and loads either backend library (switch = full reinit, no memory carry-over; possibly also user-code reload) is accepted as a **low-priority** helper, not a main objective. | Round 7 |
| P29 | The base consumes bytecode only: SPIR-V for Vulkan, DXIL for D3D12. Shader compilation is not a base concern. A helper for dynamic (runtime) DXC compilation is provided. | Round 8 |
| P30 | Shader-side bindless access is whatever the shader uses: the global heap (`ResourceDescriptorHeap[]`), declared unbounded arrays, or a mix. Equine follows reflection and imposes no style. A style the toolchain or driver cannot support on a backend fails per P11. Checking DXC/driver support for the global heap on Vulkan is an implementation task. | Round 9 |
| P31 | Memory: RI is level 2. The user allocates memory blocks and places buffers and textures in them at offsets, which permits custom allocators and aliasing. Automatic allocation (level 1, `createBuffer` without a block) is a helper on top. Whether that helper uses VMA/D3D12MA or an in-house allocator is decided under P17. | Round 10 |
| P32 | Memory kinds: a short list. GPU, Upload (CPU→GPU), Readback (GPU→CPU), and GPU + CPU write (ReBAR: Vulkan device-local + host-visible, D3D12 `GPU_UPLOAD`). Equine maps each kind per backend. The full native list is reachable through native access (P22). Availability and size of GPU + CPU write are a capability query (P11) and differ per backend: on Vulkan, systems without resizable BAR expose only a small (~256 MB) device-local + host-visible window; on D3D12, `GPU_UPLOAD` is all-or-nothing, present only when all of VRAM is CPU-visible (ReBAR enabled), with a recent Agility SDK and Windows 11 build, and absent otherwise. | Round 11 |
| P33 | Mapping: GPU memory is never mapped. Upload, Readback, and GPU + CPU write blocks are mapped once at allocation and stay mapped for their lifetime. Equine selects coherent memory so no flush/invalidate calls are needed (to confirm with `vulkaninfo` that coherent readback types exist on the floor drivers). Upload and GPU + CPU write memory is write-only from the CPU in practice: CPU reads are very slow. | Round 11 |
| P34 | Recording is immediate: each RI command call writes the native command at once. Recorded native command buffers may be resubmitted. Anything deferred is a helper on top. Multithreaded recording is **not** in v1 (P56). | Round 12 |
| P35 | Frame graph: if one is provided, it is a helper on top of RI, driven by **declarations** of what each pass reads and writes, never by inspecting commands. (Bindless makes command inspection impossible: commands do not reveal which textures are read.) RI must therefore provide: (1) stage→stage barriers, (2) placed memory (P31), (3) a **discard** command that resets a texture taking over aliased memory, and (4) multiple queues if the graph schedules async compute (Y2). Pass bodies may use any RI or native command. **User dislikes the pass abstraction and capturing lambdas**: the helper's authoring shape is to be redesigned at implementation time. The sketch in the grill log is not a commitment. | Round 12 |
| P36 | Barriers in RI are explicit (user-issued), coarse stage→stage by default. RI infers nothing. Automatic barriers exist only in helpers (for example the frame graph, P35). Follows from P15, P34, and the A' direction of L1; if A' fails measurement, this decision is revisited with L1. | Round 12 |
| P37 | v1 has only the graphics (direct) queue. Compute and copy queues come later; timeline points (P20) are per queue so they extend to multiple queues without redesign. | Round 13 |
| P38 | Pipelines take their complete state up front: shaders, fixed-function state, and render target formats (color, depth, sample count). The native PSO is compiled at creation, at a moment the user chooses; RI never compiles pipelines implicitly during recording. Lazily completing formats or variants at first use (V2's model) is an optional helper. | Round 14 |
| P39 | Geometry input: no classic vertex buffers or input layouts. Vertices reach shaders by vertex pulling (bindless buffers indexed by `SV_VertexID`) or through task/mesh shaders. Index buffers are supported for indexed draws. Classic vertex input may be retrofitted if it becomes a problem. | Round 15 |
| P40 | Rendering sections: `beginRendering` / `endRendering` (dynamic rendering; no render-pass objects). Each attachment, including every MRT color target and depth, takes its own load operation (`eLoad` / `eClear` / `eDontCare`, plus a clear value) and store operation (`eStore` / `eDontCare`). Defaults are `eLoad` / `eStore`. | Round 16 |
| P41 | **Control + convenience pattern.** For any mechanism, RI exposes the explicit, controllable form, and the automatic form is a helper built only on RI's public surface. Instances: memory (P31), lifetime (P20), barriers (P36), pipelines (P38), descriptors (P42). Apply it by default to new topics. | Round 17 |
| P42 | Descriptors: the user writes descriptors (resource views, samplers) into heap slots they choose. Several views of one resource may occupy different slots. Automatic slot assignment on resource creation (`bindlessIndex()`) is a helper (P41). | Round 17 |
| P43 | Windows and swapchains: RI creates no windows. It takes a native window handle (`HWND`, X11 window, Wayland surface) and creates a swapchain. The user controls present mode and resize recreation. `imageCount` is optional (RI picks a default for the present mode). `acquire()` returns the image to draw into, with no index exposed, and blocks if none is free. Acquire/present synchronisation is internal. Swapchain image cycling is **not** frames in flight: CPU/GPU overlap and duplicated per-frame data remain opt-in helpers on timeline points (P20, P13). A GLFW-based window helper lives outside RI (P17). | Round 18 |
| P44 | Root data size and layout are whatever the shader declares; RI chooses no budget. If the declaration is not representable on the backend/device (for example it exceeds the push/root constant limit), pipeline creation fails with a readable error (P11). | Round 19 |
| P45 | Root data is marked in the shader with native HLSL markings: `[[vk::push_constant]]` for Vulkan and a `[RootSignature(...)]` declaration for D3D12. RI reads whichever applies, and a missing or ambiguous marking is a readable error. An optional `.hlsli` helper macro writes both markings (P41). Implementation caveats: the D3D12 `[RootSignature]` describes the *whole* root signature (heap-indexing flags, any tables), and it is a string literal, so the constant count cannot come from `sizeof`. The helper must therefore produce a complete root signature and take an explicit size. When present, the shader's root signature is authoritative on D3D12 (consistent with P23 "a/d"). | Round 19 |
| P46 | Samplers live in the sampler heap, written into user-chosen slots (P42) and picked by index in the shader. No static samplers for now; they can be added later (they are expressible in HLSL only on D3D12). | Round 20 |
| P47 | Transfers: RI provides recorded copy commands (buffer→buffer, buffer→texture, texture→buffer, texture→texture) and a copy-layout query reporting the exact staging layout (row pitch, total size, alignment) per texture subresource, so users never learn per-backend alignment rules. Upload = write to Upload memory + copy + submit; readback = copy to Readback memory + wait on the timeline point + read. One-call `upload`/`readback` with staging management is a helper (P41). | Round 21 |
| P48 | Indirect execution. Level 1 (always available): GPU-written arguments and draw counts for indexed draws, mesh-task draws, and dispatches. Level 2 (behind a capability query, P11): the GPU also changes root data between draws, via D3D12 `ExecuteIndirect` or Vulkan `VK_EXT_device_generated_commands` (NVIDIA; RADV since Mesa 24.3; AMD Windows unconfirmed). On D3D12 the Level-2 query always succeeds. Note: Level 1 has no portable per-draw identifier. Vulkan has `DrawIndex`, but D3D12 has no draw-ID builtin, so per-draw identification on D3D12 uses a Level-2 root constant or an SM 6.8 `SV_StartInstanceLocation` / first-instance trick. | Round 22 |
| P49 | Acceleration structures (inline RT): RI provides a size query (structure + scratch sizes), user-placed storage (P31), and recorded `buildBLAS` / `buildTLAS` / `update` (refit) / `compact` commands. TLAS instances are a user-written buffer in the shared 64-byte instance format (identical on D3D12 and Vulkan). TLASes are bound through descriptor slots (P42). One-call building with memory/scratch/compaction management is a helper (P41). | Round 23 |
| P50 | Work graphs: D3D12 only for now. RI provides up-front program creation from node shaders (as P38), a backing-memory size query with user-placed backing memory (P31), and a recorded `dispatchGraph(program, inputRecords)`. On Vulkan, creation fails with a clear error (P11). `VK_AMDX_shader_enqueue` (AMD-only, provisional) may be revisited if it is finalized. Automatic backing-memory management is a helper (P41). **Hardware:** work graphs need Ampere+ / RDNA3+ even on D3D12 (Agility SDK 1.613+, `D3D12_OPTIONS21::WorkGraphsTier`), so on the P3 floor (Turing, RDNA2) the capability check fails on both backends, and the work-graph reference workload needs above-floor GPUs. | Round 24 |
| P51 | Queries: user-created query pools (timestamp, pipeline statistics, occlusion) with recorded `writeTimestamp`, `beginQuery` / `endQuery`, and `resolveQueries` into a user buffer (typically Readback memory, read after the timeline point), plus a device query for timestamp frequency. A named-scope GPU profiler is a helper (P41). | Round 25 |
| P52 | MSAA resolve, both forms: (1) a per-attachment `resolveTo` target performed at `endRendering`, and (2) a recorded `cmd.resolve(src, dst)` command. | Round 26 |
| P53 | Debugging. (1) Object names and command markers are always on (`VK_EXT_debug_utils`; D3D12 `SetName` / PIX events). (2) GPU crash diagnostics (D3D12 DRED; Vulkan `VK_EXT_device_fault`) are enabled by the validation flag (P21). (3) Shader `printf`: Vulkan now, via DXC + validation-layer debug printf under the validation flag; a D3D12 implementation (buffer-backed) later, as a helper. (4) Bindless index checking via Vulkan GPU-AV / D3D12 GPU-based validation, under the validation flag. | Round 27 |
| P54 | Scope of "the shader is the source of truth": the binding layout (P23), root data size and shape (P44, P45), and bindless access style (P30) come from the shader. Pipeline settings and formats come from C++ (P38). The C++ mirror of root data is written by the user and checked against reflection under validation (P24). A shared `.hlsli` header compiled by both HLSL and C++ (with HLSL-type shims) is a helper (P41). Correction (review 2026-10-04): validation can reliably check the root data *size*, not member offsets, unless the user or the shared-header helper supplies member offsets. DXIL and SPIR-V can also lay out the same HLSL differently (DX root-constant/cbuffer packing vs DXC SPIR-V std430-style push constants; e.g. `float w[4]` has a 16-byte stride under DX packing and 4 bytes under std430), so one C++ mirror fits both backends only if shaders are compiled with a fixed rule such as DXC `-fvk-use-dx-layout`. The compile helper (P29) and the shared-header helper document that flag. | Round 28 |
| P55 | Agility SDK: CMake downloads a pinned NuGet version at configure time, verified by SHA-256 (`cmake/EquineAgilitySDK.cmake`, currently 1.619.6, the latest stable on 2026-10-04). Each D3D12 executable exports `D3D12SDKVersion` / `D3D12SDKPath` through the `EQUINE_D3D12_AGILITY_SDK_EXPORTS()` macro (`backends/d3d12/D3D12AgilityExports.hpp`), and `equine_deploy_agility_sdk(<target>)` copies the runtime to `<exe dir>/D3D12/`. Exports in a DLL are ignored, so with the P28 wrapper, the wrapper executable carries the exports (in its Windows build) and is the deploy target. A `static_assert` in the export header guards against the Windows SDK `d3d12.h` shadowing the Agility one. | Round 29 |
| P56 | v1 records commands from one thread only (research workloads have few draw calls); this supersedes the Round 3 "multithreaded recording: yes". RI objects are not thread-safe in v1. Following P41: command lists (with their native pool/allocator) are user-owned and may be reset/reused only after their timeline point (P20) has completed; automatic recycling is a helper. Multithreaded recording may be added later. | Review round B1 |
| P57 | Error model: **undecided; keep it easy to change.** Every failure site reports through one central channel with a category (programmer error, unsupported feature, device lost, out of memory, swapchain needs recreation). The policy for each category (abort with message, throw, return a status, or a user callback) lives in one place, so changing the model later touches one file, not every call site. Proposals on record (not decided): abort for programmer errors/unsupported/device lost; status returns for swapchain recreation and allocation failure. | Review round B2 |
| P58 | Resource creation: textures (and buffers) take **explicit usage flags** (sampled, storage, render target, depth-stencil, copy source/destination, …). These flags also drive the backend-internal resting layout (L1) without exposing layouts. A helper picks usages automatically (P41). Consequences, applied by default (P41/P31/P11): (1) a memory-requirements query per resource description (size, alignment, compatible memory kinds — D3D12 `GetResourceAllocationInfo` + heap rules, Vulkan `memoryTypeBits`), needed to place resources in user memory; (2) a format/usage support query, with unsupported combinations failing clearly (e.g. D24S8 on AMD Vulkan). Still open: format reinterpretation (typeless/mutable formats) and subresource-range views (part of R10). | Review round B3 |
| P59 | Descriptor heaps: the user creates the resource heap and the sampler heap with sizes they choose, within device limits that RI reports. A default-heap helper picks sizes automatically (P41). Descriptor writes take effect immediately; the user must not overwrite a slot that in-flight GPU work may read, and waits on its timeline point first (same rule as destruction, P20). The Vulkan mechanism behind it (update-after-bind descriptor indexing, descriptor buffer, or descriptor heap extension) is a backend implementation choice. | Review round B4 |

### Feature scope (Round 1, Q7)

| Feature | Status | Notes to verify |
|---------|--------|-----------------|
| Task and mesh shaders | In | D3D12 SM 6.5; `VK_EXT_mesh_shader`. |
| Ray tracing | In, inline only (`RayQuery`) for now | Acceleration-structure API settled by P49. |
| Work graphs | In (P50) | D3D12 SM 6.8, **Ampere+ / RDNA3+ only**: not available on the P3 floor even on D3D12. On Vulkan only `VK_AMDX_shader_enqueue` (AMD, provisional) exists. This is the main P11 case. |
| Indirect execution (ExecuteIndirect / DGC) | In | Levels settled by P48. |
| Sparse / tiled resources | Undecided (F1) | Relevant to the GPU virtual-memory reference workload. Can be added later as a queue-level operation with timeline points and a capability query, without reworking P31/P20/L1. |
| Cooperative matrix / vectors | In, behind a capability query (F3) | Cooperative matrix needs RDNA3+ on AMD (not RDNA2); Turing is fine. Vulkan has `VK_KHR_cooperative_matrix`. Cooperative vectors (D3D12 SM 6.9 / `VK_NV_cooperative_vector`) are vendor- and device-limited and preview-level. Verify before committing. |
| 64-bit atomics | In | Capability inputs: D3D12 SM 6.6 plus `OPTIONS9::AtomicInt64OnTypedResourceSupported`, `OPTIONS9::AtomicInt64OnGroupSharedSupported`, and `OPTIONS11::AtomicInt64OnDescriptorHeapResourceSupported` (the one that matters for heap-indexed access, P30); `Int64ShaderOps` is a prerequisite. Vulkan: `VK_KHR_shader_atomic_int64` feature bits (buffers/shared) and `VK_EXT_shader_image_atomic_int64` (images). |
| Variable-rate shading | Undecided (F2) | |

## 3. Reference workloads

These are capability targets, not port targets:

- three published papers built on various EquineEngine states. All three use mesh and
  compute shaders and use Equine purely as a graphics API. Their main cost was hitting
  features Equine had not implemented and implementing them mid-project. A native API
  would have caused no such problem, except that the co-authors are not fluent in one;
- a D3D12-only system combining work graphs with a custom GPU virtual-memory system.

## 4. Carried over from EquineEngineV2

Taken as-is into the scaffold, open to revision:

- `.clang-format`, `.clang-tidy`, `.clangd`;
- the compiler-runtime policy: exceptions on, ASan + UBSan on by default, a `bench`
  preset without sanitizers, clang-tidy in every build;
- `types.hpp`, `handles.hpp` (now in `namespace RI`, without V2's unused lifetime parameter), `utils/macros.hpp` (without the print-and-return stub macros, see below), and vendored RavenLib (bitmask, slotmap);
- the dependency-free self-registering test harness with one CTest entry per case;
- `FindDXC.cmake` and the single-DXC-for-both-targets rule.

Explicitly **not** carried over as decisions, though available as reference: the
record → compile → resident → execute lifecycle, command patching, the graph compiler,
reflection-driven binding tables, and the print-and-return-invalid stub convention
(it conflicts with P11; its macros were removed in the review cleanup).

## 5. Open questions

Each one is to be settled in a grill round and then moved to §2.

**Naming**
- N1. Settled (P25).

**Escape hatches**
- E1. Settled (P22, P27).

**Shaders**
- S1. Settled (P54).
- S2. Settled (P29).
- S3. Settled (P30).

**Memory and lifetime**
- M1. Settled (P31).
- M2. Settled (P20).
- M3. Settled (P32, P33).

**Synchronization and submission**
- L1. **Blocking; requires joint evaluation (P18).** How are image layouts hidden (P16)
  while the base stays bindless (it cannot see which textures a shader touches)?
  **Current direction: A'** (one resting layout; Equine transitions only around the
  operations it executes itself). It is confirmed only after measurement (P18). B is
  rejected (P26). Driver research and the measurement plan are in
  `docs/LayoutEvaluation.md`. Still to fix with the user before measuring: the threshold
  X, and the RDNA2-Vulkan mode (`GENERAL` everywhere vs. around-pass transitions). If A'
  as a whole fails, L1 is reopened with the user (P36 depends on it).
- Y1. Settled (P36).
- Y2. Settled (P37): later.
- Y3. Settled (P34, P35).
- Y4. The fine-grained synchronization form promised by P10: per-resource barriers that
  name no layout (buffer range, or texture execution/memory dependency), or "fine-grained
  means native sections (P27)"?
- Y5. Settled (P56): single-threaded in v1.

**Pipelines**
- G1. Settled (P38).
- G2. Settled (P39).
- G3. Settled (P38, P40, P52).

**Backends and build**
- B1. Settled (P28).
- B2. Settled (P55).

**Surface not yet discussed** (added round 16)
- R1. Settled (P43).
- R2. Settled (P44, P45).
- R3. Settled (P46).
- R4. Settled (P47).
- R5. Settled (P48).
- R6. Settled (P49).
- R7. Settled (P50).
- R8. Settled (P51).
- R9. Settled (P42).

**Features still undecided** (added in the review, 2026-10-04)
- F1. Sparse / tiled resources (feature table).
- F2. Variable-rate shading (feature table).
- F3. Cooperative matrix / vectors support check on the floor hardware (feature table).

**Resources**
- R10. Views: subresource ranges (mips/layers/aspects) for descriptors, discard, resolve,
  and attachments; format reinterpretation.

**Errors**
- X1. Final error policy per category (P57).

**Tooling**
- T1. Settled (P53; timestamps in P51).

**Workloads**
- W1. Answered: mesh + compute, and missing features were the pain (§3). This feeds E1.
- U1. The ideal hello-world (buffer → compute → readback; texture → swapchain). Deferred
  while the user consults colleagues; the user leans toward raw-Vulkan-like code.

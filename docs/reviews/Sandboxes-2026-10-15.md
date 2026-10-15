# What the two sandboxes ask of RI (2026-10-15)

Before starting the layer above RI, the user asked which features of two research sandboxes
belong in RI, belong in that layer, or are missing from RI. This report covers both sandboxes in
one place. Nothing was built or run.

| Sandbox | Read | What it is |
|---|---|---|
| `Raven33/vulkan_sandboxCailloux` | `main` (2026-10-13), on `Raven33/EquineEngine` at `86caf7e` | Mesh-shader rock piles (RockPiles2): GPU seeding, indirect compute chains, shadow atlas, IBL, a CSV benchmark runner. GLSL. Detail: `Sandbox-2026-10-15.md`. |
| `Andarael/DX12_sandbox` | `Blender-demos-2` at `505a2d5` (2026-10-14), the most up-to-date branch; `main` at `31fb1d2` too | GPU runtime for Blender Geometry Nodes: a Python compiler emits HLSL over a half-edge mesh in a paged GPU arena, run as work graphs or (default) compute passes, drawn with mesh shaders, checked against Blender. D3D12 only. |

The DX12 sandbox is the "D3D12 renderer driven by work graphs with a custom GPU virtual-memory
system" of CONTEXT §1 and §3. Its own TODOs ask for what RI is: "decouple device and resources
from `SandboxApp` and remove `gimslib`", and a backend abstraction to resume "when a real second
backend (Linux/Vulkan) drives the seams".

Three places: **RI core** (signatures change; the user decides), **RI helper** (helpers first,
promoted later), **the layer above** (opinions on how a frame or an app is organised, P35, P80).

## Summary

| # | Item | Place | From | Status |
|---|---|---|---|---|
| C1 | Rendering without attachments | core | Vulkan | **Approved** (P105 (1)) |
| C2 | Mesh and task counters in pipeline statistics | core | Vulkan | **Approved**, shape after a measurement (P105 (2)) |
| C3 | 64-bit atomics flags and check | core | both | **Approved** (P105 (3)) |
| C4 | No blit command | core | Vulkan | **Decided: no** (P105 (4)) |
| C5 | Pipeline creation from several threads | core | both | **Open**: strongest new case |
| C6 | Largest buffer view: limit and check | core | DX12 | **Open** |
| C7 | Group-count limits on dispatches and mesh draws | core | DX12 | **Open** |
| C8 | Work-graph input limits (backing memory) | core | DX12 | **Open**, low priority |
| C9 | Mesh nodes in work graphs | core | DX12 | Recommend **not now** |
| H1 | Per-frame upload and readback rings, with heap slots | helper | both | First helper to design |
| H2 | Shader building: cache, includes, hot reload, linking, deferred release | helper | both | Depends on C5 |
| H3 | Contiguous slot ranges | helper | DX12 | Small |
| H4 | Single-pass downsampler (mips) | helper | Vulkan | **Decided** (P105 (4)), designed later |
| H5 | ImGui renderer | helper (own library) | both | Later |
| H6 | Group grids (C++ and HLSL) | helper | DX12 | Small, with C7 |
| H7 | Submit and wait, CPU profiler scopes | helper | Vulkan | Small |

## 1. RI core

### Decided (P105)

**C1. Rendering without attachments.** The Vulkan sandbox's shadow atlas rasterizes with no
attachment; its pixel shader writes depth with `imageAtomicMin`. Approved as
`cmdBeginRendering(cmd, RenderingSize{width, height})`; API in
`docs/FeatureProposal-Sandbox-2026-10-15.md`.

**C2. Mesh and task counters.** The RockPiles2 benchmarks record mesh primitives per run.
Approved: Vulkan `VK_EXT_mesh_shader` statistics bits and the mesh-primitives query; D3D12
`PIPELINE_STATISTICS1`; `kNoCounter` when unsupported. The shape waits for a measurement of the
statistics-query cost the user saw.

**C3. 64-bit atomics.** The Vulkan sandbox's JFA pass uses `uint64_t` buffer atomics; the DX12
sandbox uses `InterlockedAdd64` on a byte-address buffer. Approved: `int64BufferAtomics` and
`int64SharedAtomics` in `DeviceLimits`, with a validation check at pipeline creation.

**C4. No blit.** D3D12 has none, so a blit would be RI's first built-in shader. Mip generation
becomes a downsampler helper (H4).

### Open

**C5. Pipeline creation from several threads (reopens P56 for creation only).**
The Vulkan sandbox builds pipelines with `std::async`. The DX12 sandbox measured why
(`docs/PERFORMANCE.md` on the branch). On a cold driver, work-graph `CreateStateObject` runs on
one core at about 0.5 s per node: a 196-node graph takes 100.6 s, a 62-node graph 64.0 s. One
gate run spent 553 s of its 600 s in state-object creation. The sandbox now compiles with DXC
and creates state objects and PSOs on a thread pool, one compiler per thread. That divides wall
time by about 9 on 16 cores. Its long-term target, an interactive node editor, is bound by edit
latency.

RI says "RI objects are not thread-safe in v1" (P56). Both APIs allow creation from any thread.
Decision needed: may `createComputePipeline`, `createGraphicsPipeline` and `createWorkGraph` be
called from several threads on one device, with recording staying single-threaded? RI would guard
its own state: object tracking in validation builds, D3D12 root-signature creation, any cache.
A related question: may each thread use its own `ShaderCompiler`?

Out of RI's reach: the driver's disk cache. About 210 state objects are rebuilt from it at each
process start (50 to 260 ms each when warm). D3D12 pipeline libraries do not cover state
objects. Whether `ID3D12ShaderCacheSession` helps for work graphs would need a probe.

**C6. Largest buffer view.** D3D12 limits a buffer view to 2^27 elements: 512 MiB raw,
`stride` × 2^27 structured. Vulkan has `maxStorageBufferRange`, per driver. The DX12 sandbox
hard-codes the D3D12 number (`kMaxRawBufferViewBytes`), sizes its arena segments by it, and pages
its arena because of it (D07). RI reports neither limit and checks neither:

- `checkBufferView` (`src/RI/Validation/Checks.cpp:58`) checks range and alignment only.
- The D3D12 backend casts the element count to `UINT` (`backends/d3d12/Resources.cpp:421`).

So a 1 GiB raw view passes validation, works on Vulkan, and is wrong on D3D12. Proposed:
`DeviceLimits::maxBufferViewSize` and a check in `writeDescriptors`. Bytes or elements is to
decide, since only D3D12 scales the limit with the stride.

**C7. Group-count limits.** The DX12 sandbox folds every mesh-shader and normal-cache launch into
a 2D grid (`HE_GroupGrid`, commit `c5b8c29`). D3D12 allows 65,535 groups per dimension. Vulkan's
minimum is the same, but current desktop drivers accept far more in X. A 100,000-group
`cmdDispatch` can work on Vulkan and fail on D3D12. Mesh and task launches also have a total of
2^22 groups. RI neither states nor checks these. Proposed: portable constants
(`kMaxGroupsPerDimension`, `kMaxMeshGroups`) and a validation check on direct `cmdDispatch` and
`cmdDrawMesh`. Indirect arguments cannot be checked on the CPU.

**C8. Work-graph input limits.** The DX12 sandbox measured 128 MiB to 1.3 GiB of backing memory
per graph, with min equal to max. D3D12's one lever is
`ID3D12WorkGraphProperties1::SetMaximumInputRecords(graph, maxRecords, maxNodeInputs)`, called
before the memory query. RI's `createWorkGraph` (`backends/d3d12/Pipeline.cpp:556`) makes no such
promise, so the user cannot lower `WorkGraph::backing`. Proposed: two optional fields in
`WorkGraphDesc`, with a check in `cmdDispatchGraph`. Check first that the interface exists in
RI's Agility SDK (1.619.6). Low priority, since compute is now the sandbox's default backend.

**C9. Mesh nodes.** A test app and two gimslib tutorials draw from work graphs:
`NodeLaunch("mesh")` plus a pixel shader, in a generic program with graphics state. The product
does not. Mesh nodes need SM 6.9 and the preview Agility SDK, and RI pins a retail one.
Recommendation: not now. Native code can do it meanwhile, in a native section (P27, P70).

### Covered already

Checked against both sandboxes, with no change needed:

- **Commands:**
  - indirect dispatches and mesh draws (P48 level 1). This includes the DX12 sandbox's compute
    backend loop: a one-group prologue, a barrier, an indirect worker dispatch, and a barrier.
  - work graphs with CPU and GPU input and user-placed backing memory (P50, P98 (9))
  - mesh shaders
  - buffer and texture clears
  - viewport and scissor (P103)
- **Queries:** timestamps and the profiler helper.
- **Shader features:** 32-bit image atomics, wave and subgroup operations, `globallycoherent`,
  and the work-graph barriers. The last two are DXC only.
- **Textures:** the DX12 scene pass's two RGBA16F targets, sampled D32 depth (typeless, as RI
  already does), and an R16F storage texture written by compute.
- **Presentation:** present modes, and headless rendering (RI creates no window, P43).
- **Device loss:** detection (DRED, Vulkan device fault). Recovery after a hang is process
  isolation in the test harness, not an RI feature.

Impossible in either API, so not a gap: timestamps per node inside one `DispatchGraph`.

## 2. RI helpers

**H1. Per-frame upload and readback rings, with heap slots.**
- Readback: RockPiles2 reads its allocator state back every frame, and the engine waits on a
  fence each time. A ring tied to `FrameRing` lets the CPU read slot N frames later, with no wait.
- Upload: the DX12 sandbox binds root CBVs per pass and per draw, each a slot of a per-frame
  upload buffer. RI has root data and heap views only (P94 (8)), so each `cbuffer :
  register(bN)` becomes a heap constant view whose index goes in root data. The ring must
  therefore hand out heap slots per frame too.
- Both sandboxes need it, so this is the first helper to design.

**H2. Shader building.** The DX12 sandbox's `ShaderBlobCache` and `PipelineManager` are its most
developed part:
- DXC output keyed by content: the source, every included file, the arguments, and the
  compiler's identity. Kept in memory and on disk.
- A parallel build pool.
- DXIL linking with `IDxcLinker`. Kernels compile once as libraries and link with per-graph
  field code into one `cs_6_x`.
- No pipeline is released while a command list in flight may use it.

The Vulkan sandbox adds hot reload ("Reload Shaders"). RI's `compileShader` has none of this.

A helper would:
- report the included files (for both the cache and the watcher);
- cache by content;
- recreate pipelines on reload;
- release old pipelines after their last timeline point.

Linking is DXIL only. On Vulkan the source would be compiled whole; SPIR-V linking (`spirv-link`)
is unverified. The parallel part depends on C5.

**H3. Contiguous slot ranges.** `Arena64PagedBuffer` takes N heap slots in a row, one per page.
RI's `SlotAllocator` hands out one at a time. A range form (`allocateSlots(count)`,
`releaseSlots(first, count, lastUse)`) is what any paged or virtual-memory scheme needs.

**H4. Downsampler.** The Vulkan sandbox generates mips 6 times (PBR materials). It is decided as a
single-pass, SPD-like helper. sRGB textures need a UNORM view format for storage writes (P74).

**H5. ImGui renderer.** Both sandboxes' whole UI is ImGui (the engine's renderer, gimslib's DX12
backend). One RI version, in its own library like the GLFW helper, serves both backends:
vertices pulled from a buffer, textures as heap slots, the projection in root data.

**H6. Group grids.** The folding of C7, as one C++ function and one HLSL include next to
`equine/root.hlsli`.

**H7. Small ones.**
- Submit and wait as one call. The Vulkan sandbox builds its SDF once per load; `Transfers`
  already does this inside.
- CPU scopes in the profiler, beside the GPU scopes, as in the Vulkan sandbox's zone tree.

## 3. The layer above

**App shell (both):**
- a sub-app or demo registry, switchable at run time, with an ImGui menu bar
- pipelines registered by name and rebuilt on reload, with each pipeline's state, error and
  build time shown
- `--frames N`, headless runs, CPU time per frame phase
- no-sync, vsync or a target frame rate; a fullscreen toggle; finding the project root

**Research tooling (Vulkan sandbox; GPU-API independent):**
- `ParameterCombinator`: linear or log steps, read from CSV
- `Benchmarker`: collects frames until the 95% confidence interval is within a threshold, then
  writes CSV
- a one-minute GPU warm-up
- NVML power draw

Lesson from P96: interleave the variants frame by frame, since clock states move. The DX12
sandbox reached the same rules on its own: same-batch A/B, one case per process, and no timings
on correctness rows.

**Rendering pieces:**
- Vulkan sandbox:
  - fly and debug cameras
  - lights, and a shadow atlas with tiles handed out per light
  - IBL precompute
  - PBR material loading
  - OBJ and glTF loaders
  - procedural generators
- DX12 sandbox:
  - a linear-HDR scene pass with SSAO and color management
  - targets retired after the frames in flight
  - a GPU-driven half-edge mesh renderer: amplification groups packing faces, a normal cache

**Memory (DX12 sandbox):**
- `IMemEngine` and `Arena64`: ephemeral and stable handles in one paged arena (D17), a value
  table of geometry sets (D32), readback by ticket, simulation state ping-ponged across frames
  with no readback.
- On RI this needs H3 and C6. Everything else (buffers, raw views, 64-bit atomics, GPU
  addresses) is there or approved.

**What the old engine did for you that RI leaves above, on purpose:** uniform blocks bound by
name, automatic barriers and layout tracking, cached command lists, per-frame texture copies,
default swapchain depth, deferred deletion. A frame graph above RI (P35) is where most of these
get decided.

**Test systems:** both sandboxes have their own (the DX12 sandbox's Python gate with Blender
references, machine lock, job counts, timeouts). They are not RI's concern.

## 4. Moving each sandbox onto RI

**vulkan_sandboxCailloux.** 61 GLSL files using mesh shaders, subgroup operations and 64-bit
atomics must be rewritten in HLSL (P93, P94). That is the largest single cost of the port. With
C1 to C3 in, nothing else blocks it.

**DX12_sandbox.** Already HLSL with bindless indexing. Work graphs are D3D12 only (P50), but on
the branch the compute backend runs every graph, is the default, and is checked against work
graphs by `--backend parity`. On RI it would run on both backends; only the linking (H2) needs
another path on Vulkan. RI's profiler would also give it the per-pass GPU times its
PERFORMANCE.md lacks for comparing the two backends.

The port costs:
- every `cbuffer : register(bN)` becomes a heap index in root data (H1)
- the push-constant heap-index convention becomes `EQ_ROOT_DATA` (P45)

This is the strongest case of the two for moving onto RI.

## 5. For the DX12 sandbox's authors (not RI; from reading only, unverified)

- `RenderContext::dispatchWorkGraphMesh` (`SandboxApp.hpp:890`) calls
  `SetMaximumInputRecords(index, 1, sizeof(MeshNodeRecord))` at each dispatch, after the backing
  memory was sized and allocated. The third argument is a node-input count, not a record size.
  Once the backing exists, the call probably has no effect.
- `wgProgramDesc.WorkGraph.Flags` keeps `D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE` on every
  `SetProgram` (`PipelineManager.hpp:1310`), so the backing is re-initialized at every dispatch.
  It may bear on PERFORMANCE.md's "repeated evaluation gets slower with dispatch count". RI
  initializes once, at `bindWorkGraphMemory` (P98 (9)).

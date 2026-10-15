# Proposal: the open core items of the sandboxes review (2026-10-15)

`docs/reviews/Sandboxes-2026-10-15.md` lists five RI core items still open, C5 to C9. This page
proposes an implementation for each: what changes, the rules, how it maps to Vulkan and D3D12,
what stays out, and the tests. Nothing is implemented. Every signature change needs the user's
approval. The questions are at the end.

Recommended order:
- C6 and C7 are small checks that remove silent differences between the backends.
- C5 is small in code and large in effect.
- C8 and C9 wait for an Agility SDK that RI can pin (see "SDK" below).

## SDK: what the pinned Agility SDK has

Checked in the headers of the nupkg RI pins (1.619.6, SHA-256 as in
`cmake/EquineAgilitySDK.cmake`) and of the newest preview on NuGet (1.721.3-preview):

| | 1.619.6 (retail, RI's pin, newest retail on NuGet) | 1.721.3-preview |
|---|---|---|
| `ID3D12WorkGraphProperties1::SetMaximumInputRecords` | no | yes |
| `D3D12_WORK_GRAPHS_TIER_1_1` (mesh nodes) | no | yes |
| `D3D12_STATE_SUBOBJECT_TYPE_GENERIC_PROGRAM` | yes | yes |

So C8 and C9 cannot be built on RI's SDK. The preview declares the signature as
`SetMaximumInputRecords(UINT WorkGraphIndex, UINT MaxRecords, UINT MaxNodeInputs)`. This confirms
the review's note that the sandbox passes a record size where a node-input count is expected.

## C5. Creating pipelines from several threads

Use: the DX12 sandbox's cold work-graph creation runs at about 0.5 s per node on one core. Its
thread pool cuts wall time about 9× on 16 cores. The Vulkan sandbox builds pipelines with
`std::async`.

### What changes

No signature changes. The rule of P56 changes for a short list of functions:

> RI objects are not thread-safe, with one exception: `createComputePipeline`,
> `createGraphicsPipeline` and `createWorkGraph` may be called from any thread, concurrently
> with each other and with recording and submission on another thread. Everything else, including
> `destroyPipeline`, `destroyWorkGraph`, `bindWorkGraphMemory` and `workGraphEntry`, stays on the
> thread that records.

```cpp
// Worker threads, while the main thread records frames:
std::vector<std::future<RI::Pipeline>> jobs;
for (const auto &kernel : kernels) {
    jobs.push_back(std::async(std::launch::async, [&] {
        RI::Helpers::ShaderCompiler compiler = RI::Helpers::createShaderCompiler(); // one per thread
        auto shader = RI::Helpers::compileShader(compiler, kernel.source);
        RI::Helpers::destroyShaderCompiler(compiler);
        return RI::createComputePipeline(device, {.shader = shader.bytecode});
    }));
}
```

### What RI must guard (read from the code)

The creation paths of both backends write one piece of shared state: the **object tracker** of
validation builds. `EQ_DX_TRACK` and `EQ_VK_TRACK` insert into `ObjectTracker::live`, an
`unordered_set`, while every recording call reads it through `checkAlive`. Everything else the
creation paths touch is either immutable after device creation or reentrant:
- the device, the volk function table, and the two descriptor set layouts (immutable);
- `createRootSignature` and Vulkan's `createLayout` (they create a new object each call and cache
  nothing);
- the DXIL and SPIR-V readers (no static mutable state);
- `reportError` (aborts).

Implementation:
- `ObjectTracker` gains a `std::shared_mutex`. `trackObject` and `untrackObject` take it
  exclusively; `checkAlive` takes it shared. Validation builds only; release builds have no
  tracker and pay nothing.
- The object-id counter is already a process-wide `std::atomic`.
- D3D12 `CreateComputePipelineState`, `CreatePipelineState`, `CreateStateObject` and
  `CreateRootSignature` are free-threaded. Vulkan `vkCreate*Pipelines` only needs external
  synchronization on the pipeline cache, which RI does not pass (`VK_NULL_HANDLE`).

Destruction stays on the recording thread for a concrete reason. D3D12 `destroyPipeline` erases
from `DeviceState::commandSignatures`, which `cmdExecuteIndirect*` reads and fills while
recording (`backends/d3d12/Indirect.cpp`). Vulkan `indirectScratchRequirements` fills
`indirectLayouts` the same way. Making those thread-safe is possible but not needed by either
sandbox.

### ShaderCompiler (helper)

Document that one `ShaderCompiler` is used by one thread at a time, and that several may exist
at once. `IDxcCompiler3` and the include handler are per object; loading `dxcompiler` twice is
reference-counted by the OS loader. To check while implementing: that `createShaderCompiler`
loads the library through one shared handle, or that loading it twice is harmless on both OSes.

### The thread check (decided by the user, 2026-10-15)

Validation builds report a call from the wrong thread as a programmer error, naming the call
and both threads:
- The **device thread** is the thread that called `createDevice`.
- Every RI function that takes a `Device`, `CommandList` or `Queue` and is not on the any-thread
  list (survey below) checks `std::this_thread::get_id()` against it.

Implementation:
- one shared check function in `src/RI/Validation/` (P88);
- one `EQ_VALIDATE` line per backend function;
- the id stored in `DeviceState` beside the trackers.

Cost: one comparison per call in validation builds; nothing in release builds.

A device created on one thread and driven from another would need a hand-over call. Neither
sandbox does this, so it stays out until one does.

### Survey: which other calls would benefit from other threads

Read from both backends:
- the shared state each function touches, in `DeviceState` and the validation trackers;
- what each native API requires of the caller.

The benefit is judged against the two sandboxes and P56's premise, few draw calls.

| Group | Functions | Benefit | What it would cost RI | Recommendation |
|---|---|---|---|---|
| **Pipeline creation** | `createComputePipeline`, `createGraphicsPipeline`, `createWorkGraph` | High: measured (C5 above) | A lock on the validation object tracker | **Yes** (this proposal) |
| **Work-graph entries** | `workGraphEntry` | The worker that creates a graph wants its entry indices | None: it reads the graph's own state object, and `GetEntrypointIndex` is free-threaded | **Yes**, with creation |
| **Naming what you just created** | `setName` right after creation | Workers name what they build | None, once the name is given at creation | **Replaced** by names in the descs (below); `setName` stays on the device thread |
| **Waiting on the timeline** | `wait`, `isReached` | Medium: a readback or capture thread waits for a point while the main thread keeps recording. The DX12 sandbox's capture with a timeout and heartbeat is this shape | None. Both read only the fence or timeline semaphore (`GetCompletedValue`, `SetEventOnCompletion` with a null event, `vkGetSemaphoreCounterValue`, `vkWaitSemaphores`), all free-threaded | **Yes** |
| **Pure queries** | `bufferRequirements`, `textureRequirements`, `formatSupported`, `copyLayout`, `pipelineStatisticsLayout`, `blasSizes`, `tlasSizes`, `accelerationStructureRequirements` | Low: loader threads can plan layouts and sizes | Probably none: native queries with no RI cache. Each to confirm when implementing | **Yes if confirmed free**, otherwise leave out. Cheap to allow and to test |
| **Resources from a loader thread** | `allocateMemory`, `createBuffer`, `createTexture`, `createView`, `writeDescriptors`, `writeSamplers`, their `destroy*`/`free*` | Medium for streaming, low for both sandboxes: they load at start-up, and the Vulkan sandbox's parallel part is the CPU image decode, which needs no RI call | Several locks: the object and placement trackers; **D3D12 `createTexture` submits on the shared internal list and bumps `lastSignaled`**, which `submit` also uses, so internal submission and `submit` share a lock (Vulkan the same, its internal command buffer); D3D12 `createView` takes slots from RI's CPU RTV/DSV heaps (`allocateSlot`); Vulkan `vkUpdateDescriptorSets` needs the heap's descriptor set externally synchronized (a lock per heap); D3D12 destroys erase from caches recording reads | **Not now.** Revisit with a streaming workload |
| **Indirect level-2 scratch query** | `indirectScratchRequirements` | Low | Vulkan fills `indirectLayouts`, which recording also fills | **No** |
| **Recording several command lists in parallel** | `cmd*` on different lists | Low: research workloads record few commands. The DX12 sandbox's compute backend records three to four commands per pass, and graphs have hundreds of passes, not thousands | D3D12 UAV clears take slots from the shared clear heap (`clearNext`); D3D12 `commandSignatures` and Vulkan `indirectLayouts` are filled lazily while recording; and submission | **No** (P56, P34 stay). Measure CPU recording time on a real frame before revisiting |
| **Submitting from several threads** | `submit`, `present` | Low, and only together with parallel recording | Vulkan queues are externally synchronized; `lastSignaled` is shared on both backends | **No** |
| **Device, swapchain, descriptor heaps, query pools** | create, destroy, resize, acquire, `resetQueries` | None seen | Various | **No** |

Helpers follow the same split:
- `ShaderCompiler`: one per thread, several at once (above).
- `MemoryAllocator`, `SlotAllocator`, `Transfers`, `FrameRing`, `Profiler`: device-thread only,
  since they call device-thread functions.

The any-thread list therefore becomes:
- the three create functions, and `workGraphEntry`;
- `wait` and `isReached`;
- the pure queries that are confirmed to touch no RI state.

The thread check does not apply to these. The validation tracker lock makes their
`checkAlive` reads safe.

### Names at creation (decided by the user, 2026-10-15)

`setName` was a separate call because both native APIs name an object after it exists, and
P98 (2) wrapped them one to one. RI gives the name at creation instead.

- Every desc gains `std::string_view name` as its **last** field, so existing brace-init stays
  valid: `BufferDesc`, `TextureDesc`, `TextureViewDesc`, `ComputePipelineDesc`,
  `GraphicsPipelineDesc`, `WorkGraphDesc`, `DescriptorHeapDesc`, `QueryPoolDesc`, `SwapchainDesc`.
- `allocateMemory`, `createCommandList` and `createAccelerationStructure` have no desc. They gain
  a trailing `std::string_view name = {}`.
- RI copies the name during the call, so a temporary string is fine.
- An empty name is today's behavior: no naming call, and validation messages say `object #N`.
- `setName` stays, on the device thread, for renaming (a command list reused each frame).

```cpp
RI::Pipeline p = RI::createComputePipeline(device, {.shader = cs.bytecode, .name = "gn_extrude_emit"});
RI::Memory m = RI::allocateMemory(device, RI::MemoryKind::eGPU, size, nullptr, "arena page 3");
```

Optional, in validation builds: the object tracker keeps the name, so a message reads
`buffer 'arena page 3' was destroyed` instead of `object #42`.

### What stays out

- Recording and submitting on several threads (P56, P34 stay).
- Resources created or destroyed from other threads (survey).
- A call that hands the device thread to another thread.

### Tests

GPU tests per backend:
- 8 threads create 256 compute pipelines (and, where supported, 16 work graphs), name them, and
  read their entries. Meanwhile the main thread records, submits and waits on a dispatch every
  iteration, and a ninth thread waits on each submitted point. Then every pipeline is used once.
- A validation test: `cmdDispatch` and `destroyPipeline` from a second thread are reported, with
  the call's name.

On Linux, a run under `-fsanitize=thread` in a scratch build.

## C6. The largest buffer view

Use: a 1 GiB raw `ReadWriteView` is valid on Vulkan and wrong on D3D12. RI's validation accepts
it (`checkBufferView`, `src/RI/Validation/Checks.cpp:58`), and the D3D12 backend truncates the
element count to `UINT` (`backends/d3d12/Resources.cpp:421`).

### What changes

Following P94 (4), which already chose one portable rule for view offsets "checked under
validation (D3D12 addresses views in elements)":

```cpp
// The most elements one buffer view may cover, on every backend: D3D12's limit
// (2^27). Raw views count 4-byte elements: at most 512 MiB.
inline constexpr uint32 kMaxBufferViewElements = 1u << 27;

struct DeviceLimits {
    // ...
    uint64 maxBufferViewSize = 0;   // bytes of one read or read-write buffer view
    uint64 maxConstantViewSize = 0; // bytes of one constant buffer view
};
```

`maxBufferViewSize`:
- Vulkan: `maxStorageBufferRange`; it is 32-bit, so 4 GiB − 1 at most.
- D3D12: the element limit only, so the field is UINT32_MAX × 4 and the element limit is what
  binds.

`maxConstantViewSize`:
- Vulkan: `maxUniformBufferRange` (64 KiB on NVIDIA, larger elsewhere).
- D3D12: 65,536 (4,096 × 16 bytes).

### Rules (checked in validation builds, in `writeDescriptors`)

- A read or read-write buffer view covers at most `kMaxBufferViewElements` elements (size ÷
  stride, 4 for raw), on both backends. A program tested on Vulkan then never breaks on D3D12.
- It also covers at most `limits.maxBufferViewSize` bytes, which binds on Vulkan.
- A constant view covers at most `limits.maxConstantViewSize` bytes. The portable floor is 64 KiB,
  stated in the comment.
- The D3D12 `static_cast<UINT>` stays. With the check, it can no longer truncate in validation
  builds.

### How it maps

| | Vulkan | D3D12 |
|---|---|---|
| element limit | none in Vulkan; RI's portable rule | `D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP` (27) |
| byte limit | `maxStorageBufferRange` | none beyond the element limit |
| constant view | `maxUniformBufferRange` | `D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT` × 16 |

To verify on Windows with the debug layer: that structured views are limited by element count
only, so 2^27 elements of any stride is valid.

### What stays out

A "large view" mode that splits a view across several descriptors. That is the paged arena's
job, and it belongs above RI (`Arena64` does it), helped by contiguous slot ranges (helper H3).

### Tests

Validation tests:
- a raw view of 2^27 + 1 elements;
- a structured view of 2^27 + 1 elements with stride 16;
- a constant view of 64 KiB + 256.

Each must be rejected with a message that names the limit. The limits get conformance entries.

## C7. Group counts of dispatches and mesh draws

Use: the DX12 sandbox folds every launch into a 2D grid because D3D12 allows 65,535 groups per
dimension. Vulkan drivers accept far more in X, so a 100,000-group `cmdDispatch` works on Vulkan
and is invalid on D3D12.

### What changes

The same portable approach as C6. The numbers are fixed, so no `DeviceLimits` fields are needed:

```cpp
// The most groups of one dispatch or mesh draw, per dimension, on every backend (D3D12's limit).
inline constexpr uint32 kMaxGroupsPerDimension = 65535;
// The most groups of one mesh draw (task or mesh groups), x * y * z (D3D12's limit).
inline constexpr uint32 kMaxMeshGroups = 1u << 22;
```

### Rules (checked in validation builds)

- `cmdDispatch(cmd, x, y, z)`: each of x, y, z is at most `kMaxGroupsPerDimension`.
- `cmdDrawMesh(cmd, x, y, z)`: the same, and x × y × z is at most `kMaxMeshGroups`, computed in
  64 bits.
- A count of 0 stays valid: it launches nothing on both APIs.
- Indirect arguments (`cmdExecuteIndirect`) cannot be checked on the CPU. The constants'
  comments say they apply there too; GPU-AV and GBV catch some cases.

### How it maps

| | Vulkan (minimums the spec guarantees) | D3D12 |
|---|---|---|
| compute, per dimension | `maxComputeWorkGroupCount` ≥ 65,535 | `D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION` = 65,535 |
| mesh/task, per dimension | `maxMeshWorkGroupCount`, `maxTaskWorkGroupCount` ≥ 65,535 | 65,535 |
| mesh/task, total | `maxMeshWorkGroupTotalCount`, `maxTaskWorkGroupTotalCount` ≥ 2^22 | 2^22 |

Every value is the Vulkan minimum, so the rule never rejects what a Vulkan device must accept.

### Helper (H6, alongside)

`RI::Helpers::groupGrid(count, groupSize)` returns `{x, y, 1}` within `kMaxGroupsPerDimension`.
A matching HLSL function in `equine/grid.hlsli` turns `SV_GroupID` back into a linear group
index. This is the sandbox's `HE_GroupGrid`.

### Tests

Validation tests for x = 65,536 and for a mesh draw of 2,048 × 2,048 × 2 groups. A GPU test that
dispatches `groupGrid` for 10^7 groups and checks a counter.

## C8. Work-graph input limits (needs an SDK with `ID3D12WorkGraphProperties1`)

Use: the DX12 sandbox measured 128 MiB to 1.3 GiB of backing memory per graph, with min equal to
max. D3D12 lets the application promise the largest input of any `DispatchGraph`; the driver can
then size the backing memory for that promise.

### What changes

```cpp
struct WorkGraphDesc {
    Bytecode library;
    uint32 rootDataSize = 0;
    // The largest input of any dispatch of this graph, which lets the driver ask for
    // less backing memory (WorkGraph::backing). 0: no promise.
    uint32 maxInputRecords = 0; // records across all inputs of one dispatch
    uint32 maxNodeInputs = 0;   // entry nodes fed by one dispatch
};
```

`createWorkGraph` calls `SetMaximumInputRecords(graphIndex, maxInputRecords, maxNodeInputs)`
when either field is non-zero, before `GetWorkGraphMemoryRequirements`, so `WorkGraph::backing`
reflects the promise.

### Rules (checked in validation builds)

- Both fields are 0, or both are non-zero.
- `cmdDispatchGraph`: the sum of `recordCount` over the inputs is at most `maxInputRecords`, and
  `inputs.size()` is at most `maxNodeInputs`.
- `cmdDispatchGraphIndirect` cannot be checked on the CPU. The field comment says the promise
  covers GPU inputs too.
- To verify against the work-graphs spec: whether `MaxRecords` counts records across all inputs
  or per input, and how it interacts with `[NodeMaxDispatchGrid]`.

### How it maps

D3D12 only, like every work-graph function (P50). Vulkan reports unsupported as today.

### What it waits for

Neither field can be set on RI's SDK, 1.619.6. Two ways forward:
- Move RI to a preview SDK. That conflicts with pinning a retail SDK (P55), and a preview runtime
  needs Windows developer mode.
- Wait for the next retail SDK. Recommended. Native code cannot call it either in the meantime,
  since the interface is missing from the same headers.

### Tests

On an above-floor GPU: the backing size with and without a promise of (1, 1), the smaller one
working for a one-record dispatch. A validation test for a dispatch over the promise.

## C9. Mesh nodes (recommended: not now)

Kept here so the shape is known when an SDK and a workload arrive.

### Possible shape

```cpp
// The draw part of a work graph whose leaf nodes are mesh nodes (NodeLaunch("mesh")).
struct WorkGraphGraphics {
    Bytecode pixel;                            // ps_6_9, entry "main"
    std::vector<Format> colorFormats;          // as GraphicsPipelineDesc
    Format depthFormat = Format::eUndefined;
    RasterState raster;                        // the same fixed state as GraphicsPipelineDesc
    DepthState depth;
    std::vector<BlendState> blend;
};

struct WorkGraphDesc {
    // ... as C8
    std::optional<WorkGraphGraphics> graphics; // with mesh nodes; DeviceLimits::workGraphMeshNodes
};
```

The type names stand for whatever groups `GraphicsPipelineDesc` uses for its fixed state. The
point is to reuse those groups, not repeat them.

Rules:
- A graph with `graphics` is dispatched inside a rendering section whose formats match.
- A graph without it is dispatched outside any section, as today.

Mapping:
- a generic program over the mesh nodes and the pixel shader;
- the rasterizer, depth-stencil, blend and format subobjects built from the same code as
  `createGraphicsPipeline`;
- `D3D12_STATE_OBJECT_FLAG_WORK_GRAPHS_USE_GRAPHICS_STATE_FOR_GLOBAL_ROOT_SIGNATURE`;
- `WorkGraphsTier >= 1_1` for the new limit.

### Why not now

It needs SM 6.9 and `D3D12_WORK_GRAPHS_TIER_1_1`, which only the preview SDK declares. Only a
test app and two tutorials use it; the product does not. Until then, native code can create the
state object and dispatch it in a native section (P27, P70).

## Questions for the user

1. **C5: decided (user, 2026-10-15).**
   - The thread check is added.
   - The any-thread list of the survey is approved.
   - The device thread is the one that called `createDevice`.
   - Names move into the descs, as an optional last field (empty = today's behavior), and
     `setName` stays on the device thread.
   - Recorded as P106.
2. **C6 and C7: decided (user, 2026-10-15): one portable rule, checked on both backends.**
3. **C6: decided (user, 2026-10-15): `maxConstantViewSize` is added too.**
4. **C8: decided (user, 2026-10-15): keep the retail SDK pin for now; C8 waits.**
5. **C9: decided (user, 2026-10-15): not now.**

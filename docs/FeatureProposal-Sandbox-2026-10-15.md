# Proposal: features from the sandbox review (2026-10-15)

The user approved three core features after the sandbox review
(`docs/reviews/Sandbox-2026-10-15.md`, P105):

1. rendering without attachments;
2. mesh and task counters in pipeline statistics;
3. 64-bit atomics in `DeviceLimits`, with a check.

This page proposes their API. Each part covers what changes, the rules, how it maps to
Vulkan and D3D12, and what stays out. Nothing is implemented yet.

## 1. Rendering without attachments

Use: rasterize only to run pixel shaders that write storage textures or buffers. Examples
are the sandbox's shadow atlas (depth written with `InterlockedMin`), voxelization, and
visibility buffers built with atomics.

### What changes

**Decided by the user (2026-10-15): option (a) of question 1.** A second `cmdBeginRendering`
takes only the size. `RenderingDesc` does not change.

```cpp
struct RenderingSize {
    uint32 width = 0;
    uint32 height = 0;
};

void cmdBeginRendering(CommandList &cmd, const RenderingDesc &desc); // as today
// A section with no attachment: rasterization covers `size`, and pixel shaders
// write only storage (UAVs).
void cmdBeginRendering(CommandList &cmd, RenderingSize size);       // new
```

A pipeline for such a section has no `colorFormats` and `depthFormat = eUndefined`. That
is already valid in `GraphicsPipelineDesc`, so the pipeline needs nothing new.

```cpp
RI::cmdBeginRendering(cmd, RI::RenderingSize{.width = 4096, .height = 4096});
RI::cmdSetPipeline(cmd, shadowPipeline);  // no color formats, no depth format
RI::cmdSetViewport(cmd, tileViewport);    // P103, as usual
RI::cmdDrawMesh(cmd, groups, 1, 1);
RI::cmdEndRendering(cmd);
RI::cmdBarrier(cmd, RI::Stage::ePixel, RI::Stage::ePixel); // storage writes -> sampling, as usual
```

### Rules (checked in validation builds)

- Width and height are 1 to 16384. 16384 is D3D12's
  render-target limit; Vulkan's `maxFramebufferWidth`/`Height` is at least that on the floor
  GPUs.
- `cmdBeginRendering` with a `RenderingDesc` still needs an attachment: an empty
  description is a programmer error that points to the size overload.
- The pipeline must have no color formats, no depth format, and `samples == 1`. On D3D12,
  multisampled rasterization without targets needs `ForcedSampleCount`, a separate feature,
  so it stays out until a workload needs it.
- The viewport and scissor start at the whole size (P103), as they do for attachments.
- Storage writes from the pixel shader are ordinary storage writes. The user orders them
  with the stage barriers they already issue (`Stage::ePixel`); RI adds nothing.

### How it maps

- **Vulkan:** `VkRenderingInfo` with `colorAttachmentCount = 0`, no depth or stencil
  attachment, `renderArea = {0, 0, width, height}`, `layerCount = 1`. Dynamic rendering
  allows zero attachments. The pipeline's `VkPipelineRenderingCreateInfo` already lists zero
  formats.
- **D3D12:** `OMSetRenderTargets(0, nullptr, FALSE, nullptr)`, then the viewport and scissor
  from the size. The backend already handles empty colors; only the size source is new.

### What stays out

- A size together with attachments (a render area smaller than the targets). D3D12 has no
  render area, only the scissor, which already does this.
- Layered rasterization without attachments (`SV_RenderTargetArrayIndex`). There is no
  target layer to write.
- Multisampled rasterization without targets (see the rules).

### Tests

On both backends: a section with no attachment where the pixel shader runs `InterlockedMin`
into a storage texture, read back. Validation tests for each rule.

## 2. Mesh and task counters in pipeline statistics

### What changes

`PipelineStatisticsLayout` gets three offsets, and the helper's `PipelineStatistics` gets the
same three fields:

```cpp
struct PipelineStatisticsLayout {
    // ... existing counters ...
    uint32 taskShaderInvocations = 0; // new: Vulkan task, D3D12 amplification shader
    uint32 meshShaderInvocations = 0; // new
    uint32 meshPrimitives = 0;        // new: primitives the mesh shaders output
};
```

A GPU that cannot count them reports `kNoCounter`, as P102 does for other stages, so no new
`DeviceLimits` flag is needed. Using them is unchanged: `cmdBeginQuery`/`cmdEndQuery` on a
statistics pool, then `cmdResolveQueries`. `layout.size` bytes per query, as today.

### How it maps

- **Vulkan** (`VK_EXT_mesh_shader`, feature `meshShaderQueries`):
  - The two invocation counters are statistics bits (`TASK_SHADER_INVOCATIONS_BIT_EXT`,
    `MESH_SHADER_INVOCATIONS_BIT_EXT`). RI already turns on every supported bit and
    computes offsets from them, so they join the result with no other change.
  - The primitive count is a **separate query type**,
    `VK_QUERY_TYPE_MESH_PRIMITIVES_GENERATED_EXT`. A statistics `QueryPool` holds a second,
    hidden native pool of that type, used together with the first:
    - `cmdBeginQuery`/`cmdEndQuery` begin and end both;
    - `resetQueries` and `cmdResetQueries` reset both;
    - `cmdResolveQueries` copies both into the same buffer with the same stride: the
      statistics at offset 0, the primitive count after them. `meshPrimitives` points
      there.

    The user sees one query.
- **D3D12** (`OPTIONS9::MeshShaderPipelineStatsSupported`): the pool uses
  `D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1` and `D3D12_QUERY_TYPE_PIPELINE_STATISTICS1`.
  The result is `D3D12_QUERY_DATA_PIPELINE_STATISTICS1`: the same counters, then
  `ASInvocations`, `MSInvocations`, `MSPrimitives`. That makes `layout.size` per device
  (it is already returned per device). Without the option, RI keeps the current heap type
  and the three offsets are `kNoCounter`.

### Cost: to measure before deciding the shape (P18)

The user saw a performance loss in vulkan_sandbox when running pipeline-statistics queries
on Vulkan. The engine there (EquineEngine `86caf7e`) turned on one statistics bit (compute
invocations) and a separate mesh-primitives pool. It read results after the frame's timeline
wait, so the loss was the queries' GPU cost, not a CPU stall. RI turns on **every** supported
statistics bit in every statistics pool, so its cost may be higher.

Measure on a mesh-heavy workload (the RTX 3080, the AMD Windows iGPU, RADV). Interleave the
variants frame by frame (P79, P96) and compare:

| Variant | Vulkan | D3D12 |
|---|---|---|
| A. no query | — | — |
| B. statistics, today's bits | all supported bits except task and mesh | `PIPELINE_STATISTICS` |
| C. B + task and mesh bits | + `TASK_`/`MESH_SHADER_INVOCATIONS` | `PIPELINE_STATISTICS1` |
| D. C + mesh primitives | + the `MESH_PRIMITIVES_GENERATED` query | (included in C) |
| E. one bit only, as in the sandbox | compute invocations only | — |

How the results decide the shape:

- **C and D cost about the same as B:** the shape above stands (one query; the primitive
  count joins it).
- **D costs clearly more than C:** the primitive count becomes its own query type
  (`QueryType::eMeshPrimitives`, one 64-bit value per query), so only users who want it pay.
- **B costs clearly more than E:** the number of enabled counters matters. A statistics pool
  then takes the counters it wants (a mask in `QueryPoolDesc`). That is a larger change, to
  propose separately.

"Clearly more" uses P79's rule.

**Found while building the measurement (2026-10-15):** Vulkan forbids a statistics query
that counts input assembly, vertex, geometry, tessellation or clipping from being active
during a mesh draw (`VUID-vkCmdDrawMeshTasksEXT-pipelineStatistics-07076`). RI's statistics
pools count all of those, so **RI's statistics queries cannot be used around mesh draws on
Vulkan today**. D3D12 has no such rule. So a statistics pool must know, on Vulkan, whether it
measures the vertex pipeline or the mesh pipeline, whatever the timings say. RADV also does
not report `meshShaderQueries`: the mesh counters would be `kNoCounter` there. Measurement
and results: `docs/measurements/statistics/README.md`.

### To verify while implementing

- Whether Vulkan's and D3D12's primitive counts agree for the same draw. Both count
  primitives the mesh shader outputs, but culled primitives may be counted differently.
  If they differ, the docs page says how.
- Whether the AMD Windows driver reports `meshShaderQueries` and
  `MeshShaderPipelineStatsSupported`. Where it doesn't, the counters are `kNoCounter`.

### Tests

On both backends: a task and mesh draw with known group and primitive counts, all three
counters read back. A device without them reports `kNoCounter`.

### The three shapes, after the measurement

Measured on the RTX 3080 (`docs/measurements/statistics/README.md`). On Vulkan, the
mesh-primitives query is free, the task counter costs 1.6 ms per frame, and the mesh counter
costs about 100 ms. On D3D12 the three only exist together, in `PIPELINE_STATISTICS1`, at
+130%. Two fixed counter sets are decided (user, 2026-10-15); the question is where the task,
mesh and primitive counters go.

#### Common to the three

The mesh set is a new query type. Its pools count only what Vulkan allows during mesh draws:

```cpp
enum class QueryType : uint8 {
    eTimestamp,
    eOcclusion,
    ePipelineStatistics,     // the classic set: today's counters, for vertex-pipeline draws and dispatches
    eMeshPipelineStatistics, // new: the mesh set, for mesh draws and dispatches
    // ... per option, below
};
```

`pipelineStatisticsLayout` takes the query type. **This is a signature change.** The default
keeps today's calls working:

```cpp
PipelineStatisticsLayout pipelineStatisticsLayout(Device &device,
                                                  QueryType type = QueryType::ePipelineStatistics);
```

Counters a type does not hold are `kNoCounter` in its layout, so `readPipelineStatistics`
(the helper) reads them as 0. Nothing else changes: `cmdBeginQuery`, `cmdEndQuery` and
`cmdResolveQueries` work on every counter type.

How the backends map it:

- **Vulkan:**
  - classic: today's bits;
  - mesh set: `FRAGMENT_SHADER_INVOCATIONS | COMPUTE_SHADER_INVOCATIONS`.
- **D3D12:** both sets use `PIPELINE_STATISTICS`. The mesh set's layout only names the pixel
  and compute offsets.

Rules:

- **Classic set during a mesh draw:** the Vulkan validation layer already reports it (VUID
  07076). D3D12 counts nothing wrong, so RI adds no check.
- **One statistics query at a time:** both APIs allow only one active statistics query of a
  kind. On Vulkan, two `PIPELINE_STATISTICS` pools count as the same kind.
  - The Vulkan layer reports a second one.
  - RI adds a check only where D3D12 is silent, and only if a test shows it is.

#### Option 1: mesh set = pixel and compute; the primitive count is its own query type; task and mesh invocations stay native

**Chosen by the user (2026-10-15).**

```cpp
enum class QueryType : uint8 { ..., eMeshPipelineStatistics, eMeshPrimitives };
struct PipelineStatisticsLayout { /* existing */ uint32 meshPrimitives = kNoCounter; };
```

Usage:

```cpp
RI::QueryPool stats = RI::createQueryPool(device, {.type = RI::QueryType::eMeshPipelineStatistics, .count = 8});
RI::QueryPool prims = RI::createQueryPool(device, {.type = RI::QueryType::eMeshPrimitives, .count = 8});
RI::cmdBeginQuery(cmd, stats, 0);
RI::cmdBeginQuery(cmd, prims, 0);
RI::cmdDrawMesh(cmd, groups, 1, 1);
RI::cmdEndQuery(cmd, prims, 0);
RI::cmdEndQuery(cmd, stats, 0);
RI::cmdResolveQueries(cmd, stats, 0, 1, readback, 0);
RI::cmdResolveQueries(cmd, prims, 0, 1, readback, 256);
// once the point is reached
const auto s = RI::Helpers::readPipelineStatistics(RI::pipelineStatisticsLayout(device, RI::QueryType::eMeshPipelineStatistics), readback.mapped);
const auto p = RI::Helpers::readPipelineStatistics(RI::pipelineStatisticsLayout(device, RI::QueryType::eMeshPrimitives),
                                                   static_cast<std::byte *>(readback.mapped) + 256);
// s.pixelShaderInvocations, s.computeShaderInvocations, p.meshPrimitives
```

Implementation:

- **Vulkan:** `eMeshPrimitives` is `VK_QUERY_TYPE_MESH_PRIMITIVES_GENERATED_EXT`, needing
  `meshShaderQueries`. The layout is `size = 8`, `meshPrimitives = 0`. Free on the RTX 3080.
- **D3D12:** `eMeshPrimitives` is a `PIPELINE_STATISTICS1` heap, needing
  `MeshShaderPipelineStatsSupported`. The layout is `size = 112`,
  `meshPrimitives = offsetof(…, MSPrimitives)`. Costs +130%: D3D12 counts task and mesh
  invocations with it.
- **Without support** (RADV; AMD Windows has no mesh shaders): `createQueryPool` reports
  unsupported, and the layout says `meshPrimitives = kNoCounter`. RI reports it the way it
  reports other missing features (P11).
- **Cost:** what RI offers is cheap, except the primitive count on D3D12, which the docs
  state.

#### Option 2: mesh set = pixel and compute; a third, explicit "mesh counters" query

```cpp
enum class QueryType : uint8 { ..., eMeshPipelineStatistics, eMeshShaderCounters };
struct PipelineStatisticsLayout {
    // existing
    uint32 taskShaderInvocations = kNoCounter;
    uint32 meshShaderInvocations = kNoCounter;
    uint32 meshPrimitives = kNoCounter;
};
```

Usage: as option 1, with `eMeshShaderCounters` in place of `eMeshPrimitives`, read through
the same layout and helper.

```cpp
RI::QueryPool counters = RI::createQueryPool(device, {.type = RI::QueryType::eMeshShaderCounters, .count = 8});
// ... as option 1 ...
// c.taskShaderInvocations, c.meshShaderInvocations, c.meshPrimitives
```

Implementation:

- **Vulkan:** an RI pool holds two native pools, which are begun, ended, reset and resolved
  together. The first is a statistics pool with `TASK_ | MESH_SHADER_INVOCATIONS`; the
  second is the primitives pool. The layout is `size = 24`: task at 0, mesh at 8,
  primitives at 16. RI resolves the two pools into one result with the same stride.
- **D3D12:** `PIPELINE_STATISTICS1`. The layout is `size = 112`, with `ASInvocations`,
  `MSInvocations` and `MSPrimitives`.
- **Rule:** on Vulkan, the mesh set and this type cannot be active together, since both are
  `PIPELINE_STATISTICS` pools. The layer reports it.
- **Cost:** up to 65x on Vulkan (the mesh counter) and +130% on D3D12. The docs call it a
  profiling tool, not something to leave on. The primitive count alone, free on Vulkan, is
  not offered.

#### Option 3: mesh set = pixel, compute and primitives; task and mesh invocations stay native

```cpp
enum class QueryType : uint8 { ..., eMeshPipelineStatistics };
struct PipelineStatisticsLayout { /* existing */ uint32 meshPrimitives = kNoCounter; };
```

Usage: one pool, as option 1's `stats`, and `s.meshPrimitives` is filled in where supported.

Implementation:

- **Vulkan:** the hidden-pool scheme of option 2. The statistics pool has
  `FRAGMENT | COMPUTE`, and a primitives pool is added when `meshShaderQueries` is on. The
  layout is `size = 24` (or 16 without primitives, with `meshPrimitives = kNoCounter`).
- **D3D12:** `PIPELINE_STATISTICS1` where supported. The layout is `size = 112`, with pixel,
  compute and `MSPrimitives`. Otherwise `PIPELINE_STATISTICS`, with
  `meshPrimitives = kNoCounter`.
- **Cost:** free on Vulkan, but **+130% on D3D12 for every mesh-set query**, even when only
  pixel and compute counts are wanted.

#### Comparison

| | Option 1 | Option 2 | Option 3 |
|---|---|---|---|
| New query types | 2 (mesh set, primitives) | 2 (mesh set, mesh counters) | 1 (mesh set) |
| Primitive count, Vulkan | free, own query | 65x, with the mesh counters | free, in the mesh set |
| Primitive count, D3D12 | +130%, own query | +130% | +130% on every mesh-set query |
| Task and mesh invocations | native only | in RI, as a profiling query | native only |
| Hidden second native pool (Vulkan) | no | yes | yes |
| Signature change | `pipelineStatisticsLayout(device, type)` | same | same |

## 3. 64-bit atomics

### What changes

Two flags in `DeviceLimits`:

```cpp
struct DeviceLimits {
    // ... existing limits ...
    bool int64BufferAtomics = false; // new: 64-bit integer atomics on buffers reached through the heap (P30)
    bool int64SharedAtomics = false; // new: 64-bit integer atomics on groupshared memory
};
```

Two flags rather than one because the APIs report them separately, and a GPU may have one
without the other. On the floor GPUs both are expected to be true; the Windows check below
confirms it.

### How it maps

- **Vulkan:**
  - `int64BufferAtomics` = `shaderBufferInt64Atomics`.
  - `int64SharedAtomics` = `shaderSharedInt64Atomics`.
  - Both are Vulkan 1.2 features, already turned on by RI when supported (P60).
- **D3D12:**
  - `int64BufferAtomics` = `OPTIONS11::AtomicInt64OnDescriptorHeapResourceSupported`.
    That is the flag for resources reached through `ResourceDescriptorHeap[]`, which is how
    RI shaders reach buffers. Shader model 6.6 alone does not guarantee it.
  - `int64SharedAtomics` = `OPTIONS9::AtomicInt64OnGroupSharedSupported`.
  - Both also need `OPTIONS1::Int64ShaderOps`.

### The check

Following the user's rule, RI checks only where the native validation does not already
catch it.

- **Vulkan:** the validation layer already rejects a shader that uses 64-bit atomics on
  storage the device lacks. Its SPIR-V checks compare against `shaderBufferInt64Atomics` and
  `shaderSharedInt64Atomics`. RI adds nothing. A test confirms the layer reports it, by
  turning the feature off through `DeviceFeatures`.
- **D3D12:** to be verified on Windows first, by creating a pipeline that uses heap
  resource 64-bit atomics on a device that reports the flag false. If that is not
  possible, the test forces the check path instead.
  - **If the runtime or debug layer already gives a clear error**, RI adds nothing.
  - **If it does not**, RI adds a check at pipeline creation in validation builds. RI's
    DXIL reader already reads the shader's feature flags (`SFI0`, P67). It would add two
    bits, `ATOMIC_INT64_ON_DESCRIPTOR_HEAP_RESOURCE` and `ATOMIC_INT64_ON_GROUP_SHARED`,
    verified against DXC 1.9.2609 like the two heap-indexing bits. A shader with a bit
    whose limit is false is a programmer error that names the shader stage and the
    missing limit.

### What stays out

- 64-bit atomics on textures (Vulkan `VK_EXT_shader_image_atomic_int64`, D3D12
  `AtomicInt64OnTypedResourceSupported`). Less common, and unused by the sandbox. They are
  added with their own flag when a workload needs them.
- A device requirement. The flags report; `createDevice` does not refuse a GPU without
  them.

### Tests

- On both backends: a compute shader running `InterlockedMin`/`InterlockedAdd` on a
  `uint64_t` in a buffer and in groupshared memory, skipped when the limit is false.
- Unit tests of the DXIL flag reading, if the D3D12 check is added.
- On Windows, the limits printed for the RTX 3080 and the AMD iGPU.

## Questions for the user

1. **Rendering without attachments: the shape. Decided: (a)** (user, 2026-10-15). The user found a size field in
   `RenderingDesc` unergonomic: it is required in a rare case and has no effect otherwise.
   Options:
   - **(a) An overload** taking only the size. The size exists only where it matters.
     ```cpp
     void cmdBeginRendering(CommandList &cmd, const RenderingDesc &desc); // as today
     void cmdBeginRendering(CommandList &cmd, RenderingSize size);       // new: no attachment
     ```
     `cmdEndRendering`, viewport, scissor and every other in-section command stay the same.
   - **(b) The field as proposed above**, required with no attachment and refused otherwise.
   - **(c) A size that always applies:** the section covers `size`, and the attachments must
     be at least that large (Vulkan `renderArea`; D3D12 clamps the scissor to it). The field
     always has an effect, but every user must fill it, and it repeats the scissor (P103).

   Recommended: (a).
2. **64-bit atomics:** two flags. Settled (user, 2026-10-15).
3. **Statistics: two fixed counter sets** (user, 2026-10-15), forced by the Vulkan rule above:
   the classic pipeline's counters (today's), and the mesh pipeline's (pixel, compute, task and
   mesh invocations, mesh primitives). Whether the primitive count stays in the mesh set or
   becomes its own query waits for variant D on Windows.

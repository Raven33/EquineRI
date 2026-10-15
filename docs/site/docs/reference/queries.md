---
symbols: [RI::cmdBeginQuery, RI::cmdEndQuery, RI::pipelineStatisticsLayout, RI::PipelineStatisticsLayout, RI::kNoCounter]
---

# RI::cmdBeginQuery, RI::cmdEndQuery, RI::pipelineStatisticsLayout

## What it does

Occlusion and counter queries ([P71](../design-notes/decisions.md#p71),
[P105](../design-notes/decisions.md#p105)), in a [query pool](queryPool.md) of type
`QueryType::eOcclusion`, `ePipelineStatistics`, `eMeshPipelineStatistics` or
`eMeshPrimitives`:

- `RI::cmdBeginQuery(cmd, pool, index)` / `RI::cmdEndQuery(cmd, pool, index)` measure the
  commands between them, in the same command list;
- an **occlusion** query counts the samples that passed the depth and stencil tests (an exact
  count on both backends): one 64-bit value;
- a **pipeline-statistics** query (`ePipelineStatistics`, the classic set) counts vertices,
  primitives and shader invocations, for draws with vertex shaders and for dispatches;
- a **mesh-pipeline statistics** query (`eMeshPipelineStatistics`, the mesh set) counts
  pixel-shader and compute invocations only. Use it around mesh draws: Vulkan forbids the
  classic set there;
- a **mesh-primitives** query (`eMeshPrimitives`) counts the primitives mesh shaders output.
  It is free on Vulkan (measured on an RTX 3080), but on D3D12 it is a
  `PIPELINE_STATISTICS1` query, which also counts task and mesh invocations and made a frame
  2.3x slower in the same measurement (`docs/measurements/statistics/`). Some GPUs cannot
  count it (RADV): `pipelineStatisticsLayout(device, eMeshPrimitives).meshPrimitives` is then
  `kNoCounter`, and `createQueryPool` reports it unsupported;
- the layout of a counter query's result differs per backend and per type, so
  `RI::pipelineStatisticsLayout(device, type)` gives the result's `size` and the byte offset
  of each 64-bit counter (`type` defaults to `ePipelineStatistics`). RI does not convert. A
  counter the type does not hold, or the GPU lacks, is `kNoCounter`. The
  [statistics helper](../helpers/statistics.md) reads any of them into one struct.

Task and mesh invocation counters are not in RI: on Vulkan the mesh counter made a frame 65x
slower (RTX 3080), the task counter 2x. Use them natively
([native sections](../native/index.md)) when you need them.

Copy results to a buffer with [`cmdResolveQueries`](timestamps.md) (each counter-query result
takes `size` bytes), read them after the submission's timeline point, and reset with
[`resetQueries`](queryPool.md).

## Signature

--8<-- "generated/signatures/RI.cmdBeginQuery.md"

--8<-- "generated/signatures/RI.cmdEndQuery.md"

--8<-- "generated/signatures/RI.pipelineStatisticsLayout.md"

--8<-- "generated/signatures/RI.PipelineStatisticsLayout.md"

--8<-- "generated/signatures/RI.kNoCounter.md"

## Example

```cpp
--8<-- "snippets/ri/copies.cpp:queries"
```

## How RI does it

- **Vulkan**: `vkCmdBeginQuery` / `vkCmdEndQuery`; occlusion with
  `VK_QUERY_CONTROL_PRECISE_BIT` (RI requires `occlusionQueryPrecise`). Statistics pools ask
  for seven counters (input vertices and primitives, vertex, clipping, pixel and compute
  invocations, plus geometry and tessellation where the GPU has them); Vulkan writes only
  those, in bit order. Mesh-set pools ask for pixel and compute invocations (16 bytes).
  `eMeshPrimitives` is `VK_QUERY_TYPE_MESH_PRIMITIVES_GENERATED_EXT` (8 bytes; needs
  `meshShaderQueries`).
- **D3D12**: `BeginQuery` / `EndQuery` with `D3D12_QUERY_TYPE_OCCLUSION` and
  `D3D12_QUERY_TYPE_PIPELINE_STATISTICS` for both statistics sets; the result is the 88-byte
  `D3D12_QUERY_DATA_PIPELINE_STATISTICS`, whose vertex counters stay 0 around mesh draws.
  `eMeshPrimitives` is `D3D12_QUERY_TYPE_PIPELINE_STATISTICS1` (needs
  `MeshShaderPipelineStatsSupported`); the result is the 112-byte
  `D3D12_QUERY_DATA_PIPELINE_STATISTICS1`, and RI points at its `MSPrimitives`.

## Rules and errors

- A timestamp pool takes `cmdWriteTimestamp`; occlusion and statistics pools take
  `cmdBeginQuery` / `cmdEndQuery` (**programmer error** in validation builds otherwise).
- A query that begins inside a rendering section ends in it.
- On Vulkan, a classic statistics query must not be active during a mesh draw, and only one
  statistics query (classic or mesh set) may be active at a time. The Vulkan validation
  layer reports both.
- Begin a mesh-primitives query **before** any statistics query that is open at the same
  time, or measure it alone ([P105](../design-notes/decisions.md#p105)). Begun inside an open
  statistics query, NVIDIA's D3D12 driver counts no primitives (nor task and mesh
  invocations), with no warning from the debug layer; begun first, both queries count
  correctly. RI reports the nested order (**programmer error** in validation builds, on both
  backends, so the same code is right everywhere).
- Statistics count all GPU work between begin and end, including the work of RI's own
  operations and of the driver: a clear (`LoadOp::eClear`, `cmdClearTexture`) or a layout
  switch may run as a compute shader on some GPUs. To measure only your draws, begin the
  query inside the rendering section, after `cmdBeginRendering`.
- **How counters count is up to the GPU.** AMD counts pixel-shader invocations per 2x2 quad,
  so 1024 covered pixels may read 256; helper invocations may be counted elsewhere. Compare
  counts on one GPU, not across GPUs.

## Related decisions

- [P71](../design-notes/decisions.md#p71): query reset, resolve, and the statistics layout query.
- [P105](../design-notes/decisions.md#p105): the mesh set and the mesh-primitives query.

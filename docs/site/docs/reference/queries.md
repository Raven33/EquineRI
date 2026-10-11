---
symbols: [RI::cmdBeginQuery, RI::cmdEndQuery, RI::pipelineStatisticsLayout, RI::PipelineStatisticsLayout]
---

# RI::cmdBeginQuery, RI::cmdEndQuery, RI::pipelineStatisticsLayout

## What it does

Occlusion and pipeline-statistics queries ([P71](../design-notes/decisions.md#p71)), in a
[query pool](queryPool.md) of type `QueryType::eOcclusion` or
`QueryType::ePipelineStatistics`:

- `RI::cmdBeginQuery(cmd, pool, index)` / `RI::cmdEndQuery(cmd, pool, index)` measure the
  commands between them, in the same command list;
- an **occlusion** query counts the samples that passed the depth and stencil tests (an exact
  count on both backends): one 64-bit value;
- a **pipeline-statistics** query counts vertices, primitives and shader invocations. The
  layout of its result differs per backend, so `RI::pipelineStatisticsLayout(device)` gives
  the result's `size` and the byte offset of each 64-bit counter. RI does not convert.

Copy results to a buffer with [`cmdResolveQueries`](timestamps.md) (each statistics result
takes `size` bytes), read them after the submission's timeline point, and reset with
[`resetQueries`](queryPool.md).

## Signature

--8<-- "generated/signatures/RI.cmdBeginQuery.md"

--8<-- "generated/signatures/RI.cmdEndQuery.md"

--8<-- "generated/signatures/RI.pipelineStatisticsLayout.md"

--8<-- "generated/signatures/RI.PipelineStatisticsLayout.md"

## Example

```cpp
--8<-- "snippets/ri/copies.cpp:queries"
```

## How RI does it

- **Vulkan**: `vkCmdBeginQuery` / `vkCmdEndQuery`; occlusion with
  `VK_QUERY_CONTROL_PRECISE_BIT` (RI requires `occlusionQueryPrecise`). Statistics pools ask
  for seven counters (input vertices and primitives, vertex, clipping, pixel and compute
  invocations); Vulkan writes only those, in bit order, so the result is 56 bytes.
- **D3D12**: `BeginQuery` / `EndQuery` with `D3D12_QUERY_TYPE_OCCLUSION` and
  `D3D12_QUERY_TYPE_PIPELINE_STATISTICS`; the result is the 88-byte
  `D3D12_QUERY_DATA_PIPELINE_STATISTICS`.

## Rules and errors

- A timestamp pool takes `cmdWriteTimestamp`; occlusion and statistics pools take
  `cmdBeginQuery` / `cmdEndQuery` (**programmer error** in validation builds otherwise).
- A query that begins inside a rendering section ends in it.
- **How counters count is up to the GPU.** AMD counts pixel-shader invocations per 2x2 quad,
  so 1024 covered pixels may read 256; helper invocations may be counted elsewhere. Compare
  counts on one GPU, not across GPUs.

## Related decisions

- [P71](../design-notes/decisions.md#p71): query reset, resolve, and the statistics layout query.

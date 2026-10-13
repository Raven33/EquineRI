---
symbols: [RI::createQueryPool, RI::destroyQueryPool, RI::resetQueries, RI::QueryPool, RI::QueryPoolDesc, RI::QueryType]
---

# RI::createQueryPool, RI::destroyQueryPool, RI::resetQueries

## What it does

A **query pool** holds `count` queries of one type that command lists write into
([P51](../design-notes/decisions.md#p51)):

- `QueryType::eTimestamp`: one 64-bit GPU tick count per query (see
  [Timestamps](../concepts/timestamps.md));
- `QueryType::eOcclusion`: one 64-bit count of samples that passed the depth and stencil
  tests, and `QueryType::ePipelineStatistics`: counters laid out as
  [`pipelineStatisticsLayout`](queries.md) says. Both are measured with
  [`cmdBeginQuery` / `cmdEndQuery`](queries.md).

- `RI::createQueryPool(device, desc)` creates a pool, already **reset**: every query can be
  written;
- `RI::resetQueries(device, pool, first, count)` resets queries `first` to
  `first + count - 1` **on the CPU**, after their results were read. It costs no GPU work (and
  does nothing on D3D12, whose queries need no reset) ([P71](../design-notes/decisions.md#p71));
- `RI::destroyQueryPool(device, pool)` destroys it.

Write timestamps with [`cmdWriteTimestamp`](timestamps.md), copy their results to a buffer with
[`cmdResolveQueries`](timestamps.md), and reset in a command list with
[`cmdResetQueries`](timestamps.md).

## Signature

--8<-- "generated/signatures/RI.createQueryPool.md"

--8<-- "generated/signatures/RI.destroyQueryPool.md"

--8<-- "generated/signatures/RI.resetQueries.md"

--8<-- "generated/signatures/RI.QueryPool.md"

--8<-- "generated/signatures/RI.QueryPoolDesc.md"

--8<-- "generated/signatures/RI.QueryType.md"

## Example

```cpp
--8<-- "snippets/ri/textures.cpp:create-query-pool"
```

```cpp
--8<-- "snippets/ri/textures.cpp:read-timestamps"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/textures.cpp:create-query-pool"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:create-query-pool"
    ```

    What to watch for: a new query pool is **not** reset; reset it before its first use
    (`vkResetQueryPool` from the CPU needs `hostQueryReset`, core in Vulkan 1.2, which RI
    requires). A query written twice without a reset in between is invalid.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:create-query-pool"
    ```

    What to watch for: a query heap needs no reset. The timestamp frequency belongs to the
    queue (`GetTimestampFrequency`).

## Rules and errors

- Reset queries only when the GPU is done with them (after the
  [timeline point](../concepts/timeline.md) of the submission that wrote or resolved them).
- `first` and `count` must be inside the pool (**programmer error** in validation builds).
- Destroy a pool only when the GPU no longer uses it. `QueryPool` is a thin value: copies refer
  to the same pool ([P81](../design-notes/decisions.md#p81)).
- Occlusion and pipeline-statistics queries are written with `cmdBeginQuery` and
  `cmdEndQuery` ([queries](queries.md)).

## Related decisions

- [P51](../design-notes/decisions.md#p51): user-created query pools.
- [P71](../design-notes/decisions.md#p71): created reset; CPU reset after reading; no GPU
  auto-reset.

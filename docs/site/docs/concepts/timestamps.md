# Timestamps

## Measuring GPU time

The CPU cannot time GPU work with its own clock: the GPU runs later, and in parallel. Instead,
the GPU writes **timestamps**, counter values taken as it reaches points in a command list. The
difference between two timestamps, divided by the counter's frequency, is the GPU time between
them ([P51](../design-notes/decisions.md#p51)).

Timestamps are **queries**, kept in a **query pool** you create
([`createQueryPool`](../reference/queryPool.md)). The flow:

1. **write** a timestamp into a query of the pool, before and after the work you measure
   ([`cmdWriteTimestamp`](../reference/timestamps.md));
2. **resolve** the queries into a buffer: one 64-bit value per query
   ([`cmdResolveQueries`](../reference/timestamps.md));
3. after the submission's [timeline point](timeline.md), **read** the buffer (readback memory
   is mapped) and convert ticks with `device.limits.timestampFrequency`;
4. **reset** the queries before writing them again ([`resetQueries`](../reference/queryPool.md)).

```cpp
--8<-- "snippets/ri/textures.cpp:create-query-pool"
```

```cpp
--8<-- "snippets/ri/textures.cpp:write-timestamp-begin"
```

```cpp
--8<-- "snippets/ri/textures.cpp:write-timestamp-end"
```

```cpp
--8<-- "snippets/ri/textures.cpp:read-timestamps"
```

## What a timestamp measures

A timestamp is written when **every earlier command** in the queue has finished. The difference
between two timestamps is therefore the time the GPU took to finish the work between them,
including any wait at a barrier. Work that overlaps the measured range from outside it is
counted too: GPU time is wall time on the GPU, not "the cost of these commands alone".

Only differences are meaningful. Absolute values start at an arbitrary point and may wrap.

## Resets

A query must be reset before it is written again ([P71](../design-notes/decisions.md#p71)):

- a pool is created reset;
- after reading results, call `resetQueries(device, pool, first, count)`: a CPU call with no
  GPU cost (and nothing at all on D3D12);
- to reuse queries within one command list, `cmdResetQueries` records the reset, outside
  rendering sections.

Resolving never resets queries, so a range can be resolved again until it is reset.

## The frequency

`device.limits.timestampFrequency` is the number of ticks per second: Vulkan reports a period
(`timestampPeriod`, nanoseconds per tick) and RI converts it; D3D12 reports the queue's
frequency. It is a device value while RI has one queue
([P71](../design-notes/decisions.md#p71)).

## Other queries and the profiler

Occlusion and pipeline-statistics queries use the same pools: see
[queries](../reference/queries.md). The [profiler helper](../helpers/profiler.md) measures
named, nested scopes with timestamps ([P51](../design-notes/decisions.md#p51)).

## Related decisions

- [P51](../design-notes/decisions.md#p51): query pools, writes, resolves, the frequency.
- [P71](../design-notes/decisions.md#p71): resets, 64-bit results, the frequency per device.
- [P20](../design-notes/decisions.md#p20): reading results after the timeline point.

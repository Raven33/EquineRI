---
symbols: [RI::cmdWriteTimestamp, RI::cmdResetQueries, RI::cmdResolveQueries]
---

# RI::cmdWriteTimestamp, RI::cmdResolveQueries, RI::cmdResetQueries

## What it does

The recorded half of [timestamps](../concepts/timestamps.md)
([P51](../design-notes/decisions.md#p51), [P71](../design-notes/decisions.md#p71)):

- `RI::cmdWriteTimestamp(cmd, pool, index)` writes the GPU's tick count into query `index`
  once every earlier command has finished. Allowed inside and outside rendering sections;
- `RI::cmdResolveQueries(cmd, pool, first, count, buffer, offset)` copies `count` results, one
  64-bit value each, to `buffer` at `offset`. It waits for the queries to be written, and never
  resets them;
- `RI::cmdResetQueries(cmd, pool, first, count)` resets queries in the command list, to write
  them again later in the same list. Most programs reset on the CPU instead, with
  [`resetQueries`](queryPool.md), after reading the results.

Read the results after the submission's [timeline point](../concepts/timeline.md), from a
buffer in `eReadback` memory, and turn tick differences into time with
`device.limits.timestampFrequency` ([`DeviceLimits`](DeviceLimits.md)).

## Signature

--8<-- "generated/signatures/RI.cmdWriteTimestamp.md"

--8<-- "generated/signatures/RI.cmdResolveQueries.md"

--8<-- "generated/signatures/RI.cmdResetQueries.md"

## Example

```cpp
--8<-- "snippets/ri/textures.cpp:write-timestamp-begin"
```

```cpp
--8<-- "snippets/ri/textures.cpp:write-timestamp-end"
```

```cpp
--8<-- "snippets/ri/textures.cpp:read-timestamps"
```

Resetting in a command list:

```cpp
--8<-- "snippets/ri/textures.cpp:reset-in-list"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/textures.cpp:write-timestamp-end"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:write-timestamp-begin"
    ```

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:write-timestamp-end"
    ```

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:read-timestamps"
    ```

    What to watch for: `vkCmdWriteTimestamp2` takes a stage; `ALL_COMMANDS` waits for all
    earlier work. `vkCmdCopyQueryPoolResults` needs `64_BIT` (or results are 32-bit) and
    `WAIT` (or unfinished queries are skipped). The copy is a transfer write: make it visible
    to the CPU with a barrier to the host stage (RI's `cmdEnd` does).

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:write-timestamp-begin"
    ```

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:write-timestamp-end"
    ```

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:read-timestamps"
    ```

    What to watch for: a timestamp is an `EndQuery` with no `BeginQuery`. `ResolveQueryData`
    writes 64-bit values; the destination offset must be a multiple of 8.

## Rules and errors

- `cmdResolveQueries`: the buffer needs `BufferUsage::eCopyDst`, and `offset` must be a
  multiple of 8 with `count * 8` bytes available. On Vulkan RI reports a **programmer error**
  otherwise (the range in validation builds).
- `index`, `first` and `count` must be inside the pool (**programmer error** in validation
  builds, Vulkan).
- `cmdResolveQueries` and `cmdResetQueries` are not allowed inside rendering sections.
- A query must be reset between two writes (on Vulkan; D3D12 needs no reset, and RI's resets
  do nothing there).
- No barrier is needed between `cmdWriteTimestamp` and `cmdResolveQueries`, nor before the
  CPU reads the buffer after the timeline point.

## Related decisions

- [P51](../design-notes/decisions.md#p51): queries, resolve into a user buffer.
- [P71](../design-notes/decisions.md#p71): resets, 64-bit results, resolve never resets.
- [P94](../design-notes/decisions.md#p94): `cmdEnd` makes GPU writes visible to the CPU.

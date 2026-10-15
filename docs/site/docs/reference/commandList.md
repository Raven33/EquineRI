---
symbols: [RI::createCommandList, RI::destroyCommandList, RI::resetCommandList, RI::CommandList]
---

# RI::createCommandList, RI::destroyCommandList, RI::resetCommandList

## What it does

A **command list** is where you record GPU work: copies, dispatches, barriers. Recording
writes each native command at once ([P34](../design-notes/decisions.md#p34)); nothing runs
until you [`submit`](submit.md) the list.

- `RI::createCommandList(device)` creates an empty list with **its own native command memory**
  (a Vulkan command pool, a D3D12 command allocator) ([P56](../design-notes/decisions.md#p56)).
- `RI::resetCommandList(cmd)` throws the recorded commands away so the list can be recorded
  again. Call it only when the list's last submission has finished (its
  [timeline point](../concepts/timeline.md) is reached).
- `RI::destroyCommandList(device, cmd)` destroys it, under the same condition.

The life of a list: create once; then, repeatedly, [`cmdBegin`](cmdBegin.md), record,
`cmdEnd`, `submit` once, wait for the point, `resetCommandList`.

## Signature

--8<-- "generated/signatures/RI.createCommandList.md"

--8<-- "generated/signatures/RI.destroyCommandList.md"

--8<-- "generated/signatures/RI.resetCommandList.md"

--8<-- "generated/signatures/RI.CommandList.md"

## Example

```cpp
--8<-- "snippets/ri/compute.cpp:create-command-list"
```

Recording it again after its point is reached:

```cpp
--8<-- "snippets/ri/compute.cpp:reuse"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:create-command-list"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:create-command-list"
    ```

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:reset"
    ```

    What to watch for: command buffers are allocated from pools, and resetting a pool resets
    all of its buffers. One pool per list keeps lists independent.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:create-command-list"
    ```

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:reset"
    ```

    What to watch for: the allocator owns the memory, the list records into it. Resetting
    the allocator while the GPU still executes the list is undefined behaviour.

## Rules and errors

- Reset or destroy a list only after its last submission's timeline point. In validation
  builds, doing it earlier is a **programmer error**: `resetting a command list the GPU may
  still be executing: wait for its timeline point (value 3) first; the queue has reached 2`.
- A recorded list is submitted **once**; there is no resubmission in this version
  ([P63](../design-notes/decisions.md#p63)).
- Record on the [device thread](../concepts/threads.md), the one that called `createDevice`;
  RI objects are not thread-safe ([P56](../design-notes/decisions.md#p56),
  [P106](../design-notes/decisions.md#p106)). A recording call from another thread is a
  **programmer error** in validation builds.
- `cmdBegin` also clears the list's state (heap, pipeline): set them again in each recording.
- `destroyCommandList` resets the struct.

## Related decisions

- [P34](../design-notes/decisions.md#p34): immediate recording.
- [P56](../design-notes/decisions.md#p56): user-owned lists, reused after their point; one
  thread.
- [P63](../design-notes/decisions.md#p63): submit once, then reset.
- [P20](../design-notes/decisions.md#p20): timeline points.

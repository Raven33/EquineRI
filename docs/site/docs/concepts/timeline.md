# Timeline points and command-list reuse

## The GPU runs behind the CPU

When you [`submit`](../reference/submit.md) a command list, the call returns at once; the GPU
runs the work later, while your program goes on. So the CPU needs a way to ask "has the GPU
finished that work yet?". In RI the answer is a **timeline point**
([P20](../design-notes/decisions.md#p20)).

Each queue has a **timeline**: a 64-bit counter on the GPU that only grows. Every `submit`
returns the next value as an `RI::TimelinePoint`; the GPU raises the counter to that value
when it has finished the submitted work (and everything submitted before it). The point
`{0}` is always reached.

```cpp
--8<-- "snippets/ri/compute.cpp:submit-wait"
```

Two ways to use a point:

- [`RI::wait(queue, point)`](../reference/submit.md) blocks the CPU until the point is reached;
- [`RI::isReached(queue, point)`](../reference/submit.md) answers at once, without blocking.

```cpp
--8<-- "snippets/ri/compute.cpp:poll"
```

Natively, the timeline is a Vulkan **timeline semaphore** and a D3D12 **fence**.

## What waits on a timeline point

RI has **no frame concept** and destroys objects **immediately**
([P20](../design-notes/decisions.md#p20)). Anything the GPU may still use must wait for the
point of the last submission that uses it:

- **reading results** on the CPU (readback memory);
- **destroying** or freeing a buffer, memory, a pipeline, a heap, or a command list;
- **overwriting a descriptor slot** that in-flight work may read;
- **resetting or re-recording a command list** (below).

Frames in flight, deferred destruction and ring buffers are helpers built on timeline points
(keep the point with each object, destroy it once `isReached`).

## Command lists are submitted once, then reused

A [command list](../reference/commandList.md) has one life cycle
([P56](../design-notes/decisions.md#p56), [P63](../design-notes/decisions.md#p63)):

1. `cmdBegin`, record commands, `cmdEnd`;
2. `submit` it **once**, keep its timeline point;
3. when the point is reached, `resetCommandList` and record it again (or destroy it).

```cpp
--8<-- "snippets/ri/compute.cpp:reuse"
```

There is no resubmission of a recorded list in this version. Each command list has its own
native command memory (a Vulkan command pool, a D3D12 command allocator), so resetting one
list never disturbs another. Recording happens on one thread in this version, and RI objects
are not thread-safe: only the calls of the [any-thread list](threads.md) run elsewhere, `wait`
and `isReached` among them.

In validation builds, resetting or destroying a list the GPU may still
be executing is a **programmer error**: `resetting a command list the GPU may still be
executing: wait for its timeline point (value 3) first; the queue has reached 2`.

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:submit-wait"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:timeline"
    ```

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:submit"
    ```

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:wait"
    ```

    What to watch for: binary semaphores and fences (`VkFence`) are the older tools; a
    timeline semaphore replaces both for this use, and needs the `timelineSemaphore` feature
    (core in Vulkan 1.2).

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:timeline"
    ```

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:submit"
    ```

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:wait"
    ```

    What to watch for: `Signal` is a separate queue call after `ExecuteCommandLists`. A
    removed device reports `UINT64_MAX` on every fence; RI checks for it and reports a
    **device lost** error.

## Related decisions

- [P20](../design-notes/decisions.md#p20): per-queue timeline points; immediate destruction.
- [P56](../design-notes/decisions.md#p56): one recording thread; command lists reused only
  after their point.
- [P106](../design-notes/decisions.md#p106): the calls that may run on other threads.
- [P63](../design-notes/decisions.md#p63): no resubmission in v1.
- [P94](../design-notes/decisions.md#p94): `submit`, `wait`, `isReached`; the reuse check.

# Timeline points and command-list reuse

A **timeline point** answers one question: *has the GPU finished this piece of work yet?*
This page explains why you need to ask, what a point is, and the one rule that follows from it.

## The problem: the CPU runs ahead of the GPU

[`submit`](../reference/submit.md) does not run your commands. It hands them to the GPU and
returns **at once**, usually before the GPU has even started. Your program goes on while the
GPU works in the background, often one or two frames behind.

That is good for speed, and it creates a hazard. Right after `submit`:

- the result buffer does **not** hold the result yet;
- the buffers, textures and pipelines the commands use are **still being used**;
- the command list itself is **still being read** by the GPU.

If you read the result, destroy a buffer, or re-record the list now, you race the GPU: you
get stale data, a crash, or a lost device, and often only on some GPUs. You need a way to
know when the GPU is done. That is what a timeline point is for
([P20](../design-notes/decisions.md#p20)).

## What a timeline point is

Think of a ticket counter at a bakery:

- each customer takes a **ticket** with a number: 1, 2, 3, ...;
- a display shows **"now served: n"**, and n only goes up;
- your order is ready once the display shows your number or more.

In RI:

| Bakery | RI |
|---|---|
| The display | The queue's **timeline**: a 64-bit counter kept by the GPU. |
| Taking a ticket | `submit(queue, cmd)`, which returns an `RI::TimelinePoint`. |
| Your ticket's number | `point.value`. |
| "Is my order ready?" | `isReached(queue, point)`: is the counter `>= point.value`? |
| Waiting at the counter | `wait(queue, point)`: sleep until it is. |

A `TimelinePoint` is just that number, wrapped in a struct:

```cpp
struct TimelinePoint {
    uint64 value = 0;
};
```

It holds no object and no handle. Copy it, store it, compare it: it is a plain value.

### A timeline, step by step

Three submissions on the graphics queue:

```text
CPU:  submit(A) -> {1}   submit(B) -> {2}   submit(C) -> {3}   ... CPU goes on ...

GPU:  [---- runs A ----][---- runs B ----][---- runs C ----]
counter:  0             1                 2                 3
                        ^ A done          ^ B done          ^ C done
```

At any moment you can ask about any of the three points:

- just after the three submits, the counter is probably still `0`: none is reached;
- when it shows `2`, points `{1}` and `{2}` are reached, `{3}` is not;
- once it shows `3`, all three are.

Two facts make points easy to use:

1. **Reaching a point means every earlier point is reached too.** The queue runs its
   submissions in order, so if you hold several points, you only need to wait for the
   **latest**.
2. **The point `{0}` is always reached.** A default-constructed `TimelinePoint` means "no
   GPU work to wait for", which is handy as "never used yet".

## Asking: `wait` and `isReached`

```cpp
--8<-- "snippets/ri/compute.cpp:submit-wait"
```

- [`RI::wait(queue, point)`](../reference/submit.md) **blocks** the CPU until the point is
  reached. Simple, and fine for tools, tests, and shutdown. In a frame loop it throws away
  the CPU/GPU overlap.
- [`RI::isReached(queue, point)`](../reference/submit.md) answers **at once**, without
  blocking. Use it to check whether old resources can be freed yet, and do other work if
  not.

```cpp
--8<-- "snippets/ri/compute.cpp:poll"
```

Both may be called from any thread ([P106](../design-notes/decisions.md#p106)).

## The one rule

> Remember the point of the **last submission that uses** something.
> Touch that something only once that point is **reached**.

RI has **no frame concept** and destroys objects **immediately**
([P20](../design-notes/decisions.md#p20)): it does not keep track of what the GPU is using.
That bookkeeping is yours (or a [helper's](#helpers-that-do-the-bookkeeping)). The rule covers:

| You want to... | Wait for the point of... |
|---|---|
| read results on the CPU (readback memory, [queries](timestamps.md)) | the submission that wrote them. |
| destroy a buffer, texture, pipeline or heap, or free memory | the last submission that used it. |
| overwrite a descriptor slot ([P59](../design-notes/decisions.md#p59)) | the last submission that may read the old descriptor. |
| write new data into an upload buffer the GPU reads | the last submission that reads the old data. |
| reset, re-record or destroy a command list | that list's own submission ([below](#command-lists-are-submitted-once-then-reused)). |

"Uses" is about **submissions**, not recording. A buffer used in a list that was recorded but
never submitted is not in use by the GPU.

### Example: read a result

```cpp
const RI::TimelinePoint done = RI::submit(queue, cmd); // the GPU copies into `result`
// result.mapped holds garbage here: the GPU may not have started.
RI::wait(queue, done);
// Now it holds the GPU's writes.
```

### Example: destroy a buffer you stopped using

A buffer used in frame 10 and not after. Store frame 10's point with it, and destroy it
later, once that point is reached, without stalling:

```cpp
struct Retired {
    RI::Buffer buffer;
    RI::TimelinePoint lastUse;
};
std::vector<Retired> retired;

retired.push_back({oldBuffer, frame10Point}); // stop using it, but do not destroy yet

// Each frame: destroy whatever the GPU is done with.
std::erase_if(retired, [&](Retired &r) {
    if (!RI::isReached(queue, r.lastUse)) {
        return false;
    }
    RI::destroyBuffer(device, r.buffer);
    return true;
});
```

Since points only grow, the oldest entries are reached first: in a list kept in order you
can stop at the first one that is not reached.

### Frames in flight

To keep the GPU busy, the CPU records frame n + 1 while the GPU runs frame n. Anything the
CPU writes every frame (a command list, an upload buffer of per-frame constants) then needs
**one copy per frame in flight**: copy i is touched again only once the point of the frame
that last used it is reached. With two copies, the CPU waits only when it gets two frames
ahead. The [frame ring](../helpers/frame-ring.md) helper does exactly this.

## Helpers that do the bookkeeping

The rule is simple but repetitive. The helpers take points so you do not have to track them
([P41](../design-notes/decisions.md#p41)):

| Helper | What it does with points |
|---|---|
| [Frame ring](../helpers/frame-ring.md) | `endFrame` returns the frame's point; `beginFrame` waits for it only when it reuses that frame's list. |
| [Slot allocator](../helpers/allocators.md) | `releaseSlot(slots, slot, lastUse)`: the slot is handed out again only once `lastUse` is reached. |
| [Memory allocator](../helpers/allocators.md) | `release(allocator, allocation, lastUse)`: the range is reused only once `lastUse` is reached. |

```cpp
--8<-- "snippets/ri/helpers.cpp:release"
```

## Things that trip people up

- **A point belongs to one queue.** Ask about it on the queue whose `submit` returned it.
  In this version there is only `device.graphicsQueue`
  ([P37](../design-notes/decisions.md#p37)); with more queues, each will have its own
  counter, and `{5}` on one queue says nothing about another.
- **Do not make up points.** Only use points that `submit` returned (or `{0}`). Waiting for a
  value no submission will ever signal blocks forever: `wait` has no timeout in this version.
- **`wait` right after every `submit` is correct, but slow.** It is the easy way to start;
  switch to keeping points and `isReached` (or the frame ring) when the CPU and GPU should
  overlap.
- **A swapchain image cycle is not frames in flight**
  ([P43](../design-notes/decisions.md#p43)): `acquire` and `present` synchronise the
  images, not your own per-frame data. That still needs points.
- **Shutting down:** before destroying anything, wait for the last point you got (the latest
  one covers all earlier ones), then destroy in any order the objects themselves allow.
- **A lost device** (driver crash, GPU reset) makes `wait` and `isReached` report a
  **device lost** error through the [error channel](error-channel.md).

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

This is the one case RI checks for you: in validation builds, resetting or destroying a list
the GPU may still be executing is a **programmer error**: `resetting a command list the GPU
may still be executing: wait for its timeline point (value 3) first; the queue has reached 2`.
For buffers, textures, descriptors and the rest, RI does not check the rule; the native
[debug layers](debugging.md) catch some of these mistakes, not all.

## The same in native code

Natively, the timeline is a Vulkan **timeline semaphore** and a D3D12 **fence**: both are a
64-bit counter the GPU raises, as above. RI creates one per queue, picks the next value at each
`submit`, and gives it back to you as a `TimelinePoint`.

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
- [P37](../design-notes/decisions.md#p37): one graphics queue in v1; points are per queue.
- [P41](../design-notes/decisions.md#p41): frames in flight and deferred frees are helpers.
- [P43](../design-notes/decisions.md#p43): swapchain image cycling is not frames in flight.
- [P56](../design-notes/decisions.md#p56): one recording thread; command lists reused only
  after their point.
- [P59](../design-notes/decisions.md#p59): no overwriting a descriptor slot in-flight work
  may read.
- [P63](../design-notes/decisions.md#p63): no resubmission in v1.
- [P94](../design-notes/decisions.md#p94): `submit`, `wait`, `isReached`; the reuse check.
- [P106](../design-notes/decisions.md#p106): the calls that may run on other threads.

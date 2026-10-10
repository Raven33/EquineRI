---
symbols: [RI::submit, RI::wait, RI::isReached, RI::TimelinePoint, RI::Queue]
---

# RI::submit, RI::wait, RI::isReached

## What it does

- `RI::submit(queue, cmd)` sends a closed [command list](commandList.md) to the GPU and
  returns at once with a **timeline point**: the value the queue's timeline reaches when the
  GPU has finished this list ([P20](../design-notes/decisions.md#p20)).
- `RI::wait(queue, point)` blocks the CPU until the point is reached.
- `RI::isReached(queue, point)` tells, without blocking, whether it is.

The queue is the device's `device.graphicsQueue`, the only queue in this version
([P37](../design-notes/decisions.md#p37)). Points of one queue only grow: each submit returns
a larger value, and reaching a point means every earlier submission is finished too. The
default point `{0}` is always reached.

Timeline points are how you know when you may read results, reuse a command list, overwrite a
descriptor, or destroy an object: see [Timeline points](../concepts/timeline.md).

## Signature

--8<-- "generated/signatures/RI.submit.md"

--8<-- "generated/signatures/RI.wait.md"

--8<-- "generated/signatures/RI.isReached.md"

--8<-- "generated/signatures/RI.TimelinePoint.md"

--8<-- "generated/signatures/RI.Queue.md"

## Example

```cpp
--8<-- "snippets/ri/compute.cpp:submit-wait"
```

```cpp
--8<-- "snippets/ri/compute.cpp:read-back"
```

Polling instead of blocking:

```cpp
--8<-- "snippets/ri/compute.cpp:poll"
```

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

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:is-reached"
    ```

    What to watch for: the signal value goes in the submission itself
    (`VkSemaphoreSubmitInfo`, synchronization2). Values must strictly increase.

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

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:is-reached"
    ```

    What to watch for: `Signal` is a separate queue command after `ExecuteCommandLists`.
    `SetEventOnCompletion` with a null event blocks until the value is reached.

## Rules and errors

- Submit a list only after [`cmdEnd`](cmdBegin.md), and only once per recording.
- The device being lost while waiting or polling reports a **device lost** error.
- In validation builds, submitting a destroyed list is a **programmer error**.
- `wait` has no timeout in this version.

## Related decisions

- [P20](../design-notes/decisions.md#p20): per-queue timeline points; no frame concept.
- [P37](../design-notes/decisions.md#p37): one graphics queue in v1.
- [P56](../design-notes/decisions.md#p56): lists are reused after their point.
- [P94](../design-notes/decisions.md#p94): submission (7).

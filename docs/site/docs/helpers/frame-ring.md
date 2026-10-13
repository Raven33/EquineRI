# Frame ring

## What it does

`<RI/Helpers/FrameRing.hpp>` keeps N command lists (two by default) and uses them in turn,
so the CPU records frame n + 1 while the GPU runs frame n ([P20](../design-notes/decisions.md#p20)).

- `beginFrame` waits only when the list it reuses is still in flight, resets it and begins
  recording.
- `endFrame` ends it, submits it on the graphics queue and returns its `TimelinePoint`.
- `frameIndex(ring)` (0 .. N - 1) picks your per-frame copy of anything the CPU writes
  while the GPU may still read the previous one (upload buffers, root data arrays).

Swapchain acquire and present stay yours ([swapchain](../reference/swapchain.md)).

## Example

```cpp
--8<-- "snippets/ri/helpers.cpp:frame-ring"
```

## Shutdown

`destroyFrameRing` waits for every frame still in flight, then destroys the lists. Call it
**before** destroying anything the frames used (pipelines, buffers, textures, heaps): they
may only be destroyed once the GPU is done with them ([P20](../design-notes/decisions.md#p20)).

```cpp
--8<-- "snippets/ri/helpers.cpp:frame-ring-shutdown"
```

Anything you replace while frames are in flight, such as a depth texture after the window
was resized, follows the same rule: wait for the points `endFrame` returned (or keep the old
one until then) before destroying it.

## Rules and errors

- The ring keeps a pointer to the `Device` you created it with: the device must stay at the
  same address, and alive, until `destroyFrameRing`. Do not create the ring from a local
  `Device` that you then copy or move elsewhere.
- `beginFrame` twice without `endFrame`, or `endFrame` without `beginFrame`, is a
  **programmer error**.

## How to roll your own

An array of N `CommandList` and their last `TimelinePoint`: before reusing list i,
`wait(queue, points[i])`, then `cmdBegin`; after `cmdEnd`, `points[i] = submit(queue, list)`.

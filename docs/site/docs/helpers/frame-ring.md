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

## How to roll your own

An array of N `CommandList` and their last `TimelinePoint`: before reusing list i,
`wait(queue, points[i])`, then `cmdBegin`; after `cmdEnd`, `points[i] = submit(queue, list)`.

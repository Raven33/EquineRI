# GPU profiler

## What it does

`<RI/Helpers/Profiler.hpp>` measures named, nested scopes on the GPU ([P51](../design-notes/decisions.md#p51)).
`beginScope` and `endScope` write a timestamp each and also open and close a marker region,
so the scopes show in RenderDoc, PIX and Nsight ([debugging](../reference/debugging.md)).

Per frame: `resolveProfiler` before `cmdEnd` copies the timestamps to readback memory;
once the frame's point is reached, `profilerResults` returns each scope's name, depth and
milliseconds in begin order, and resets the profiler for the next frame. Up to
`maxScopes` (256 by default) per frame.

One profiler serves one frame at a time: with several frames in flight, keep one per frame
(index them with `frameIndex`).

## Example

```cpp
--8<-- "snippets/ri/helpers.cpp:frame-ring"
```

## How to roll your own

A timestamp `QueryPool` of 2 × scopes; `cmdWriteTimestamp` at each begin and end;
`cmdResolveQueries` into a readback buffer; after the point, convert ticks with
`timestampFrequency(queue)`. Add `cmdBeginMarker` / `cmdEndMarker` for the tools.

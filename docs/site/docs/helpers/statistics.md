# Pipeline statistics

## What it does

`<RI/Helpers/Statistics.hpp>` reads one [pipeline-statistics query](../reference/queries.md)
into a fixed struct, the same on both backends ([P71](../design-notes/decisions.md#p71)).

Vulkan and D3D12 write the counters in different orders, and RI reports where each one is
(`pipelineStatisticsLayout`) without converting them. `readPipelineStatistics(layout, result)`
does the conversion on the CPU: give it the layout and a pointer to one query's result, as
`cmdResolveQueries` copied it into readback memory, once the timeline point is reached. Each
field of `PipelineStatistics` holds one counter; a stage the GPU lacks reads 0.

It runs nothing on the GPU and keeps no state.

## Example

```cpp
const RI::PipelineStatisticsLayout layout = RI::pipelineStatisticsLayout(device);
// ... cmdBeginQuery / cmdEndQuery around the draws, cmdResolveQueries into `results`,
// submit, then wait for the point ...
const RI::Helpers::PipelineStatistics stats = RI::Helpers::readPipelineStatistics(layout, results.mapped);
std::printf("%llu pixel shader runs\n", static_cast<unsigned long long>(stats.pixelShaderInvocations));
```

## How to roll your own

Each counter is a 64-bit value at `result + layout.<counter>`; a counter whose offset is
`kNoCounter` is missing on this GPU. Copy the ones you need with `std::memcpy`.

# Statistics-cost measurement (P105)

The question (`docs/FeatureProposal-Sandbox-2026-10-15.md`, part 2, "Cost"): what do
pipeline-statistics queries cost on the GPU, and does adding the task and mesh counters,
or the mesh-primitives query, cost more? The user saw a loss in vulkan_sandbox with
statistics queries on Vulkan.

Harness: `tools/statistics_bench` (`statistics_bench_Vulkan`, `statistics_bench_D3D12`).
A frame is 6 mesh-shader passes (512 task groups x 32 meshlets x 98 triangles each,
1920x1080 color and depth) and one compute pass, with one query around each pass. The
variants run interleaved frame by frame (P96). The frame's GPU time comes from two
timestamps, and every variant also copies its results to readback memory.

| Variant | Vulkan | D3D12 |
|---|---|---|
| A | no query | no query |
| B | pixel + compute invocations (the counters allowed with mesh draws) | `PIPELINE_STATISTICS` |
| C | B + task and mesh invocations | `PIPELINE_STATISTICS1` |
| D | C + the mesh-primitives query on mesh passes | (in C) |
| E | compute invocations only (the sandbox's choice) | — |

"Clearly more" uses P79's frame thresholds: over 2% and over 0.05 ms.

## Finding before any timing: Vulkan forbids RI's counters around mesh draws

A statistics query must not be active during a mesh draw if its pool counts input assembly,
vertex, geometry, tessellation or clipping
(`VUID-vkCmdDrawMeshTasksEXT-pipelineStatistics-07076`). RI's statistics pools count all of
those, so RI's statistics queries cannot be used around mesh draws on Vulkan today. The
validation layer reports it; release builds would be undefined behavior. D3D12 has no such
rule: the vertex counters just stay 0.

## Results

| GPU | Backend | A (ms) | B | C | D | E | Run |
|---|---|---|---|---|---|---|---|
| AMD Radeon 680M (RADV, Mesa 26.2.2, Linux) | Vulkan | 6.692 | −0.08% | n/a | n/a | −0.17% | `radv-680m-linux.txt` |
| RTX 3080 (Windows) | Vulkan, D3D12 | | | | | | not run yet |
| AMD RDNA2 iGPU (Windows) | Vulkan, D3D12 | | | | | | not run yet |

RADV does not report `meshShaderQueries`, so C and D cannot run there; RI's mesh counters
would read `kNoCounter` on it. On RADV no variant costs anything measurable.

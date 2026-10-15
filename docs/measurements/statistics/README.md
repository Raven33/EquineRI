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
| RTX 3080 (Windows, driver 617.42) | Vulkan | 1.567 | +2.3% | **+6553%** (104.2 ms) | +6519% | +2.2% | `windows_Vulkan_nvidia.txt`  |
| RTX 3080 (Windows) | D3D12 | 1.574 | +4.4% (0.07 ms) | **+130%** (3.6 ms) | (in C) | n/a | `windows_D3D12_nvidia.txt` |
| AMD RDNA2 iGPU (Windows) | Vulkan, D3D12 | | | | | | no mesh shaders on this GPU: nothing to measure |

RADV does not report `meshShaderQueries`, so C and D cannot run there; RI's mesh counters
would read `kNoCounter` on it. On RADV no variant costs anything measurable.

On the RTX 3080, **the task and mesh counters are what costs**: the Vulkan frame goes from
1.6 ms to 104 ms (65x), and the D3D12 frame from 1.6 ms to 3.6 ms. The counters that are
allowed with mesh draws (pixel, compute) cost about 2% on Vulkan, and D3D12's classic
statistics cost 4.4% (0.07 ms, just over P79's thresholds).

### Which counter (RTX 3080, Vulkan, `4add8cd`, `windows_Vulkan_nvidia_4add8cd.txt`)

| Variant | Median (ms) | vs B |
|---|---|---|
| A: no query | 1.575 | |
| B: pixel + compute | 1.619 | |
| F: B + mesh-primitives query | 1.622 | +0.2%, free |
| G: B + task invocations | 3.199 | **+98%** (+1.6 ms) |
| H: B + mesh invocations | 102.503 | **+6232%** (+101 ms) |
| C: B + task + mesh invocations | 103.326 | as H |
| D: C + mesh-primitives query | 103.921 | as C |
| E: compute only | 1.617 | B costs the same |

**The mesh-invocations counter alone is the 100 ms**, the task counter costs 1.6 ms, and
the primitive count is free. On D3D12 the three come together in `PIPELINE_STATISTICS1`
(+130%), so the primitive count cannot be had alone there. Validation runs of F, G and H on
the RTX 3080 were clean.

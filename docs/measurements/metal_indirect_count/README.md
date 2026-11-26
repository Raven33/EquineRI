# GPU-written indirect counts on Metal: RI's path against ICBs (P18, P116 (3))

The question (P116 (3), proposal §7 (a)): Metal has no draw count. RI's path copies the
`maxCount` records into command memory, empties those at or beyond the GPU-written count, and
encodes `maxCount` indirect draws on the CPU. P116 (3) asked to measure that against indirect
command buffers (ICBs) encoded on the GPU.

Harness: `tools/indirect_count_bench` (native Metal 4 through metal-cpp; build it in a tree with
`-DEQUINE_BUILD_SAMPLES=ON`, as `tools/metal_view_bench`). Level 2 records (16 bytes of root data,
then `MTLDrawPrimitivesIndirectArguments`), drawn in one render pass on a 1920 x 1080 target,
each record lighting its own pixel; every variant is checked to light exactly `count` pixels.
Times: CPU time to encode the pass; GPU time of the command buffer (commit feedback); median of
20 runs after 3. Results: `m4-macos27.txt` (Apple M4, macOS 27, 2026-11-26, no Metal
validation); three runs agreed except the first run's smallest rows (warm-up).

- **A, RI's path:** the count copy as a vertex function with rasterization off (RI cannot
  dispatch compute inside a section), then `maxCount` indirect draws, each pointing
  argument-table indices 2 and 4 at its record.
- **B, ICB:** a vertex function encodes `maxCount` ICB commands, binding what an RI command
  binds (both heaps, root data, draw arguments, index type; vertex and fragment stages) and
  resetting the commands past the count; one `executeCommandsInBuffer` over `maxCount`.
- **B2, ICB with a GPU-written range:** the same function encodes only the first `count`
  commands and writes the execution range, which `executeCommandsInBuffer` reads from GPU memory.

| maxCount | count | A cpu | A gpu | B cpu | B gpu | B2 cpu | B2 gpu |
|---|---|---|---|---|---|---|---|
| 256 | 2 | 0.023 | 0.043 | 0.006 | 0.047 | 0.006 | 0.046 |
| 256 | 256 | 0.029 | 0.045 | 0.006 | 0.052 | 0.007 | 0.054 |
| 4096 | 40 | 0.316 | 0.123 | 0.006 | 0.089 | 0.006 | 0.048 |
| 4096 | 4096 | 0.320 | 0.134 | 0.006 | 0.184 | 0.006 | 0.184 |
| 65536 | 655 | 4.877 | 1.396 | 0.014 | 0.912 | 0.006 | 0.090 |
| 65536 | 32768 | 4.805 | 1.621 | 0.028 | 1.947 | 0.028 | 1.375 |
| 65536 | 65536 | 4.801 | 1.798 | 0.027 | 2.742 | 0.028 | 2.677 |

(milliseconds)

What it says:

- **CPU:** RI's path costs about 73 ns per record of `maxCount`, whatever the count (4.8 ms for
  65,536). ICBs cost a few microseconds, constant.
- **GPU, many records drawn:** RI's indirect draws are cheaper than ICB commands: about 27 ns
  against 41 ns per drawn record (1.8 against 2.7 ms at 65,536 of 65,536).
- **GPU, few records drawn:** RI's empty records still cost about 21 ns each, reset ICB commands
  about 13 ns, and B2's skipped ones nothing: 1.40, 0.91 and 0.09 ms for 655 of 65,536.
- **Shader validation:** under `MTL_SHADER_VALIDATION=1` (RI's `macos-debug` preset), B2 draws
  nothing (the GPU-written range is not honoured); A and B stay correct. `MTL_DEBUG_LAYER=1`
  alone reports nothing for any variant.

So neither path wins everywhere: ICBs remove the CPU cost (which dominates from a few
thousand records on) and win on the GPU when the count is small against `maxCount` (culling);
RI's path wins on the GPU when most records are drawn.

**Decision (user, 2026-11-26): keep RI's path.** No ICB path is built. Metal native sections
(P27, P70), when built, must let user code run its own ICBs inside a section instead (P116 (3)).

What an ICB path in RI would have needed: pipelines created with
`supportIndirectCommandBuffers` (its own cost unmeasured), an ICB of `maxCount` commands per
call (memory RI does not own today, P31: user scratch through `indirectScratchRequirements`, or
command memory, P115), a mesh-draw and a dispatch variant, and B rather than B2 while shader
validation is on, since B2 breaks under it.

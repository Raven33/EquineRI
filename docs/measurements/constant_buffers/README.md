# P112: push-descriptor constant buffers (2026-11-05)

`tools/constant_bench` reads the same 64-byte constant blocks three ways on one Vulkan device:

- **push**: a uniform buffer in a push-descriptor set, which is how bindful slots work (`cmdSetConstantBuffer`);
- **heap**: an index in root data into a uniform-buffer array bound once, the heap-indexed way RI already had;
- **root**: the block in the push constants themselves.

Rounds alternate the modes, and every mode's results are checked equal to push before timing.

| Measure | push | heap | root |
|---|---|---|---|
| Recording one dispatch with its own block (CPU, ns) | 245–251 | 128–134 | 175–186 |
| 4096 one-group dispatches (GPU, ms) | 0.085–0.158 | 0.083–0.153 | 0.080–0.152 |
| One large dispatch reading its block 256 times per thread (GPU, ms) | 0.945–0.950 | 0.986–0.992 | 0.865–0.867 |

Machine: Radeon 680M, RADV (Mesa 26.2.5), `clang-bench` build, 3 runs of 15 rounds (`radv_680m_run*.csv`).
In run 3 the GPU clock was higher for the whole run; the order between the modes holds within each run.
Runs with 3 rounds put push at 2.7 ms in the large dispatch: that was warm-up, gone at 15 rounds.

Reading the numbers:
- On the GPU, a pushed constant buffer reads as fast as a heap-indexed one. Each dispatch costs about 1.5 ns more.
- On the CPU, a push costs about 120 ns more than writing a heap index. At a few hundred binds per frame, that is well under 0.1 ms.

**Outcome on RADV:** push descriptors stay the Vulkan mechanism (P112).
NVIDIA and AMD Windows are not measured yet. The `windows-equineri` session runs the same binary on both GPUs.

# P94: descriptor sets against VK_EXT_descriptor_heap (2026-10-15)

`tools/descriptor_bench` runs the same compute shaders through RI's Vulkan descriptor heap
(descriptor sets with update-after-bind mutable descriptors) and through
`VK_EXT_descriptor_heap`. Two devices on one GPU, interleaved rounds, identical contents;
the results of both are compared and were identical in every run. Shaders come from glslang,
because DXC cannot compile heap access to buffers or writable resources yet. Columns below
are heap time divided by sets time (above 1: the extension is slower).

| Measure | RTX 3080 (NVIDIA 617.42, 5 runs) | Radeon 680M (RADV 26.2.2, 7 runs) |
|---|---|---|
| Random texture loads | 2.35–2.49 | 1.00–1.05 (2 runs: 3.9–4.0) |
| Wave-uniform texture loads | 3.63 | 0.97–1.02 (2 runs: 4.2–4.4) |
| Random buffer loads | 1.74 | 0.99–1.06 |
| Random texture + sampler | 2.35–2.51 | 0.98–1.00 (2 runs: 3.4–3.7) |
| Writing descriptors (CPU) | 12.9–13.9 (84 ns against 6.5 ns) | 8.0–8.6 (99 ns against 12 ns) |
| Recording pipeline, root data, dispatch (CPU) | 0.96–1.02 | 0.81–0.99 |

The two slow RADV runs (`radv_680m_run1.csv`, `run2.csv`) were the first after the build and
were not reproduced in ten later runs; the cause is unknown.

**Outcome:** the extension wins nowhere and loses clearly on NVIDIA, so P94's rule ("adopted
if it wins and every target driver and SDK has it") keeps descriptor sets. It is also not
available: the AMD Windows driver (26.8.1) lacks the extension, and DXC 1.9.2609 cannot
compile for it. Worth measuring again when the drivers and DXC mature.

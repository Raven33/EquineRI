# The cost of pixel-format views on Metal (P74, P116)

The question: RI's Metal backend copies textures between formats through a buffer, which
multisampled textures cannot use. A direct copy needs a view of one texture in the other's
format, which Metal allows only for textures made with `MTLTextureUsagePixelFormatView`. What
does that flag cost, if RI set it on every texture that copies?

Harness: `tools/metal_view_bench` (native Metal through metal-cpp; build with
`-DEQUINE_BUILD_SAMPLES=ON`, e.g. `cmake --preset macos-debug -B build/bench
-DCMAKE_BUILD_TYPE=Release -DEQUINE_BUILD_SAMPLES=ON -DEQUINE_BUILD_TESTS=OFF
-DEQUINE_VALIDATION=OFF`, target `metal_view_bench`). The same work on textures with and
without the flag, 3840 x 2160: eight render passes that load, blend over and store the target
(memory traffic, where lossless compression acts), a resolve-only pass (4x), and a compute pass
reading every sample; for D32Float_S8, eight passes that load, write and store depth and
stencil. Median GPU time of 30 frames after 3. Results: `m4-macos27.txt` (Apple M4, macOS 27,
2026-11-26); three runs agreed within 1 %, except the first scratch run's 1x RGBA8 row
(warm-up).

| Target | Passes without | with the flag | Ratio |
|---|---|---|---|
| RGBA8Unorm 4x | 6.06 ms | 18.65 ms | 3.1 |
| RGBA16Float 4x | 14.42 ms | 38.21 ms | 2.6 |
| RGBA8Unorm 1x | 2.21 ms | 4.70 ms | 2.1 |
| RGBA16Float 1x | 4.04 ms | 9.42 ms | 2.3 |
| D32Float_S8 4x | 14.19 ms | 14.31 ms | 1.0 |
| D32Float_S8 1x | 4.18 ms | 4.16 ms | 1.0 |

Resolves (4x) and sample reads cost 1.6 to 2.7 times as much with the flag, too.

**Decision (assistant, for the user's review; CONTEXT.md P116):** RI does not set the flag for
copies. A copy between formats is direct when one of the two textures already has it (it lists
`viewFormats`); otherwise 1x copies are staged as before, and multisampled ones report
`eUnsupported`. The flag RI sets on sampled depth-stencil textures (for their stencil views)
costs nothing measurable. `viewFormats`' documentation now says what an entry costs on Metal.

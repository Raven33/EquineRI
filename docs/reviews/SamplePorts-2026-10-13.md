# Sample-port review, pilot batch (2026-10-13)

Review of the pilot in `Raven33/EquineRI-Samples` at `9b171f5` (RI pinned at `c777bc8`): the
quiz, the triage, four ports (triangle, texture, computeparticles, shadowmapping) and the
friction log, following `docs/SamplePortReview.md`. The pilot agent ran on Windows (RTX 3080);
this review rebuilt and ran the ports on Linux (Vulkan, RADV 680M, validation on).

## Counts

| Measure | Result |
|---|---|
| Quiz | 6 correct, 2 partly correct, 0 wrong |
| RI implementation files read to learn usage | 0 backend files; 1 helper file (`helpers/core/FrameRing.cpp`, which found a real gap) |
| Friction entries logged | 11 |
| Further problems found by this review | 8 |
| Triage verdicts wrong | 10 of 99 (below) |

Friction entries and review findings per class:

| Class | Logged | Found in review | Action |
|---|---|---|---|
| Doc wrong | 3 | 1 (ten more stale "later" notes) | Fixed |
| Doc missing | 5 | 2 (unread root data is stripped; no "not in RI" list) | Fixed |
| Doc unclear or hard to find | 2 | 1 (shutdown order hidden by the snippets) | Fixed |
| RI defect | 1 | 0 | Waiting for the user |
| API gap | 0 | 1 (depth bias; see also the triage) | Waiting for the user |
| Agent error | 0 | 3 (+ the code violations below) | Noted |

## Quiz

1. **Partly correct.** Right core (layouts hidden, usage flags, one barrier after the section),
   but says a sampled render target "stays in its rest layout for the whole frame";
   `concepts/layouts.md` says plainly that `cmdBeginRendering` switches it to `RENDER_TARGET`
   on D3D12. Agent misread.
2. Correct.
3. Correct.
4. Correct, with an invented detail ("BC block history on tiled GPUs") no page says.
5. **Partly correct.** Says a barrier cannot be recorded inside a section "on Vulkan";
   `concepts/rendering.md` says barriers belong outside sections on both backends.
6. Correct (the `PROFILE` keyword of `equine_add_shaders` is dropped in the answer, but used
   correctly in the build).
7. Correct, but cites `concepts/pipelines.md`, which does not exist.
8. Correct.

Citations to pages that do not exist appear across the deliverables:
`concepts/pipelines.md`, `concepts/limits.md`, `concepts/raytracing.md`,
`getting-started/first-steps.md`, `reference/backend.hpp`, `reference/cmdDrawIndexed.md`. They
are agent errors, but they show where agents expect pages.

## Friction log, entry by entry

| # | Entry | Class | Fix |
|---|---|---|---|
| 1 | `queryPool.md` says occlusion and statistics queries "come later" | Doc wrong | Removed; links to `queries.md` |
| 2 | `cmdDraw.md` says indexed and indirect draws "come later" | Doc wrong | Removed; links to `indirect.md` |
| 3 | No guidance for loading image files | Doc missing | `helpers/transfers.md`: RI reads no image files; decode, then upload per mip |
| 4 | Point size does not exist on D3D12 | Doc missing | `graphicsPipeline.md`: points and lines are one pixel; draw quads |
| 5 | The frame ring keeps a `Device *`; a copied `Device` crashed | Doc missing | Rule on the frame-ring, allocator, profiler and helpers pages, and on the three `create*` declarations |
| 6 | Vertex-to-pixel values must match by register on D3D12 | Doc missing | `concepts/shaders.md`, "HLSL that works on both backends" |
| 7 | `bufferRequirements` "typical values" (16 to 256 bytes on Vulkan) | Doc wrong | RI rounds to `bufferImageGranularity` (1 KiB on NVIDIA); how to lay out a block |
| 8 | Constant buffer views round up to 256 bytes | Hard to find | The rule was in `writeDescriptors.md`; now also in `buffer.md`, where the agent looked |
| 9 | D3D12: two validator warnings per frame from RI's swapchain | RI defect | Fixed 2026-10-15 (`8d64689`) |
| 10 | Root data packing between HLSL and C++ | Doc unclear | `root-data.md`: a vector never crosses a 16-byte boundary; scalars; `static_assert` |
| 11 | Matrices column-major; clip-space depth 0 to 1 | Doc missing / hard to find | `concepts/shaders.md`: majorness, and a link to the clip-space section of `rendering.md` (depth 0 to 1 was there, under "Clip space points up") |

## Found in review

- **Ten more stale "later" notes** (Doc wrong): texture copies (`textures.md`, `texture.md`,
  `textureRequirements.md`), indirect arguments (`buffer.md`, `barriers.md`, `cmdBarrier.md`),
  root data for draws (`root-data.md`), native sections (both `objects.md`), the run-time
  compiler (`shaders.md`), and the "Not yet" section of `timestamps.md`. All replaced by links
  to the existing pages.
- **Unread root data counts as none** (Doc missing): a shader that declares `EQ_ROOT_DATA`
  but never reads it gets `rootDataSize` 0 (checked on Vulkan with a probe), so setting root
  data for it is an error. The agent worked around it by adding the slot to `w` in
  `shadow_clear_vert.hlsl`. Now in `root-data.md` and `graphicsPipeline.md`.
- **No list of what RI lacks** (Doc missing): the glossary had "—" rows only for what RI hides.
  Quiz answer 8 sends agents there, and the triage errors below follow from it. Added
  "Not in RI v1": cube and 3D textures, compressed formats, geometry and tessellation, depth
  bias, wireframe, line width and point size, alpha to coverage and similar, blits and mip
  generation, specialization constants, multiview, several recording threads, more queues,
  ray tracing pipelines, each with what to do instead.
- **Shutdown order** (Doc unclear): every port destroys pipelines and buffers *before*
  `destroyFrameRing` waits, so the three that reach their teardown abort on exit
  (`vkDestroyPipeline(): ... currently in use`, reproduced on Linux). The swapchain snippet's
  final `wait` was outside the region the page shows, and the frame-ring page said nothing
  about shutdown. Both fixed (`helpers.cpp:frame-ring-shutdown`, the `frame` region of
  `swapchain.cpp`).
- **Depth bias** (API gap): the shadowmapping port drops `vkCmdSetDepthBias` for a shader
  epsilon.
- **"D16 clears write NaN"** (Agent error): the shadowmapping code cites "FRICTION #14", which
  is not in the log. A probe clears a D16 texture to 1.0 and 0.5 and reads back `0xffff` and
  `0x8000` on Vulkan; the D3D12 code passes the float to `ClearDepthStencilView` unchanged. The
  agent most likely read the 16-bit texels as floats. Not checked on D3D12.
- **Overstated run status** (Agent error): the READMEs say the ports run cleanly, but
  `texture` renders nothing (its matrices are still transposed, as the agent itself noted),
  `triangle` exits after three frames into a debug dump, and no port was closed normally.

## Ported code against RI's rules

| Rule | Result |
|---|---|
| Barriers between dependent work | Right in all four (copy → vertex, depth → pixel, compute → vertex), except one: computeparticles reads the previous frame's output without an `eCompute → eCompute` barrier at the start of the list (`barriers.md` covers earlier submissions). |
| `cmdSetHeap` before `cmdSetPipeline`; root data sizes | Right. |
| Draws in sections; dispatches and copies outside | Right. |
| Descriptor slots not overwritten in flight | Right (all written once at load). |
| Destroy only after the timeline point | **Violated in all four** at exit (above), and in shadowmapping on resize: the window depth texture is destroyed while the previous frames may still use it. |
| `cmdDiscard` after reuse | Not needed (no aliasing). |
| Swapchain and resize | Right. |
| Frames in flight | Right: per-frame UBOs and staging; four particle buffers. |
| Validation | Silent on Linux while running (6 s each); errors at exit (above). |
| No edits under `external/EquineRI`; shaders through `equine_add_shaders` | Right. |
| Finished code | Debug code left in triangle (exits after three frames, writes `C:\tmp\...`) and shadowmapping (`TEMP` probes, `eCopySrc` flags); no README for shadowmapping. |

## Triage

Verdicts were checked against the original sources. Wrong verdicts:

- **"port", but needs cube maps:** hdr, pbrtexture, vulkanscene (skybox); bloom is listed as
  "adapt" because of subpasses it does not use, and needs a cube map too.
- **"port", but needs depth bias:** shadowmapping (pilot) and deferredshadows, which also uses a
  geometry shader for its layered shadow maps.
- **"port", but needs point sprites:** computenbody.
- **"port", but needs wireframe:** pipelines (one of its three pipelines).
- **Wrong facts about the native APIs:** geometry shaders "not at all in Vulkan"; D3D12 "has
  no" conservative rasterization, barycentrics or multiview (it has all three). These verdicts
  stay "skip" for RI, but the reasons are wrong.

Every KTX file the samples load is uncompressed RGBA (`*_rgba.ktx`), so compressed formats block
nothing; the real cost is loading glTF models and KTX files at all, which the pilot replaced
with procedural data.

## Fixed in RI

All the documentation fixes above, plus header comments on `createFrameRing`,
`destroyFrameRing`, `createMemoryAllocator` and `createProfiler`. The strict docs build passes
(code-documentation check: 0 problems), the snippets compile, and the Linux tests pass (86/86).
No signature changed.

## Added after the review

The user asked for the vertex-to-pixel rule to be caught at validation (P101):
`createGraphicsPipeline` now matches the pixel shader's inputs to the previous stage's outputs
by semantic and slot, on both backends, and `concepts/shaders.md` gives the rules.

## Waiting for the user

Ranked by samples affected:

1. **The original assets.** Most remaining "port" samples load glTF models and KTX textures.
   Should the samples repository fetch Sascha Willems' assets and use small loaders (for
   example cgltf and libktx), or keep procedural stand-ins? This is a samples-repository
   question, not an RI one.
2. **Cube textures** (8 samples: bloom, hdr, pbribl, pbrtexture, shadowmappingomni,
   texturecubemap, texturecubemaparray, vulkanscene).
3. **Ray tracing pipelines** (8 samples): out of v1 by P49.
4. **Geometry and tessellation shaders** (6 samples).
5. **Depth bias** (3 samples: shadowmapping, deferredshadows, shaderobjects).
6. **Wireframe** (pipelines, and optional toggles in about six others).
7. **The D3D12 swapchain warnings** (every D3D12 sample with a window): `acquire` and
   `present` each execute a command list holding only a barrier, and the D3D12 debug layer
   warns about it twice per frame. A backend fix, no API change, to be verified on Windows.
   **Fixed 2026-10-15** (`8d64689`): back buffers rest in `COMMON`, which is `PRESENT`, so
   `acquire` and `present` submit nothing; verified on both Windows GPUs.

The user stopped the sample ports on 2026-10-15. Items 2, 4, 5 and 6 (and 3D textures, depth
clamp, line strips) became `docs/FeatureProposal-2026-10-15.md`; item 1 is moot.

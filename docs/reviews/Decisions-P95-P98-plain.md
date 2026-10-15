# P95 to P98 in plain words

`CONTEXT.md` records these four decisions as one dense paragraph each. This page says the
same things one point at a time, in plain words. Nothing here is new.

- **P96 and P97** are settled: they record a measurement and what you decided after it.
- **P95 and P98** are decisions the assistant made while implementing, which you have not reviewed
  yet. Under each point, **"Your call"** names what you may want to change. Points with no
  "Your call" just describe how the code works.

## Words used on this page

- **Layout:** the way a GPU arranges a texture's pixels in memory. A GPU keeps different
  arrangements for different jobs (drawing into it, reading it in a shader, copying it).
  Moving a texture from one layout to another is called a *switch*, and it takes GPU time.
- **Resting layout:** the layout a texture goes back to between jobs. RI picks it from the
  texture's usage flags. Users never see layouts (P16).
- **Barrier:** a command that says "the work before this must finish writing before the
  work after this reads". In RI you name kinds of work (copy, compute, …), never textures
  (P36).
- **Rendering section:** the commands between `cmdBeginRendering` and `cmdEndRendering`,
  where draws happen.
- **Resolve:** turning a multisampled (MSAA) texture into a normal one.

---

## P95: textures and rendering (Milestone 3), not reviewed yet

### 1. Where textures rest

- **D3D12:** a texture that shaders read or write, or that copies use, rests in
  `DIRECT_QUEUE_COMMON`. That one layout allows all of those jobs. A texture used only as a
  render target or only as depth rests in its drawing layout.
- **Vulkan** has three modes, chosen per device:
  - **Unified:** newer drivers have one layout that is good for everything, so RI never
    switches. RI uses it when the driver offers it.
  - **T:** otherwise. Like D3D12, each texture has its own home: storage textures in
    `GENERAL`, sampled textures in the read layout, render targets in their drawing layout.
  - **G:** everything in `GENERAL`. Measured as very slow on AMD (P96), so RI never picks it
    on its own; you can force it for testing.

### 2. When RI switches

- RI switches a texture at the start and end of a rendering section, and around a clear.
  Nowhere else.
- When a section clears an attachment, or says it does not care about the old content, RI
  tells the GPU the old content can be thrown away. That is faster. RI does this because it
  is evident from what you asked (P36 allows it).
- A new texture is set up the same way, once, when it is created.

*Your call:* none, unless you want RI never to throw content away on its own.

### 3. What a barrier covers

- `cmdBarrier` covers everything a texture can do *while at rest*: shader reads and writes,
  copies, clears, constant buffers, and indirect arguments.
- It does not cover drawing. RI's own switch at the end of a section already handles that.
- D3D12 forces this: its "global" barriers cannot change layouts.

### 4. Rendering sections

- The viewport and the scissor cover the whole of the smallest attachment. There is no way
  to set a smaller viewport.
- Vulkan's viewport is flipped so that "up" in clip space is the same as on D3D12.
- A section can use a depth texture **read-only**: depth testing works, writing depth is
  an error, and shaders may sample the same texture (with the flag from P97).

**Decided (2026-10-15, P103):** the attachments stay the default, and `cmdSetViewport` and
`cmdSetScissor` override them inside a section.

### 5. Graphics pipelines

At the time, pipelines were the minimum the measurement needed. P98 (4) completed them.
Nothing to review here.

### 6. Views

- You create views yourself. On D3D12, RI keeps the render-target and depth descriptors in
  small hidden heaps of its own.
- Using a view format not listed in the texture's `viewFormats` is an error in every build,
  not only validation builds.

### 7. Clears

- P65 had planned internal compute shaders to clear textures on D3D12. Instead, RI uses
  D3D12's own clear functions.
- Storage textures and buffers need a descriptor to be cleared. RI writes it in a small
  hidden heap, then sets your heaps back.
- `cmdClearBuffer` needs a buffer with `eCopyDst` usage.
- For barriers, a clear counts as a **copy** (`Stage::eCopy`).

**Decided (2026-10-15):** clears keep counting as copies.

### 8. Timestamps

A query pool holds timestamps. `cmdWriteTimestamp` writes one after all earlier work.
`cmdResolveQueries` copies them into a buffer. `DeviceLimits::timestampFrequency` turns
ticks into seconds.

### 9. Placing resources in memory

- `MemoryRequirements::kinds` says which memory kinds can hold a resource. On D3D12,
  textures fit `eGPU` only.
- D3D12 needs 4 MiB alignment for MSAA textures.
- Vulkan alignments are rounded up so buffers and textures can share a block safely.

### 10. New checks in validation builds

- A memory range reused by a new resource must be discarded before its first use.
- Writing depth in a read-only section is an error.
- Rendering sections must be used correctly (no section inside a section, no copy inside
  one, …).
- View formats and ranges are checked.

### 11. The step that makes GPU writes visible to the CPU

On Vulkan, the CPU only sees what the GPU wrote after an extra barrier. D3D12 needs no such
step. RI adds that barrier automatically at the end of every command list, so both backends
behave the same. Measured: no measurable cost (2 µs at most).

**Decided (2026-10-15):** it stays automatic.

---

## P96: measuring RI's layouts, settled (led to P97)

- **The question:** do RI's hidden layouts make rendering slower than the best layouts a
  person would choose by hand?
- **The test:** a benchmark, `tools/layout_bench`, ran the same frame both ways on every
  GPU. The rule (P79) said RI passes if it is never noticeably slower.
- **The result:** RI passed everywhere except one case, on the AMD laptop GPU, with Vulkan,
  at 4K. A pass that reads depth was 10 to 13 % slower (0.06 to 0.08 ms). The cause: RI kept
  sampled depth in a layout meant for depth testing, which is slower to read.
- Mode G (everything `GENERAL`) was much slower on every AMD GPU, so RI never picks it.
- **Your decision:** reopen the layout question rather than accept the cost. P97 is the
  answer.

## P97: the depth fix, settled

- **What changed:** a new usage flag, `TextureUsage::eReadOnlyDepthSampling`.
  - **Without the flag (the default):** a sampled depth texture rests in the layout that
    is fastest to read. A read-only depth section can still depth-test against it, but
    shaders must not sample it during that section.
  - **With the flag:** shaders may sample the depth texture *while* it is the read-only
    depth of a section. It rests in the read-only depth layout to allow that.
- **Measured again:** RI now passes on every GPU and backend. One extra slowdown (18 %)
  turned out to come from where the texture sat in memory, not from RI: the hand-written
  version showed it too when placed the same way.
- **What this confirms:** users never see layouts (P16), and barriers name kinds of work,
  never textures (P36).

---

## P98: everything else in v1 (Milestone 4), not reviewed yet

### 1. The order of the work

Debugging, copies and queries, the rest of graphics, indirect drawing, swapchain, native
code, ray tracing, work graphs, helpers. This is only the order it was built in.

### 2. Debugging

- `setName` names objects, and markers name ranges of commands. Both show up in RenderDoc,
  PIX and the validation messages.
- When the GPU crashes, RI prints what the driver knows (Vulkan device fault, D3D12 DRED).
- In validation builds: `printf` from shaders, and GPU-side validation, which checks
  shaders for out-of-bounds access. `EQUINE_GPU_VALIDATION=0` turns the GPU-side part off.
- A marker begun but never ended is reported.

### 3. Copies and queries

- `copyLayout` says how a texture's pixels are laid out in a buffer for a copy. On D3D12
  each row is padded to 256 bytes, so the layout differs between backends.
- Occlusion queries count the pixels that passed the depth test.
- Pipeline-statistics queries count vertices, shader runs and so on. **The two backends
  lay out the counters differently.** RI tells you where each counter is
  (`pipelineStatisticsLayout`) rather than converting them.

**Decided (2026-10-15):** RI keeps reporting the offsets; the helper
`readPipelineStatistics` converts a result to one fixed struct on the CPU. Helpers may be
promoted to RI later, after the user's review.

### 4. The rest of graphics

- Added: blending, stencil, index buffers, mesh shaders.
- **MSAA color resolve only averages.** For depth and stencil, RI offers the resolve modes
  the GPU supports (min, max, sample 0) and emulates none.
- **The stencil reference value is part of the pipeline.** A different reference value
  means a different pipeline. This follows P38: no state you can change while recording.

**Decided (2026-10-15, P104):** `cmdSetStencilReference` overrides the pipeline's value for
the rest of a section.

### 5. Indirect drawing where the GPU also writes root data ("level 2")

- On Vulkan this needs scratch memory. **You provide it**, as you chose on 2026-10-11:
  `indirectScratchRequirements` says how much.
- `allocateMemory` can take a resource's requirements, so RI picks a memory type that
  resource accepts. The AMD Linux driver needs this for the scratch memory.
- **Off on NVIDIA when GPU-side validation is on:** the driver crashed with both. You kept
  it off and will investigate by hand.

### 6. Swapchain

- RI creates no window. You pass the window's OS handles (Win32, X11 or Wayland).
- On Vulkan, resizing or destroying a swapchain waits until the GPU is idle.
- On D3D12, sRGB is handled the way DXGI requires: the buffers are plain (UNORM) and the
  views are sRGB.
- A GLFW helper makes the window for you if you want.

### 7. Raw Vulkan or D3D12 in the middle of RI

- `beginNative` / `endNative` mark a block of your own Vulkan or D3D12 commands. After it,
  RI sets its own state back.
- If your code moved a texture to another layout, you tell RI in the list you pass to
  `endNative`.
- Objects created outside RI can be imported. RI never destroys them, only forgets them.

### 8. Ray tracing

- Inline ray queries only (`RayQuery` in any shader), no ray tracing pipelines (P49).
- **Off on AMD Windows with Vulkan:** that driver rejects the descriptor setup ray tracing
  needs. D3D12 on the same GPU works. You accepted this for now.

### 9. Work graphs (D3D12 only)

- You give the size of the root data, because RI cannot read it from a library.
- You give the graph's working memory with `bindWorkGraphMemory`, as you chose. RI sets
  that memory up right away and waits for it, because commands recorded earlier may run
  later.

*Your call:* whether that wait at bind time is acceptable. It happens once per bind, not per
frame.

### 10. Helpers

- Memory allocator, slot allocator, upload and readback in one call, frame ring, profiler,
  and a shader compiler you can call at run time.
- A frame graph helper was written and then **removed** at your review: frame graphs belong
  to the layer above RI.

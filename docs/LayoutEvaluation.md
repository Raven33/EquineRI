# L1: Hiding Image Layouts — Evaluation

> Status: **direction A'**, confirmed only after measurement (P18; round 5). This document shows what each remaining
> candidate looks like to a user and to a backend, and what has to be measured before
> choosing. API names in the sketches are placeholders, not proposals.

## Constraints

- P16: users never see or reason about image layouts.
- Bindless: the base cannot see which textures a shader reads or writes.
- P15: no mandatory per-resource state tracking in the base.
- P10: both coarse and fine-grained synchronization exist, and coarse is the default.
- Vulkan global memory barriers (`VkMemoryBarrier2`) cannot change an image layout. A
  layout change always names an image.
- Desktop and laptop only (P19), on Turing / RDNA2 and newer.

## Running example

Render a G-buffer target `albedo` (with `depth`), run a compute lighting pass that
samples `albedo` and writes storage texture `lit`, then draw a fullscreen pass that
samples `lit` into the swapchain. The next frame repeats.

## Candidate A': one resting layout, base transitions only around its own operations

User code:

```cpp
cmd.beginRendering({.colors = {albedo}, .depth = depth});
cmd.drawMeshTasks(groups);
cmd.endRendering();
cmd.barrier(Sync::eColorOutput, Sync::eCompute);     // global: names no resource
cmd.dispatch(lighting, groups, rootData);            // samples albedo, writes lit (heap indices)
cmd.barrier(Sync::eCompute, Sync::eFragment);
cmd.beginRendering({.colors = {swapchainTexture}});
cmd.draw(3);
cmd.endRendering();
```

What the backend emits:

| Backend | Emitted |
|---------|---------|
| Vulkan + `VK_KHR_unified_image_layouts` | Every image rests in `GENERAL`. User barriers are `VkMemoryBarrier2` only. The only layout transitions are base-owned and name one image each: (a) `UNDEFINED → GENERAL` once at creation/first use; (b) `UNDEFINED → GENERAL` on discard/aliasing (P35); (c) swapchain acquire → `GENERAL` (from `UNDEFINED` when contents are discarded, else from `PRESENT_SRC`); (d) `GENERAL → PRESENT_SRC` at present. |
| Vulkan without it | Either `GENERAL` everywhere (legal, but attachment compression may suffer) or around-operation transitions as on D3D12. This is a per-driver fallback choice. |
| D3D12 (enhanced barriers) | `beginRendering`: albedo resting → `RENDER_TARGET`, depth resting → `DEPTH_STENCIL_WRITE`. `endRendering`: back to resting. The user barriers become global `D3D12_GLOBAL_BARRIER`s. The same base-owned initial and discard transitions as above apply (into `DIRECT_QUEUE_COMMON`), as they do on Vulkan without the extension. |

Properties:

- User burden: none. Barriers say "what must finish before what", never which texture.
- Hidden cost: on D3D12 (and on Vulkan drivers without unified layouts), an attachment
  round-trips through the resting layout at every pass boundary. That can trigger
  decompression or metadata work the user cannot see or avoid.
- Information lost: the base never learns that a previous texture's contents can be
  discarded, unless `beginRendering` load ops carry it (they can: `eDontCare` / `eClear`).
- Native escape hatch (E1, settled by P27): `beginNative` gives each texture's native
  resting layout; at `endNative` the user lists the textures left in another layout, and
  Equine transitions them back. Every texture is at rest after `endNative`.
- Multithreaded recording: no cross-command-buffer state. Every command buffer starts and
  ends with all textures at rest.
- ~~Unverified on D3D12: whether a single resting layout permits both sampled reads and
  UAV writes.~~ Resolved: `DIRECT_QUEUE_COMMON` (see Research results).

## Candidate B: per-texture barriers that name accesses, never layouts

> **Rejected (round 4, P26): too verbose.** Kept for the record. Hybrid H depends on B
> and is rejected with it.

User code:

```cpp
cmd.barrier({{albedo, Access::eNone, Access::eColorAttachmentWrite, Discard::eYes},
             {depth,  Access::eNone, Access::eDepthWrite,           Discard::eYes}});
cmd.beginRendering({.colors = {albedo}, .depth = depth});
cmd.drawMeshTasks(groups);
cmd.endRendering();
cmd.barrier({{albedo, Access::eColorAttachmentWrite, Access::eComputeSampled},
             {lit,    Access::eFragmentSampled,      Access::eComputeStorageWrite, Discard::eYes}});
cmd.dispatch(lighting, groups, rootData);
cmd.barrier({{lit,              Access::eComputeStorageWrite, Access::eFragmentSampled},
             {swapchainTexture, Access::ePresent,             Access::eColorAttachmentWrite, Discard::eYes}});
cmd.beginRendering({.colors = {swapchainTexture}});
cmd.draw(3);
cmd.endRendering();
```

The backend derives everything from the access pair:

| Access pair | Vulkan (sync2) | D3D12 (enhanced barriers) |
|-------------|----------------|---------------------------|
| `ColorAttachmentWrite → ComputeSampled` | `COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL`, plus stage/access masks | `RENDER_TARGET → SHADER_RESOURCE`, plus sync/access |
| `ComputeStorageWrite → FragmentSampled` | `GENERAL → SHADER_READ_ONLY_OPTIMAL` | `UNORDERED_ACCESS → SHADER_RESOURCE` |
| `X → Y` with unified layouts | `GENERAL → GENERAL`; only sync and access remain | unchanged |

Properties:

- User burden: the user must state each texture's *previous* access, including across
  command buffers and frames ("lit was last sampled by the fragment stage last frame").
  A wrong `before` is a validation error under P21, and undefined behavior without it.
- Hidden cost: none. Layouts are optimal, and the discard information allows skipping
  decompression and initial-content preservation.
- It maps one-to-one onto Vulkan sync2 and D3D12 enhanced barriers, with the layout
  vocabulary replaced by access vocabulary.
- P10 still holds. Coarse global barriers stay the default for buffers and for textures
  whose role does not change (for example storage → storage). A per-texture barrier is
  needed only when a texture changes role.
- Native escape hatch (E1): native code must report, or agree on, the accesses it leaves
  textures in.
- Multithreaded recording: the base keeps no state. Knowing previous accesses across
  threads is the user's (or a helper's) problem.
- A tracking helper can reconstruct `before` automatically for users who want that,
  layered on top (P15).

## Hybrid H (for discussion)

B everywhere, plus an opt-in per-texture creation flag that keeps a texture in a resting
layout (A' semantics) so it never needs per-texture barriers. The performance cost
becomes a visible, per-texture user choice instead of a global base decision. The cost
is two models to document and validate.

## What must be measured (P18)

1. **Driver support survey.** `VK_KHR_unified_image_layouts` on Turing and RDNA2:
   Windows NVIDIA, Windows AMD, Linux NVIDIA, Linux RADV. D3D12 enhanced-barrier
   support (`D3D12_FEATURE_D3D12_OPTIONS12::EnhancedBarriersSupported`) on the same GPUs.
2. **Render → sample.** Formats RGBA8, RGBA16F, R11G11B10F, D32F, D24S8 (D3D12 and NVIDIA Vulkan only; AMD Vulkan has no D24S8, use D32F_S8 there), plus 4x MSAA
   color, at 1080p and 4K. Compare (i) B optimal layouts, (ii) `GENERAL` everywhere
   without the extension, (iii) A' with unified layouts, (iv) the D3D12 resting
   round-trip. Measure GPU timestamps around the barrier and the consuming pass. Confirm
   in RGP / Nsight whether a decompression actually ran.
3. **Storage write → sample** for compute-written textures in each mode, explicitly including a texture resting in `GENERAL` on RDNA2 Vulkan without the extension.
4. **Depth write → sample** (HiZ / depth pyramid), the case most sensitive to metadata.
5. **D3D12 resting-layout validity.** Determine which enhanced-barrier layout, if any,
   permits SRV + UAV access on a compressed texture without per-use transitions.

Decision rule: agreed together before measuring, for example "A' if its worst case on the
hardware floor costs at most X% per affected pass." B and H are rejected (P26), so the
alternatives left if it fails are: choosing the other A' variant per driver (`GENERAL`
everywhere vs. around-pass transitions), accepting the cost, an opt-in per-texture
simultaneous-access mode (C), or reopening L1 with the user.

## Research results (2026-10-03)

### D3D12 resting layout: resolved

The enhanced-barriers spec defines `D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON`. It permits
shader-resource, unordered-access, and copy access on the direct queue. The spec notes
that "on some hardware, costly texture decompression can be avoided if accesses on both
sides of a layout transition barrier happen on the same queue type", which is why the
queue-specific variant is preferable to plain `COMMON`. Plain `COMMON` allows only SRV
and copy, not UAV.

- Resting layout on the direct queue: `DIRECT_QUEUE_COMMON`. A compute-queue
  counterpart exists; multi-queue comes later (P37), and its resting-layout/sharing
  implications are a backend-internal question for that time.
- Render targets and depth still need `RENDER_TARGET` / `DEPTH_STENCIL_*` around passes.
  No layout combines render-target writes with shader reads.
- Simultaneous-access textures have an immutable layout and never transition, at the
  cost of compression. They remain a possible opt-in.

So measurement item 5 is answered. A' does not leak a usage decision on D3D12.

### `VK_KHR_unified_image_layouts` driver coverage

| Driver | Status | Floor hardware |
|--------|--------|----------------|
| NVIDIA Windows / Linux | Added in 573.38 / 570.123.18 (Vulkan beta driver, 2025-06-08) | Turing is supported by these drivers |
| Mesa RADV (Linux AMD) | Added in Mesa 25.2 (2025-08), **RDNA3+ only** | **RDNA2 not covered** |
| AMD Windows (proprietary) | Unknown: not found in release-note searches | Check with `vulkaninfo` on the RDNA2 machine |

Consequence: on the hardware floor, RDNA2 (at least on Linux, possibly on Windows) does
not get unified layouts. There, Vulkan must either use `GENERAL` anyway (legal, with an
unknown cost) or do the same around-pass transitions as D3D12. In the latter case the resting layout
is chosen per texture from usage bits that texture creation already requires (as D3D12
needs `ALLOW_UNORDERED_ACCESS`): storage-capable textures rest in `GENERAL`, sampled-only
textures in `SHADER_READ_ONLY_OPTIMAL` (which cannot take storage writes). This adds no
user decision and keeps P16. Measurement items 2 and 3 must include RDNA2 on Vulkan
without the extension.

Sources: the D3D12 enhanced-barriers spec
(https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html), the NVIDIA
Vulkan driver page (https://developer.nvidia.com/vulkan-driver), the Mesa 25.2.0 release
notes (https://docs.mesa3d.org/relnotes/25.2.0.html), and the Khronos blog
(https://www.khronos.org/blog/so-long-image-layouts-simplifying-vulkan-synchronisation).

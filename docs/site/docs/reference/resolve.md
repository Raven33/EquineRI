---
symbols: [RI::cmdResolve, RI::ResolveMode, RI::resolveModeBit]
---

# RI::cmdResolve, resolveTo

## What it does

A **resolve** turns a multisampled texture into a single-sampled one, one value per pixel
([P52](../design-notes/decisions.md#p52), [P76](../design-notes/decisions.md#p76)). Two forms:

- **at the end of a rendering section**: set `resolveTo` (a single-sampled view) and
  `resolveMode` on a `ColorAttachment` or the `DepthAttachment` of
  [`cmdBeginRendering`](cmdBeginRendering.md); `cmdEndRendering` resolves;
- **as a command**: `RI::cmdResolve(cmd, src, dst, mode)`, outside rendering sections.

`ResolveMode`: `eAverage`, `eMin`, `eMax`, `eSampleZero`.

- **Color** resolves average (`eAverage`), for non-integer formats.
- **Depth and stencil** use the modes the GPU reports: `device.limits.depthResolveModes` and
  `stencilResolveModes` hold `RI::resolveModeBit(mode)` for each supported mode. A format with
  stencil resolves both aspects with the same mode, which must be in both masks. `eMin` and
  `eMax` are the portable depth modes on desktop GPUs; D3D12 has no `eSampleZero`. RI does not
  emulate a missing mode: resolve in your own shader instead.

## Signature

--8<-- "generated/signatures/RI.cmdResolve.md"

--8<-- "generated/signatures/RI.ResolveMode.md"

--8<-- "generated/signatures/RI.resolveModeBit.md"

## How RI does it

- **Vulkan**: resolve attachments of dynamic rendering (`resolveImageView`, `resolveMode`);
  `cmdResolve` is an empty rendering section that loads the source and resolves it, since
  `vkCmdResolveImage` only averages color. Modes come from
  `VkPhysicalDeviceDepthStencilResolveProperties`.
- **D3D12**: `ResolveSubresourceRegion` per layer and plane, after the section (or for
  `cmdResolve`), with RI's switches to `RESOLVE_SOURCE` / `RESOLVE_DEST` and back
  ([P68](../design-notes/decisions.md#p68)). Depth and stencil resolves need programmable
  sample positions tier 2 (depth: average, min, max; stencil: min, max).

## Rules and errors

- The source is multisampled, the destination single-sampled, with the same size, layers and
  format; the destination has `eRenderTarget` (color) or `eDepthStencil` (depth) usage.
- A mode the GPU does not support, or a color mode other than `eAverage`, is a **programmer
  error** in validation builds.
- For [barriers](cmdBarrier.md), resolves count as `Stage::eRenderTarget` (color) and
  `Stage::eDepthStencil` (depth) writes; their results are visible after the switch back RI
  records, like attachment writes.
- A destination over memory another resource used must be [discarded](cmdDiscard.md) first.

## Related decisions

- [P52](../design-notes/decisions.md#p52): both resolve forms.
- [P76](../design-notes/decisions.md#p76): resolve modes, the capability query, no emulation.

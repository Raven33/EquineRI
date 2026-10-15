---
symbols: [RI::cmdBeginRendering, RI::cmdEndRendering, RI::RenderingDesc, RI::ColorAttachment, RI::DepthAttachment, RI::LoadOp, RI::StoreOp, RI::ClearValue]
---

# RI::cmdBeginRendering, RI::cmdEndRendering

## What it does

`RI::cmdBeginRendering(cmd, desc)` opens a **rendering section**: the draws recorded until
`RI::cmdEndRendering(cmd)` write to the attachments listed in `desc`
([P40](../design-notes/decisions.md#p40)). See [Rendering sections](../concepts/rendering.md)
for the ideas; this page lists the details.

`RenderingDesc` lists:

- `colors`: up to 8 `ColorAttachment`s, in the order the pixel shader's `SV_Target0`,
  `SV_Target1`, ... write them;
- `depth`: an optional `DepthAttachment`.

Each attachment has a `view` (of a texture with `eRenderTarget` or `eDepthStencil` usage), a
load operation (`eLoad`, `eClear`, `eDontCare`; default `eLoad`), a store operation (`eStore`,
`eDontCare`; default `eStore`) and a `clear` value used with `eClear`. A depth attachment also
has `stencilLoad` and `stencilStore`, and the read-only flags `depthReadOnly` and
`stencilReadOnly` ([P75](../design-notes/decisions.md#p75)). Any attachment may name a
single-sampled `resolveTo` view and a `resolveMode`: `cmdEndRendering` then
[resolves](resolve.md) the attachment into it ([P52](../design-notes/decisions.md#p52)); shaders may sample the depth
texture during a read-only section only if it has `TextureUsage::eReadOnlyDepthSampling`
([P97](../design-notes/decisions.md#p97)).

At `cmdBeginRendering`, RI:

1. switches each attachment into its attachment layout, when the backend needs it (from
   "undefined" when the load operation does not keep the contents);
2. clears the attachments whose load operation is `eClear`;
3. sets the viewport and scissor to cover the smallest attachment, with clip-space y pointing
   up on both backends ([`cmdSetViewport`, `cmdSetScissor`](cmdSetViewport.md) change them
   until the section ends).

At `cmdEndRendering`, RI switches the attachments back to rest
([P68](../design-notes/decisions.md#p68)).

## Signature

--8<-- "generated/signatures/RI.cmdBeginRendering.md"

--8<-- "generated/signatures/RI.cmdEndRendering.md"

--8<-- "generated/signatures/RI.RenderingDesc.md"

--8<-- "generated/signatures/RI.ColorAttachment.md"

--8<-- "generated/signatures/RI.DepthAttachment.md"

--8<-- "generated/signatures/RI.LoadOp.md"

--8<-- "generated/signatures/RI.StoreOp.md"

--8<-- "generated/signatures/RI.ClearValue.md"

## Example

```cpp
--8<-- "snippets/ri/textures.cpp:rendering"
```

A read-only depth section, sampling the depth texture while testing against it:

```cpp
--8<-- "snippets/ri/textures.cpp:read-only-depth"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/textures.cpp:rendering"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:rendering"
    ```

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:read-only-depth"
    ```

    What to watch for: each `VkRenderingAttachmentInfo` names the layout the image is in
    during the section, so the transition comes first. A depth-stencil view is given twice,
    as depth and as stencil attachment. The negative viewport height makes clip space y-up
    like D3D12 (Vulkan 1.1 core). With `VK_KHR_unified_image_layouts`, `GENERAL` everywhere
    replaces these transitions.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:rendering"
    ```

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:read-only-depth"
    ```

    What to watch for: render targets are bound by CPU descriptor (`OMSetRenderTargets`), and
    clearing is a command (`ClearRenderTargetView`), not a load operation. There is no "end":
    the next barrier moves the textures on. A read-only section needs a read-only DSV and the
    `DEPTH_STENCIL_READ` layout.

## Rules and errors

- At least one attachment; at most 8 colors (**programmer error**).
- Color attachments need `eRenderTarget` usage, the depth attachment `eDepthStencil`. On D3D12
  RI reports a **programmer error** otherwise; on Vulkan the validation layer does.
- Attachment views should cover one mip; a view with several layers renders to all of them
  (layered rendering) on Vulkan.
- No section may be open already, and `cmdEndRendering` needs an open one (**programmer
  error** in validation builds, in every build on D3D12).
- An attachment over memory another resource used must be [discarded](cmdDiscard.md) first
  (validation builds report it at `cmdBeginRendering`).
- Read-only depth: set `stencilReadOnly` too for formats with stencil, keep `load` at `eLoad`,
  and use pipelines that do not write depth (validation builds report one at
  `cmdSetPipeline`).
- Each pipeline used in the section must have the attachments' formats and sample count
  ([`createGraphicsPipeline`](graphicsPipeline.md)).
- `StoreOp::eDontCare` is a hint: on D3D12 it has no effect today.
- MSAA resolve at the end of a section comes later ([P52](../design-notes/decisions.md#p52)).

## Related decisions

- [P40](../design-notes/decisions.md#p40): rendering sections, per-attachment load and store.
- [P75](../design-notes/decisions.md#p75): read-only depth.
- [P68](../design-notes/decisions.md#p68): RI's switches around the section.
- [P16](../design-notes/decisions.md#p16): no layouts in the API.
- [P52](../design-notes/decisions.md#p52): MSAA resolve (later).

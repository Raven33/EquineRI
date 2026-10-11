# Rendering sections

## What a rendering section is

Draws do not happen anywhere: they write to **attachments**, up to 8 color textures and one
depth (and stencil) texture, bound for a stretch of the command list. That stretch is a
**rendering section**, opened by [`cmdBeginRendering`](../reference/cmdBeginRendering.md) and
closed by `cmdEndRendering` ([P40](../design-notes/decisions.md#p40)). There are no render
pass or framebuffer objects: a section is described where it is recorded.

```cpp
--8<-- "snippets/ri/textures.cpp:rendering"
```

Each attachment is a [texture view](textures.md#views) of a texture created with
`eRenderTarget` (color) or `eDepthStencil` (depth).

## Load and store operations

Each attachment says what happens to its contents at the start and at the end of the section.

| At the start (`LoadOp`) | Meaning |
|---|---|
| `eLoad` (default) | keep what the texture holds |
| `eClear` | set every texel to the attachment's `clear` value |
| `eDontCare` | the contents are not needed: the draws will cover every texel |

| At the end (`StoreOp`) | Meaning |
|---|---|
| `eStore` (default) | keep what the section wrote |
| `eDontCare` | the results are not needed after the section (a depth buffer used only for this pass) |

Say `eClear` or `eDontCare` whenever you can: the GPU then skips reading the old contents. A
depth-stencil attachment has separate operations for its stencil: `stencilLoad`,
`stencilStore`.

**Clear values.** `ClearValue::color` (four floats) for color attachments; for integer formats
give whole numbers (2.0f clears to 2). `depth` (default 1.0) and `stencil` (default 0) for the
depth attachment.

Load operations do not replace a [discard](textures.md#discard-and-aliasing): a texture that
takes over another resource's memory is discarded first, even if its section clears it.

## Viewport and scissor

RI sets the viewport and the scissor to cover the attachments: from (0, 0) to the size of the
**smallest** attachment, depth from 0 to 1. A view of mip 1 of a 1024×1024 texture is 512×512,
and so is the section. Smaller viewports and scissors come later.

## Clip space points up on both backends

The same vertex shader produces the same image on both backends. In clip space, x goes right,
**y goes up**, and depth goes from 0 (near) to 1 (far), as in D3D12; the top-left texel of a
texture is at clip-space (-1, 1).

Vulkan's clip space has y pointing down. RI flips it with a **negative viewport height** (a
standard Vulkan 1.1 feature): the viewport starts at the bottom of the target and has height
`-height`. The winding of triangles is flipped with it, so `frontCounterClockwise` means the
same thing on both backends.

## Read-only depth

Some effects test against the depth buffer while sampling it in the pixel shader: soft
particles, decals. Normally a texture cannot be an attachment and a shader input at once. Set
`depthReadOnly` (and `stencilReadOnly`, for a format with stencil) on the depth attachment,
and the section uses a read-only depth layout that allows both
([P75](../design-notes/decisions.md#p75)). A texture sampled this way is created with
`TextureUsage::eReadOnlyDepthSampling` ([P97](../design-notes/decisions.md#p97)):

```cpp
--8<-- "snippets/ri/textures.cpp:read-only-depth"
```

In such a section:

- pipelines must not write depth (`DepthState::write` off). In validation builds, setting a
  depth-writing pipeline is a **programmer error**: `cmdSetPipeline: the pipeline writes depth,
  but the rendering section's depth is read-only (P75)`;
- keep the load operation at `eLoad` (the default): a read-only attachment is not cleared;
- nothing is stored for the read-only aspects, whatever the store operation says;
- shaders sample the depth texture only if it has `eReadOnlyDepthSampling`. Without the flag,
  a read-only section is still allowed (depth testing only), but shaders must not sample the
  texture inside it: on Vulkan without unified layouts, a sampled depth texture rests in the
  layout fastest for ordinary sampling, which is not the read-only depth layout, and RI
  switches it around the section. RI cannot see bindless reads, so this is not checked.

## Around the section: RI's layout switches

`cmdBeginRendering` switches each attachment into its attachment layout when the backend needs
it, and `cmdEndRendering` switches it back to rest
([P68](../design-notes/decisions.md#p68)). Nothing to write on your side; see
[Image layouts are RI's business](layouts.md). What you do write is the barrier between the
section and what reads its results:

```cpp
--8<-- "snippets/ri/textures.cpp:barrier-render-sample"
```

## Inside a section

Allowed: `cmdSetPipeline` (a graphics pipeline whose attachment formats and sample count match
the section), `cmdSetRootData`, `cmdDraw`, `cmdWriteTimestamp`. Not allowed: clears, discards,
query resets and resolves, and another `cmdBeginRendering`; in validation builds these are
reported as programmer errors, for example
`cmdClearTexture: not allowed inside a rendering section (cmdEndRendering first)`. Barriers
belong outside sections too: end the section, barrier, begin the next one.

## In native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/textures.cpp:rendering"
    ```

=== "Vulkan"

    Dynamic rendering (`vkCmdBeginRendering`, Vulkan 1.3), with the transitions written by hand:

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:rendering"
    ```

=== "D3D12"

    D3D12 has no section object: render targets are bound, cleared with commands, and drawn
    to; the transitions are texture barriers:

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:rendering"
    ```

## Related decisions

- [P40](../design-notes/decisions.md#p40): rendering sections with per-attachment load and
  store operations.
- [P75](../design-notes/decisions.md#p75): read-only depth.
- [P68](../design-notes/decisions.md#p68): RI's switches around a section.
- [P38](../design-notes/decisions.md#p38): pipelines know their attachment formats.
- [P52](../design-notes/decisions.md#p52): MSAA resolve (later).

---
symbols: [RI::createView, RI::destroyView, RI::TextureView, RI::TextureViewDesc, RI::Aspect, RI::kAllMips, RI::kAllLayers]
---

# RI::createView, RI::destroyView

## What it does

A **view** names part of a texture, in one format: a mip range, a layer range and an aspect
([P73](../design-notes/decisions.md#p73)). Everything that uses a texture takes a view:
[rendering attachments](cmdBeginRendering.md), [descriptors](writeDescriptors.md),
[clears](cmdClear.md), [discards](cmdDiscard.md).

Every texture already has `texture.view`, the whole texture in its own format. For anything
else, `RI::createView(device, texture, desc)` creates a view once, and
`RI::destroyView(device, view)` destroys it. RI keeps no hidden cache of views and creates none
behind your back.

The description, all optional:

- `format`: `eUndefined` (default) means the texture's format; any other must be the texture's
  format or one of its `viewFormats` ([P74](../design-notes/decisions.md#p74));
- `baseMip`, `mipCount`: the mips, `kAllMips` (default) meaning "to the last mip";
- `baseLayer`, `layerCount`: the array layers, `kAllLayers` (default) meaning "to the last".
  For a [3D texture](texture.md#shapes) they are depth slices of the view's first mip, which
  only render targets use; a view of some slices covers one mip. A cube texture's view
  covering whole cubes reads as a cube ([shapes](texture.md#shapes));
- `aspect`: what of a depth-stencil texture the view covers.

| `Aspect` | Covers |
|---|---|
| `eDefault` | color for color formats; depth **and** stencil for depth-stencil formats |
| `eColor` | color (color formats only) |
| `eDepth` | depth only |
| `eStencil` | stencil only (formats with stencil) |

The returned `RI::TextureView` tells its `format`, `width` and `height` (of its first mip: a
view of mip 2 of a 1024×1024 texture is 256×256) and `samples`.

## Signature

--8<-- "generated/signatures/RI.createView.md"

--8<-- "generated/signatures/RI.destroyView.md"

--8<-- "generated/signatures/RI.TextureView.md"

--8<-- "generated/signatures/RI.TextureViewDesc.md"

--8<-- "generated/signatures/RI.Aspect.md"

--8<-- "generated/signatures/RI.kAllMips.md"

--8<-- "generated/signatures/RI.kAllLayers.md"

## Example

```cpp
--8<-- "snippets/ri/textures.cpp:create-views"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/textures.cpp:create-views"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:create-view-function"
    ```

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:create-views"
    ```

    What to watch for: Vulkan images have no view of their own. A view in another format
    needs an image created with `MUTABLE_FORMAT` (and preferably a format list); a view whose
    format cannot be stored to (sRGB) must drop the storage usage with
    `VkImageViewUsageCreateInfo`, which RI does for you.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:create-views"
    ```

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:write-descriptors"
    ```

    What to watch for: D3D12 has no view objects, only descriptors: render-target and
    depth-stencil views in CPU-only heaps, shader views in the shader-visible heap. An RI view
    holds the RTV and DSV descriptors its texture's usage allows (two DSVs: writable and
    read-only); shader descriptors are written by `writeDescriptors`.

## Rules and errors

- A format that is neither the texture's nor in its `viewFormats` is a **programmer error**,
  in every build: `createView: format ... is not the texture's format or one of its
  viewFormats (P74)`.
- In validation builds, a view format must have the same size per texel as the texture's
  format, and the mip and layer ranges must be inside the texture (**programmer error**).
- An aspect the format does not have (`eDepth` of a color texture) is a **programmer error**.
- Shaders cannot read a view with both depth and stencil: create one with `Aspect::eDepth` or
  `Aspect::eStencil` (`writeDescriptors` reports it on Vulkan).
- An attachment view covers one mip. Use views with `mipCount = 1` as attachments.
- Destroy the views you created before their texture, and only when the GPU no longer uses
  them. Do not destroy `texture.view`: `destroyTexture` does.

## Related decisions

- [P73](../design-notes/decisions.md#p73): explicit views, a whole-texture view per texture.
- [P74](../design-notes/decisions.md#p74): format reinterpretation through listed view formats.
- [P87](../design-notes/decisions.md#p87): descriptors take texture views through `.texture`.

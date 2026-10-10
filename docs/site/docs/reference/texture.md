---
symbols: [RI::createTexture, RI::destroyTexture, RI::Texture, RI::TextureDesc, RI::TextureUsage, RI::Format, RI::isDepthFormat, RI::hasStencil, RI::bytesPerTexel]
---

# RI::createTexture, RI::destroyTexture

## What it does

`RI::createTexture(device, memory, offset, desc)` creates a 2D texture (or 2D array) described
by `desc`, **placed** in `memory` at `offset` ([P31](../design-notes/decisions.md#p31)). Like a
buffer, it allocates nothing: the bytes belong to the memory block. Get the size, alignment and
memory kinds first with [`textureRequirements`](textureRequirements.md).
`RI::destroyTexture(device, texture)` destroys it; the memory stays.

The description:

- `width`, `height`, `layers` (array layers), `mips` (mip levels), `samples` (1, 2, 4 or 8 for
  MSAA);
- `format`: one of the [formats](../concepts/textures.md#formats);
- `usage`: everything the texture will be used for (below);
- `viewFormats`: other formats views may use, such as the sRGB version of the format. Empty by
  default ([P74](../design-notes/decisions.md#p74)).

Creating a texture also **resets** it: its contents are undefined but valid, ready for any use
([P69](../design-notes/decisions.md#p69)). RI records that reset itself, submits it on the
graphics queue and waits for it, so `createTexture` costs a short submission: create textures at
load time, not in the middle of a frame.

The returned `RI::Texture` holds its facts (size, mips, layers, samples, format, usage) and
`view`, a ready-made [view](view.md) of the **whole texture** in its own format, used for
attachments, descriptors, clears and discards.

| `TextureUsage` | Lets you | Vulkan usage | D3D12 flag |
|---|---|---|---|
| `eSampled` | read it in shaders (`ReadView`, `Texture2D<T>`) | `SAMPLED` | not `DENY_SHADER_RESOURCE` |
| `eStorage` | read and write it in shaders (`ReadWriteView`, `RWTexture2D<T>`) | `STORAGE` | `ALLOW_UNORDERED_ACCESS` |
| `eRenderTarget` | render to it ([color attachment](cmdBeginRendering.md)) | `COLOR_ATTACHMENT` | `ALLOW_RENDER_TARGET` |
| `eDepthStencil` | use it as the depth attachment | `DEPTH_STENCIL_ATTACHMENT` | `ALLOW_DEPTH_STENCIL` |
| `eCopySrc` / `eCopyDst` | copy from / to it (later) | `TRANSFER_SRC` / `TRANSFER_DST` | - |

The flags also decide the layout the texture rests in between operations, inside the backend
([Image layouts are RI's business](../concepts/layouts.md)). Set every use you need and no
more: unused flags can cost compression. On Vulkan, `eStorage`, `eRenderTarget` and
`eDepthStencil` also add `TRANSFER_DST`, for [clears](cmdClear.md).

`RI::isDepthFormat(format)` and `RI::hasStencil(format)` tell depth formats apart;
`RI::bytesPerTexel(format)` gives the size of one texel of a color format (0 for depth formats).

## Signature

--8<-- "generated/signatures/RI.createTexture.md"

--8<-- "generated/signatures/RI.destroyTexture.md"

--8<-- "generated/signatures/RI.Texture.md"

--8<-- "generated/signatures/RI.TextureDesc.md"

--8<-- "generated/signatures/RI.TextureUsage.md"

--8<-- "generated/signatures/RI.Format.md"

--8<-- "generated/signatures/RI.isDepthFormat.md"

--8<-- "generated/signatures/RI.hasStencil.md"

--8<-- "generated/signatures/RI.bytesPerTexel.md"

## Example

```cpp
--8<-- "snippets/ri/textures.cpp:texture-desc"
```

```cpp
--8<-- "snippets/ri/textures.cpp:create-textures"
```

Two textures sharing memory (they need a [discard](cmdDiscard.md) before each first use):

```cpp
--8<-- "snippets/ri/textures.cpp:create-aliased"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/textures.cpp:create-textures"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:create-image-function"
    ```

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:image-info"
    ```

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:create-textures"
    ```

    What to watch for: an image is created unbound, then bound to memory once. It starts in
    the `UNDEFINED` layout, and its first use must transition it from there. Views in other
    formats need `MUTABLE_FORMAT`; the `VkImageFormatListCreateInfo` list keeps compression
    possible.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:texture-desc-function"
    ```

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:create-textures"
    ```

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:initial-layout"
    ```

    What to watch for: `CreatePlacedResource2` takes the initial layout and the **castable
    formats** (the other formats views may use). A placed render target or depth texture must
    be initialized (a `DISCARD` barrier, a clear or a copy) before its first use. A depth
    texture that shaders read is created `TYPELESS`, so its depth view and its shader view can
    use different formats.

## Rules and errors

- `offset` must be a multiple of the alignment from `textureRequirements`, and the texture must
  fit: checked in validation builds (**programmer error**).
- Memory of a kind the texture does not fit is an **unsupported** error (textures need `eGPU`).
- A texture placed over memory another resource used must be [discarded](cmdDiscard.md) before
  its first use; validation builds report it otherwise.
- Destroy a texture only when the GPU no longer uses it (after its
  [timeline point](../concepts/timeline.md)), after the [views](view.md) you created of it, and
  before freeing its memory. `destroyTexture` also destroys `texture.view`.
- `Texture` is a thin value: copies refer to the same texture
  ([P81](../design-notes/decisions.md#p81)). Using a destroyed texture is a **programmer error**
  in validation builds.
- 3D and cube textures come later.

## Related decisions

- [P31](../design-notes/decisions.md#p31): placed resources.
- [P58](../design-notes/decisions.md#p58): explicit usage flags, which also choose the resting
  layout.
- [P73](../design-notes/decisions.md#p73): every texture has a whole-texture view.
- [P74](../design-notes/decisions.md#p74): view formats.
- [P69](../design-notes/decisions.md#p69): the initial reset and discard.
- [P16](../design-notes/decisions.md#p16): no layouts in the API.

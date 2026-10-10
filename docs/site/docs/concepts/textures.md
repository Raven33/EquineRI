# Textures and views

## What a texture is

A **texture** is GPU memory arranged as a 2D grid of texels (or several grids: mips and array
layers), in a **format** such as `eRGBA8Unorm` or `eD32Float`. Unlike a buffer, its bytes are
in an arrangement only the driver knows, often **compressed** in ways invisible to shaders.
Shaders read it with filtering and in any format-compatible way; the GPU renders into it.

In RI a texture is created like a buffer: you describe it, ask how much memory it needs, and
place it in a [memory block](memory.md) you allocated
([P31](../design-notes/decisions.md#p31)):

```cpp
--8<-- "snippets/ri/textures.cpp:texture-desc"
```

```cpp
--8<-- "snippets/ri/textures.cpp:requirements"
```

```cpp
--8<-- "snippets/ri/textures.cpp:create-textures"
```

Textures live in `eGPU` memory: [`textureRequirements`](../reference/textureRequirements.md)
says which memory kinds can hold a texture, and on today's GPUs that is `eGPU` only.

## Usage flags

The `usage` flags say everything the texture will be used for
([P58](../design-notes/decisions.md#p58)):

| `TextureUsage` | Lets you |
|---|---|
| `eSampled` | read it in shaders through a `ReadView` (`Texture2D<T>`) |
| `eStorage` | read and write it in shaders through a `ReadWriteView` (`RWTexture2D<T>`) |
| `eRenderTarget` | render to it as a color attachment |
| `eDepthStencil` | use it as the depth (and stencil) attachment |
| `eCopySrc` / `eCopyDst` | copy from / to it (texture copies come later) |

Set every use you need and no more. Each flag can cost: it can turn off compression, change
the memory needed, and it decides the layout the texture rests in between operations (see
[Image layouts are RI's business](layouts.md)). Whether a format supports a set of uses is a
question you can ask first: [`formatSupported`](../reference/formatSupported.md).

## View formats and compression

A texture's texels can be read in another format of the same size, for example
`eRGBA8Unorm` as `eRGBA8Srgb` (so shaders decode sRGB), or `eR32Uint` as `eR32Float`. List
those formats when you create the texture, in `viewFormats`
([P74](../design-notes/decisions.md#p74)).

Leave the list empty when you do not need it, which is the default. Drivers compress textures
whose format can never change, and an exact list lets them keep compression when it can. A
view in a format you did not list is a **programmer error**, in every build.

## Views

A **view** names part of a texture, in one format: a range of mips, a range of layers, an
aspect ([P73](../design-notes/decisions.md#p73)). Everything that uses a texture takes a view:
rendering attachments, descriptors, clears, discards.

Every texture comes with one ready-made: `texture.view`, the **whole texture** in its own
format. Create others when you need them, once, with
[`createView`](../reference/view.md), and destroy them before the texture:

```cpp
--8<-- "snippets/ri/textures.cpp:create-views"
```

A view reports what you need to know about it: `format`, `width` and `height` (of its first
mip) and `samples`.

## Aspects: color, depth, stencil

A depth-stencil texture holds two things: depth and stencil, its **aspects**. A view of such a
texture covers both by default (`Aspect::eDefault`), which is what a depth attachment needs.
Shaders read one aspect at a time, so a view that a shader reads names one: `Aspect::eDepth`
or `Aspect::eStencil`. A depth-only format (`eD16Unorm`, `eD32Float`) has only depth, and its
default view can be read directly.

## In shaders

Texture views go in the same descriptor heap as buffer views, through `.texture`
([`writeDescriptors`](../reference/writeDescriptors.md)):

```cpp
--8<-- "snippets/ri/textures.cpp:write-descriptors"
```

The shader indexes the heap with the slot, as for buffers:

```hlsl
Texture2D<float4> albedo = ResourceDescriptorHeap[root.albedoSlot];
RWTexture2D<float4> target = ResourceDescriptorHeap[root.targetSlot];
```

Shaders read and write textures as they are between operations: no transition, no state to
declare.

## Discard and aliasing

Two textures (or a texture and a buffer) can be placed over the **same bytes** of memory if they
are never used at the same time: a blur's two scratch textures, the G-buffer of one pass and
the bloom chain of the next. This is **aliasing**, and it saves memory.

A texture that takes over memory another resource used starts with garbage: its texels, and
also its hidden compression metadata, which the GPU trusts. A load operation (`eClear`,
`eDontCare`) does not reset that metadata. So before the **first use** of such a texture,
record [`cmdDiscard`](../reference/cmdDiscard.md) ([P69](../design-notes/decisions.md#p69)):

```cpp
--8<-- "snippets/ri/textures.cpp:discard"
```

The rule is simple: if any other resource was ever placed over any byte of the texture's memory,
discard the texture before using it, in the command list where it takes the memory over. A
discard drops the contents: never discard a texture whose texels you still need. Buffers need no
discard.

A texture placed in fresh memory needs none: [`createTexture`](../reference/texture.md) resets
it once itself.

**Validation check.** In validation builds RI remembers every placement in each memory block.
When a texture placed over bytes another resource occupied (alive or destroyed) is used as an
attachment or cleared before being discarded, RI reports a **programmer error**:
`cmdBeginRendering: the texture was placed over memory that held another resource; record
cmdDiscard before its first use (P69; load operations do not count)`. Shader uses are invisible under bindless and are not checked. Release builds
track nothing.

## Formats

| Kind | Formats |
|---|---|
| 8-bit color | `eR8Unorm`, `eRG8Unorm`, `eRGBA8Unorm`, `eRGBA8Srgb`, `eBGRA8Unorm`, `eBGRA8Srgb` |
| packed color | `eRGB10A2Unorm`, `eR11G11B10Float` |
| float color | `eR16Float`, `eRG16Float`, `eRGBA16Float`, `eR32Float`, `eRG32Float`, `eRGBA32Float` |
| integer color | `eR32Uint`, `eRG32Uint`, `eRGBA32Uint` |
| depth | `eD16Unorm`, `eD32Float` |
| depth and stencil | `eD24UnormS8Uint` (not on AMD Vulkan), `eD32FloatS8Uint` |

`RI::isDepthFormat`, `RI::hasStencil` and `RI::bytesPerTexel` answer the usual questions about a
format.

## Related decisions

- [P31](../design-notes/decisions.md#p31): placed resources.
- [P58](../design-notes/decisions.md#p58): usage flags, the requirements and format queries.
- [P73](../design-notes/decisions.md#p73): views and the whole-texture view.
- [P74](../design-notes/decisions.md#p74): view formats.
- [P69](../design-notes/decisions.md#p69): discard.
- [P16](../design-notes/decisions.md#p16): no layouts.

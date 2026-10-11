---
symbols: [RI::writeDescriptors, RI::DescriptorWrite, RI::Descriptor, RI::ReadView, RI::ReadWriteView, RI::ConstantBufferView, RI::kWholeSize]
---

# RI::writeDescriptors

## What it does

`RI::writeDescriptors(device, heap, writes)` writes a list of descriptors into heap slots. Each
`RI::DescriptorWrite` names a **slot** and a **descriptor**: one of three views
([P87](../design-notes/decisions.md#p87)). Read and read-write views name either a **texture
view** (`.texture`) or a **buffer range** (`.buffer`); constant buffer views name a buffer.

| View | Shader type | Usage needed |
|---|---|---|
| `RI::ReadView{.texture = view}` | `Texture2D<T>`, `Texture2DArray<T>`, `Texture2DMS<T>` | `TextureUsage::eSampled` |
| `RI::ReadWriteView{.texture = view}` | `RWTexture2D<T>`, `RWTexture2DArray<T>` | `TextureUsage::eStorage` |
| `RI::ReadView{.buffer = buffer}` | `StructuredBuffer<T>`, `ByteAddressBuffer` | `BufferUsage::eRead` |
| `RI::ReadWriteView{.buffer = buffer}` | `RWStructuredBuffer<T>`, `RWByteAddressBuffer` | `BufferUsage::eReadWrite` |
| `RI::ConstantBufferView` | `ConstantBuffer<T>` | `BufferUsage::eConstant` |

A texture view comes from the texture (`texture.view`, the whole texture) or from
[`createView`](view.md) (a mip, a layer range, another format, one aspect). Set either
`.texture` or `.buffer`, not both: when `.texture` holds a view, the buffer fields are ignored.
Shaders access textures as they are between operations; there is no layout to give
([P16](../design-notes/decisions.md#p16)).

Each buffer view covers part of a buffer: `offset` (bytes) and `size` (bytes; `RI::kWholeSize`, the
default, means "to the end"). Read and read-write views also have a `stride`: the element size
of a structured buffer, or **0 for a raw** (byte address) buffer.

The writes **take effect at once**: there is no "commit" step. You pass a braced list, or a
span (for example a `std::vector`) built at run time; one write is a list of one
([P94](../design-notes/decisions.md#p94), D1).

## Signature

--8<-- "generated/signatures/RI.writeDescriptors.md"

--8<-- "generated/signatures/RI.DescriptorWrite.md"

--8<-- "generated/signatures/RI.Descriptor.md"

--8<-- "generated/signatures/RI.ReadView.md"

--8<-- "generated/signatures/RI.ReadWriteView.md"

--8<-- "generated/signatures/RI.ConstantBufferView.md"

--8<-- "generated/signatures/RI.kWholeSize.md"

## Example

```cpp
--8<-- "snippets/ri/compute.cpp:write-descriptors"
```

A list built at run time, with a view of part of a buffer:

```cpp
--8<-- "snippets/ri/compute.cpp:write-descriptors-span"
```

Texture views:

```cpp
--8<-- "snippets/ri/textures.cpp:write-descriptors"
```

In the shader, the slot comes from root data:

```hlsl
--8<-- "snippets/hlsl/doc_compute.hlsl:heap-access"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:write-descriptors"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:write-descriptors"
    ```

    What to watch for: in SPIR-V, read-only and read-write structured buffers are both
    *storage buffers*; constant buffers are *uniform buffers*. Into a mutable descriptor,
    each write states the concrete type. RI writes the whole list with **one**
    `vkUpdateDescriptorSets` call.

    Texture descriptors are *sampled images* and *storage images*, and they name the layout
    the image is in when the shader reads it:

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:write-descriptors"
    ```

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:write-descriptors"
    ```

    What to watch for: buffer views count **elements**, not bytes: `FirstElement` and
    `NumElements` in units of `StructureByteStride`, or of 4 bytes for raw views
    (`R32_TYPELESS` with the `RAW` flag). Constant buffer views are addressed by GPU address
    and sized in 256-byte blocks. RI makes one call per write.

    Texture descriptors are shader-resource and unordered-access views of the texture, in the
    view's format (for a depth texture, the shader-readable format of the same bits):

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:write-descriptors"
    ```

## Rules and errors

All checked in validation builds (**programmer error**):

- the **slot** is inside the heap: `slot 8 is outside the resource heap (it has 8 slots)`;
- **read and read-write views** start at a multiple of 16 bytes and of the element size
  (`stride`, or 4 for raw views), are not empty, and cover whole elements;
- **constant buffer views** start at a multiple of 256 bytes. Their size is **rounded up to
  256** on both backends, and the rounded size must fit in the buffer: give constant buffers a
  size that is a multiple of 256;
- the buffer or texture is alive (not destroyed).

Texture views:

- shaders cannot read a view with both depth and stencil: create one with `Aspect::eDepth` or
  `Aspect::eStencil` (**programmer error** on Vulkan);
- a `ReadWriteView` needs a format the GPU can store to: sRGB formats cannot be
  ([`formatSupported`](formatSupported.md) with `eStorage`);
- a view covering several layers is read as `Texture2DArray`; an MSAA view as `Texture2DMS`.

Also:

- Never overwrite a slot that GPU work still in flight may read; wait for its
  [timeline point](../concepts/timeline.md) ([P59](../design-notes/decisions.md#p59)).
- Samplers go to the sampler heap with [`writeSamplers`](writeSamplers.md).

## Related decisions

- [P87](../design-notes/decisions.md#p87): descriptor kinds named by access; `.texture` and
  `.buffer`.
- [P73](../design-notes/decisions.md#p73): texture views.
- [P42](../design-notes/decisions.md#p42): you choose the slots.
- [P59](../design-notes/decisions.md#p59): writes take effect at once; never overwrite a slot
  in use.
- [P94](../design-notes/decisions.md#p94): the batching form (D1) and the view alignment
  rule.

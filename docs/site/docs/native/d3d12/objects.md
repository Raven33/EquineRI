---
symbols: [RI::D3D12::native, RI::D3D12::NativeDescriptorHeap, RI::D3D12::NativePipeline, RI::D3D12::NativeTexture]
---

# D3D12: native objects behind RI objects

## What it does

Every RI object has a `RI::D3D12::native(...)` overload in `<RI/D3D12/Native.hpp>` that returns
the D3D12 object(s) behind it ([P84](../../design-notes/decisions.md#p84)). Use them for what
RI does not wrap: names for PIX, feature queries, a native command.

| RI object | `RI::D3D12::native(...)` returns |
|---|---|
| `Device` | `NativeDevice`: factory, adapter, device, DIRECT queue, feature level (see [device creation](device-creation.md)) |
| `Memory` | `ID3D12Heap *` |
| `Buffer` | `ID3D12Resource *` |
| `DescriptorHeap` | `NativeDescriptorHeap`: the shader-visible `CBV_SRV_UAV` and `SAMPLER` heaps |
| `Pipeline` | `NativePipeline`: the `ID3D12PipelineState` and the root signature RI built |
| `CommandList` | `ID3D12GraphicsCommandList7 *` |
| `Texture` | `NativeTexture`: the `ID3D12Resource *` and the layout it rests in |
| `QueryPool` | `ID3D12QueryHeap *` (a `TIMESTAMP` query heap) |

A `TextureView` has no native object on D3D12: D3D12 has no view objects, only descriptors.
RI keeps the view's render-target and depth-stencil descriptors in its own CPU-only heaps.

The pointers are **borrowed**: RI keeps its references and releases them when you destroy
the RI object. Call `AddRef` to keep one longer.

## Signature

--8<-- "generated/signatures/RI.D3D12.native.md"

--8<-- "generated/signatures/RI.D3D12.NativeDescriptorHeap.md"

--8<-- "generated/signatures/RI.D3D12.NativePipeline.md"

--8<-- "generated/signatures/RI.D3D12.NativeTexture.md"

## Example: name a buffer

```cpp
--8<-- "snippets/d3d12/native-device.cpp:object-handles"
```

## Example: a native command in an RI command list

```cpp
--8<-- "snippets/d3d12/native-device.cpp:native-command"
```

## Example: a texture and its resting layout

`NativeTexture::restingLayout` is the layout the texture is in between RI's operations
([Image layouts are RI's business](../../concepts/layouts.md)): `DIRECT_QUEUE_COMMON` for
textures that shaders or copies use, `RENDER_TARGET` or `DEPTH_STENCIL_WRITE` for attachment-only
textures. Native code that moves a texture to another layout returns it there.

```cpp
--8<-- "snippets/d3d12/native-device.cpp:native-texture"
```

## Example: a query heap

```cpp
--8<-- "snippets/d3d12/native-device.cpp:native-query-pool"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/d3d12/native-device.cpp:object-handles"
    ```

=== "Vulkan"

    The Vulkan equivalents: [Vulkan: native handles](../vulkan/objects.md).

=== "D3D12"

    In a plain D3D12 program you already hold these objects; see
    [`createBuffer`](../../reference/buffer.md) and the other reference pages for how each is
    created.

## Rules

- Do not `Release` a pointer you did not `AddRef`.
- A native command recorded between RI commands must leave the state RI relies on as it was:
  the descriptor heaps, the root signature, the pipeline state and the root constants.
  Commands that set nothing (copies, `WriteBufferImmediate`, queries, markers) are safe. Native
  sections that save and restore RI's state are planned
  ([P27](../../design-notes/decisions.md#p27), [P70](../../design-notes/decisions.md#p70)).
- **A texture must be in its resting layout whenever RI uses it**: return every texture you
  moved, with an enhanced texture barrier, before the next RI command that may touch it and
  before the command list ends ([P27](../../design-notes/decisions.md#p27)).
- RI's buffers are placed resources in the `UNDEFINED` layout (enhanced barriers); synchronize
  native work with RI's [`cmdBarrier`](../../reference/cmdBarrier.md) or your own global
  barriers.
- RI's root signatures put root data at parameter 0 (`b0, space999`) and set the
  heap-indexing flags.

## Related decisions

- [P84](../../design-notes/decisions.md#p84): native access is gated.
- [P22](../../design-notes/decisions.md#p22): native resources are always reachable.
- [P27](../../design-notes/decisions.md#p27), [P70](../../design-notes/decisions.md#p70):
  [native sections](../sections.md).
- [P67](../../design-notes/decisions.md#p67): RI builds the root signature from the DXIL.

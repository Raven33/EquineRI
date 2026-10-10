---
symbols: [RI::D3D12::native, RI::D3D12::NativeDescriptorHeap, RI::D3D12::NativePipeline]
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

The pointers are **borrowed**: RI keeps its references and releases them when you destroy
the RI object. Call `AddRef` to keep one longer.

## Signature

--8<-- "generated/signatures/RI.D3D12.native.md"

--8<-- "generated/signatures/RI.D3D12.NativeDescriptorHeap.md"

--8<-- "generated/signatures/RI.D3D12.NativePipeline.md"

## Example: name a buffer

```cpp
--8<-- "snippets/d3d12/native-device.cpp:object-handles"
```

## Example: a native command in an RI command list

```cpp
--8<-- "snippets/d3d12/native-device.cpp:native-command"
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
- RI's buffers are placed resources in the `UNDEFINED` layout (enhanced barriers); synchronize
  native work with RI's [`cmdBarrier`](../../reference/cmdBarrier.md) or your own global
  barriers.
- RI's root signatures put root data at parameter 0 (`b0, space999`) and set the
  heap-indexing flags.

## Related decisions

- [P84](../../design-notes/decisions.md#p84): native access is gated.
- [P22](../../design-notes/decisions.md#p22): native resources are always reachable.
- [P27](../../design-notes/decisions.md#p27), [P70](../../design-notes/decisions.md#p70):
  native sections (later).
- [P67](../../design-notes/decisions.md#p67): RI builds the root signature from the DXIL.

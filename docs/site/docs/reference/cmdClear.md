---
symbols: [RI::cmdClearTexture, RI::cmdClearBuffer]
---

# RI::cmdClearTexture, RI::cmdClearBuffer

## What it does

Clears outside rendering sections ([P65](../design-notes/decisions.md#p65)):

- `RI::cmdClearTexture(cmd, view, value)` sets every texel of `view` to `value`: `value.color`
  for color textures (whole numbers for integer formats), `value.depth` and `value.stencil` for
  depth textures;
- `RI::cmdClearBuffer(cmd, buffer, value)` sets every 32-bit word of the whole buffer to
  `value` (a bit pattern: `0x3f800000` is `1.0f`).

To clear an attachment at the start of a rendering section, use its load operation
(`LoadOp::eClear`) instead: it is free or nearly so.

For barriers, **clears count as copies**: follow a clear with
`cmdBarrier(cmd, Stage::eCopy, ...)` before reading what it wrote, and precede it with
`cmdBarrier(cmd, ..., Stage::eCopy)` if earlier work used the resource.

## Signature

--8<-- "generated/signatures/RI.cmdClearTexture.md"

--8<-- "generated/signatures/RI.cmdClearBuffer.md"

## Example

```cpp
--8<-- "snippets/ri/textures.cpp:clears"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/textures.cpp:clears"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:clears"
    ```

    What to watch for: `vkCmdClearColorImage` and `vkCmdClearDepthStencilImage` are transfer
    commands: the image needs `TRANSFER_DST` usage and the `TRANSFER_DST_OPTIMAL` or `GENERAL`
    layout. `vkCmdFillBuffer` needs `TRANSFER_DST` too, and a size that is a multiple of 4.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:clears"
    ```

    What to watch for: render targets and depth textures are cleared with
    `ClearRenderTargetView` / `ClearDepthStencilView`, in their attachment layout. Other
    textures and buffers need an unordered-access view in **both** a shader-visible heap and a
    CPU-only heap (`ClearUnorderedAccessViewFloat` / `Uint`), and the resource must allow
    unordered access.

## How RI does it

- **Vulkan**: the native clear commands, in place for textures resting in `GENERAL`, otherwise
  inside a switch to `TRANSFER_DST_OPTIMAL` and back ([P68](../design-notes/decisions.md#p68));
  `vkCmdFillBuffer` for buffers.
- **D3D12**: render targets and depth textures are cleared inside a switch to their attachment
  layout. Storage textures and buffers use an unordered-access clear with descriptors in **RI's
  own small heap**: RI sets that heap for the clear, then sets your heaps (and root signatures)
  again, so your next commands see your heap and no slot of yours is used.

## Rules and errors

- `cmdClearTexture`: the texture needs `eRenderTarget`, `eDepthStencil` or `eStorage` usage. On
  D3D12, another texture is a **programmer error**; on Vulkan, the validation layer reports it.
- `cmdClearBuffer`: the buffer needs `BufferUsage::eCopyDst` (**programmer error**). On D3D12 it
  must also be in `eGPU` or `eGPUUpload` memory (`eUpload` and `eReadback` heaps forbid the
  unordered access the clear uses): **programmer error** otherwise. The whole buffer is
  cleared.
- Not inside a rendering section (**programmer error**).
- A texture over memory another resource used must be [discarded](cmdDiscard.md) before it is
  cleared (validation builds report it).
- On D3D12, a storage-texture or buffer clear switches descriptor heaps twice, which may cost
  a short stall on some GPUs.

## Related decisions

- [P65](../design-notes/decisions.md#p65): clears are RI commands; D3D12 uses RI's own heap and
  restores the user's.
- [P68](../design-notes/decisions.md#p68): RI's layout switches around its own operations.
- [P40](../design-notes/decisions.md#p40): clears at the start of a section are load
  operations.

---
symbols: [RI::copyLayout, RI::CopyLayout, RI::cmdCopyBufferToTexture, RI::cmdCopyTextureToBuffer, RI::cmdCopyTexture, RI::copyBytesPerTexel]
---

# RI::copyLayout, RI::cmdCopyBufferToTexture, RI::cmdCopyTextureToBuffer, RI::cmdCopyTexture

## What it does

Copies between buffers and textures ([P47](../design-notes/decisions.md#p47)):

- `RI::copyLayout(device, view)` says how the texels of a texture view are laid out in a
  buffer: `rowPitch` bytes from one row to the next, `layerPitch` from one layer to the next,
  `size` bytes in all, and the `alignment` the buffer offset needs. The pitches differ per
  backend and GPU; you never compute them yourself.
- `RI::cmdCopyBufferToTexture(cmd, src, srcOffset, dst)` copies texels from a buffer into a
  texture view (an upload, from `eUpload` memory).
- `RI::cmdCopyTextureToBuffer(cmd, src, dst, dstOffset)` copies texels from a texture view into
  a buffer (a readback, into `eReadback` memory).
- `RI::cmdCopyTexture(cmd, src, dst)` copies one texture view into another of the same size.

A copy covers the **whole view**: one mip, every layer of the view. Make a
[view](view.md) of the mip (and layers) to copy. For depth formats, copy one aspect at a
time (a view with `Aspect::eDepth` or `Aspect::eStencil`); `RI::copyBytesPerTexel(format,
aspect)` gives its size (4 bytes for depth, also for 24-bit depth; 1 for stencil).

Buffer-to-buffer copies are [`cmdCopyBuffer`](cmdCopyBuffer.md).

## Signature

--8<-- "generated/signatures/RI.copyLayout.md"

--8<-- "generated/signatures/RI.CopyLayout.md"

--8<-- "generated/signatures/RI.cmdCopyBufferToTexture.md"

--8<-- "generated/signatures/RI.cmdCopyTextureToBuffer.md"

--8<-- "generated/signatures/RI.cmdCopyTexture.md"

--8<-- "generated/signatures/RI.copyBytesPerTexel.md"

## Example

```cpp
--8<-- "snippets/ri/copies.cpp:upload"
```

```cpp
--8<-- "snippets/ri/copies.cpp:readback"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/copies.cpp:upload"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/copies.cpp:upload"
    ```

    What to watch for: the image must be in `TRANSFER_DST_OPTIMAL` (or `GENERAL`) during the
    copy, so a sampled texture needs a transition there and back. `bufferRowLength` is in
    texels, not bytes.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/copies.cpp:upload"
    ```

    What to watch for: rows are 256-byte aligned and each subresource's data starts on a
    512-byte boundary; `GetCopyableFootprints` gives the exact layout. One `CopyTextureRegion`
    per subresource (per layer and per plane).

## How RI does it

- **Vulkan**: `vkCmdCopyBufferToImage2`, `vkCmdCopyImageToBuffer2`, `vkCmdCopyImage2` with one
  region. The row pitch follows `optimalBufferCopyRowPitchAlignment`, the offset alignment
  `optimalBufferCopyOffsetAlignment` (at least the texel size and 4). A texture that rests in
  `GENERAL` is copied in place; others (mode T) switch to the transfer layout and back, with
  RI's exact/full halves ([P68](../design-notes/decisions.md#p68)).
- **D3D12**: `CopyTextureRegion` per layer, rows on 256 bytes, layers on 512 bytes. Textures
  with copy usage rest in `DIRECT_QUEUE_COMMON`, which allows copies: no switch.

## Rules and errors

- The texture needs `TextureUsage::eCopyDst` to be written by a copy and `eCopySrc` to be read
  (**programmer error** in validation builds). The buffer needs `BufferUsage::eCopySrc` /
  `eCopyDst` (**programmer error**).
- The view covers one mip and one aspect; buffer copies need a single-sampled texture.
- The buffer offset is a multiple of `layout.alignment`, and `offset + layout.size` fits in the
  buffer.
- `cmdCopyTexture`: same width, height, layer count, sample count and texel size.
- Not inside a rendering section. Copies count as `Stage::eCopy` for
  [barriers](cmdBarrier.md): a barrier from the work that wrote the source, and one to the
  work that reads the destination.
- A texture over memory another resource used must be [discarded](cmdDiscard.md) before a
  copy writes it (validation builds report it).

## Related decisions

- [P47](../design-notes/decisions.md#p47): copy commands and the copy-layout query.
- [P68](../design-notes/decisions.md#p68): RI's layout switches around its own operations.

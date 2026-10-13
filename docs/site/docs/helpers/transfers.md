# Transfers

## What it does

`<RI/Helpers/Transfers.hpp>` uploads and reads back in one call: `uploadBuffer`,
`readbackBuffer`, `uploadTexture`, `readbackTexture`. Each call creates its own staging
memory and command list, submits on the graphics queue, **waits**, and frees them.

Texture data is the view's texels (one mip, all its layers), rows **tightly packed**:
`width * bytesPerTexel` bytes per row, layers one after the other. The helper turns that into
the backend's row pitch ([copyLayout](../reference/copies.md)).

RI reads no image files (KTX, PNG, DDS): decode them with a library of your choice (libktx,
stb_image) into uncompressed texels, then upload them, one mip at a time through a view of that
mip (`createView` with `baseMip` and `mipCount = 1`).

Because it waits, it suits loading and tools, not per-frame streaming
([P47](../design-notes/decisions.md#p47)).

## Example

```cpp
--8<-- "snippets/ri/helpers.cpp:transfers"
```

## How to roll your own

Upload memory and a buffer of `copyLayout(device, view).size` bytes; write each row at
`rowPitch`; `cmdCopyBufferToTexture` (or `cmdCopyBuffer`); submit. For a readback, copy into
`eReadback` memory and read it after the point is reached. To stream, keep a ring of staging
memory and free each part at its frame's point instead of waiting.

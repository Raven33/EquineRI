---
symbols: [RI::textureRequirements, RI::fits]
---

# RI::textureRequirements, RI::fits

## What it does

Before you place a texture in a memory block, you need to know **how many bytes** it takes,
**at what alignment** its offset must be, and **which memory kinds** can hold it. All three
depend on the GPU and on the whole description (size, mips, samples, format, usage, view
formats), so RI asks the driver: `RI::textureRequirements(device, desc)` returns them, without
creating anything ([P58](../design-notes/decisions.md#p58)).

The answer is a [`MemoryRequirements`](bufferRequirements.md):

- `size` and `alignment`: place the texture at an offset that is a multiple of `alignment`, with
  `size` bytes free from there;
- `kinds`: a bit per [memory kind](../concepts/memory.md) that can hold it. Test one with
  `RI::fits(requirements, kind)`.

On the GPUs RI supports, textures fit `eGPU` memory only. Plan on that: allocate `eGPU` blocks
for textures, and upload texels through a buffer in `eUpload` memory ([copies](copies.md), or the
[transfer helpers](../helpers/transfers.md)).

## Signature

--8<-- "generated/signatures/RI.textureRequirements.md"

--8<-- "generated/signatures/RI.fits.md"

--8<-- "generated/signatures/RI.MemoryRequirements.md"

## Example

Three textures side by side in one block:

```cpp
--8<-- "snippets/ri/textures.cpp:requirements"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/textures.cpp:requirements"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:requirements"
    ```

    What to watch for: `memoryTypeBits` lists the memory **types** that accept the image;
    RI turns it into memory kinds. When buffers and optimal-tiling images share one block,
    Vulkan also asks that they be `bufferImageGranularity` apart (a device limit).

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:requirements"
    ```

    What to watch for: textures are aligned to 64 KiB, MSAA textures to 4 MiB. Asking for
    several descriptions at once gives each one's offset in a shared block. Textures live in
    `DEFAULT` heaps only.

## Rules and errors

- `createTexture` in memory of a kind not in `kinds` is an **unsupported** error:
  `createTexture: this eRGBA8Unorm texture cannot be placed in this memory kind on '<GPU>'
  (check textureRequirements(...).kinds)` (Vulkan), `createTexture: textures need eGPU memory
  on D3D12` (D3D12).
- An offset that is not a multiple of `alignment`, or a texture that does not fit, is a
  **programmer error** in validation builds: `createTexture: placing the texture in memory:
  offset ... is not a multiple of the required alignment ...`.
- Prefer separate blocks for buffers and textures. RI does not yet account for Vulkan's
  `bufferImageGranularity` when a buffer and a texture are neighbours in one block.
- MSAA textures need large alignments (4 MiB on D3D12); RI's `eGPU` memory blocks on D3D12 are
  aligned for them.

## Related decisions

- [P58](../design-notes/decisions.md#p58): (1) a memory-requirements query per resource
  description, with compatible memory kinds.
- [P31](../design-notes/decisions.md#p31): placed resources.
- [P32](../design-notes/decisions.md#p32): memory kinds.

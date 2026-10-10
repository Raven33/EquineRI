---
symbols: [RI::formatSupported]
---

# RI::formatSupported

## What it does

Not every GPU supports every format for every use. `RI::formatSupported(device, format, usage,
samples)` answers whether a texture of `format` can be created with **every** usage in `usage`
and with `samples` samples per texel (1 by default) on this device
([P58](../design-notes/decisions.md#p58)). It creates nothing.

Ask before relying on a format that varies between GPUs, and choose a fallback:

- `eD24UnormS8Uint` is missing on AMD GPUs with Vulkan: fall back to `eD32FloatS8Uint`;
- storage use (`eStorage`) of 8-bit and packed formats, and MSAA sample counts above 4, vary
  between GPUs.

Formats that every GPU at RI's hardware floor supports for a use (for example `eRGBA8Unorm`
render targets, `eD32Float` depth) need no question.

## Signature

--8<-- "generated/signatures/RI.formatSupported.md"

## Example

```cpp
--8<-- "snippets/ri/textures.cpp:format-supported"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/textures.cpp:format-supported"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:format-supported"
    ```

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:check-format"
    ```

    What to watch for: the format features (`optimalTilingFeatures`) answer per use; the
    sample counts and size limits are a separate question about one exact image
    (`vkGetPhysicalDeviceImageFormatProperties2`), which RI also asks.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:check-format"
    ```

    What to watch for: `Support1` and `Support2` are bit sets; sampled depth textures are
    checked through the shader-readable format of the same bits (for example `R32_FLOAT` for
    `D32_FLOAT`).

## Rules and errors

- Usage mapping: `eSampled` needs sampled reads, `eStorage` typed storage writes,
  `eRenderTarget` color attachment, `eDepthStencil` depth attachment. On Vulkan, `eStorage`,
  `eRenderTarget` and `eDepthStencil` also need transfer writes (RI's clears use them).
- `eUndefined` is never supported.
- Creating a texture with an unsupported combination is not checked by RI: the native call or
  the validation layer fails. Ask first.

## Related decisions

- [P58](../design-notes/decisions.md#p58): (2) a format/usage support query, with unsupported
  combinations failing clearly.
- [P11](../design-notes/decisions.md#p11): asymmetric features go through a capability query.

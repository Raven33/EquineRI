---
symbols: [RI::writeSamplers, RI::SamplerWrite, RI::SamplerDesc, RI::Filter, RI::AddressMode, RI::BorderColor, RI::Reduction]
---

# RI::writeSamplers

## What it does

`RI::writeSamplers(device, heap, writes)` writes **sampler descriptions** into the heap's
**sampler** slots ([P87](../design-notes/decisions.md#p87)). Each `RI::SamplerWrite` names a slot
and a `RI::SamplerDesc`. There is no sampler object to create or destroy: you write the
description where shaders read it.

A `DescriptorHeap` holds two heaps with separate slot numbers: `writeDescriptors` fills the
resource heap (`ResourceDescriptorHeap[i]` in HLSL), `writeSamplers` the sampler heap
(`SamplerDescriptorHeap[i]`). Create the heap with `samplerCount` slots.

`SamplerDesc`:

| Field | Meaning | Default |
|---|---|---|
| `minFilter`, `magFilter`, `mipFilter` | `eNearest` or `eLinear` | `eLinear` |
| `addressU`, `addressV`, `addressW` | `eRepeat`, `eMirror`, `eClamp` (to the edge), `eBorder` | `eRepeat` |
| `mipLodBias`, `minLod`, `maxLod` | level-of-detail bias and clamp | `0`, `0`, `1000` (all mips) |
| `maxAnisotropy` | `1` (off) to `16`; anisotropic filtering is linear, mips included | `1` |
| `compare` | a comparison sampler (`SamplerComparisonState`, shadow maps) | none |
| `border` | `eTransparentBlack`, `eOpaqueBlack`, `eOpaqueWhite`, for `eBorder` | `eTransparentBlack` |
| `reduction` | `eAverage`, or `eMin` / `eMax` of the filtered texels (`device.limits.samplerMinMax`) | `eAverage` |

## Signature

--8<-- "generated/signatures/RI.writeSamplers.md"

--8<-- "generated/signatures/RI.SamplerDesc.md"

## Example

```cpp
RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 1024, .samplerCount = 16});
RI::writeSamplers(device, heap, {
    {.slot = 0, .sampler = {}},                                            // linear, repeat
    {.slot = 1, .sampler = {.maxAnisotropy = 16}},                         // anisotropic
    {.slot = 2, .sampler = {.addressU = RI::AddressMode::eClamp,
                            .addressV = RI::AddressMode::eClamp,
                            .compare = RI::CompareOp::eLess}},              // shadow map
});
```

```hlsl
Texture2D<float4> albedo = ResourceDescriptorHeap[root.albedoSlot];
SamplerState linearRepeat = SamplerDescriptorHeap[0];
float4 color = albedo.Sample(linearRepeat, uv);
```

## How RI does it

- **Vulkan**: a `VkSampler` per distinct description, made on first use and kept in a small
  per-device cache until the device is destroyed (writing the same description again reuses
  it); the descriptor goes into the heap's sampler set (set 1). Min/max reduction is
  `VkSamplerReductionModeCreateInfo` (`samplerFilterMinmax`).
- **D3D12**: `CreateSampler` straight into the shader-visible sampler heap; the filter is
  encoded with the reduction type (comparison, minimum, maximum). Min/max needs tiled
  resources tier 2.

## Rules and errors

- Slots are within the heap's `samplerCount` (**programmer error** in validation builds).
- `maxAnisotropy` from 1 to 16, `minLod` at most `maxLod`, no `compare` together with a min or
  max reduction, and `eMin` / `eMax` only with `device.limits.samplerMinMax` (**programmer
  error** in validation builds).
- Vulkan drivers cap the number of distinct samplers (`maxSamplerAllocationCount`, often
  4000): reaching it is **unsupported**.
- Never overwrite a slot that GPU work in flight may read ([P59](../design-notes/decisions.md#p59)).

## Related decisions

- [P87](../design-notes/decisions.md#p87): descriptor kinds; samplers.
- [P94](../design-notes/decisions.md#p94): the sampler heap is set 1 on Vulkan.

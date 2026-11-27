---
symbols: [RI::Vk::importMemory, RI::Vk::importBuffer, RI::Vk::importTexture, RI::Vk::importPipeline, RI::Vk::writeNativeDescriptor, RI::D3D12::importMemory, RI::D3D12::importBuffer, RI::D3D12::importTexture, RI::D3D12::importPipeline, RI::D3D12::writeNativeDescriptor, RI::Metal::importMemory, RI::Metal::importBuffer, RI::Metal::importTexture, RI::Metal::importPipeline, RI::Metal::NativeRenderState, RI::Metal::writeNativeDescriptor, RI::Metal::NativeDescriptor]
---

# Importing native objects

## What it does

Natively created objects can be used as RI objects ([P77](../design-notes/decisions.md#p77)):
`importMemory`, `importBuffer`, `importTexture`, `importPipeline`, in each backend's native
namespace. You describe the object (size, format, usage), since Vulkan cannot report how an
image was created.

**RI does not own imported objects.** Destroying the RI object only forgets it (RI's own
views or descriptors for an imported texture are freed); you destroy the native object
yourself, afterwards. On D3D12 and Metal RI holds no reference: keep the object alive while RI
uses it.

`writeNativeDescriptor` puts a native descriptor into a heap slot, for view types RI does not
model: a `VkDescriptorImageInfo` / `VkDescriptorBufferInfo` with its descriptor type on
Vulkan, a CPU descriptor handle (copied from a non-shader-visible heap) on D3D12, a raw
24-byte entry (`NativeDescriptor`, Metal Shader Converter's `IRDescriptorTableEntry`) on Metal.

## Rules

- **Imported textures**: create them as RI would for `desc.usage` (2D, the matching usage or
  resource flags), and bring them into `native(texture).restingLayout` yourself, in a
  [native section](sections.md), before RI first uses them.
- **Imported buffers** need the usages `desc.usage` implies (Vulkan: and
  `SHADER_DEVICE_ADDRESS`); give their CPU address if they are mapped.
- **Imported memory**: Vulkan memory needs `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`; give the
  CPU address of mapped memory. RI places resources in it as in any `Memory`.
- **Imported pipelines** need a layout compatible with RI's: Vulkan set 0 the resource heap,
  set 1 the sampler heap, one push-constant range for all stages of `rootDataSize`; D3D12
  root parameter 0 the root constants (`b0, space999`), heaps directly indexed.

## Metal

Metal has no image layouts, and its pipeline objects hold less than Vulkan's or D3D12's
([P119](../design-notes/decisions.md#p119)):

- **Residency**: importing adds the object to RI's residency set, destroying the RI object
  removes it (at the next submission). What a `writeNativeDescriptor` entry points at must be
  resident too: in RI's memory, imported, or in your own set.
- **`importMemory(device, heap, kind)`**: a placement heap (`MTLHeapTypePlacement`) with the
  kind's storage mode, private for `eGPU` and shared for the others. No size or CPU address:
  the heap has its size, and RI maps CPU kinds itself.
- **`importBuffer(device, buffer, desc)`**: at least `desc.size` long; mapped unless private.
- **`importTexture(device, texture, desc)`**: any shape, matching `desc` (type, pixel format,
  size, mips, layers, samples), private storage, and the `MTLTextureUsage` RI gives
  `desc.usage` (`PixelFormatView` when `desc.viewFormats` is set). A mismatch is a
  **programmer error** in every build.
- **`importPipeline`** follows RI's binding model: shaders converted with RI's root signature
  ([P114](../design-notes/decisions.md#p114)). A compute pipeline takes its threadgroup size,
  since Metal takes it at each dispatch. A graphics pipeline takes a `NativeRenderState`: the
  render encoder state (depth-stencil state, primitive type, cull mode, winding, fill mode,
  depth clip and bias, stencil reference, mesh threadgroup sizes) that Vulkan and D3D12 keep in
  the pipeline. Geometry and tessellation pipelines cannot be imported.

```cpp
RI::Pipeline imported = RI::Metal::importPipeline(
    device, renderPipelineState, sizeof(DrawRoot),
    {.depthStencil = depthStencilState, .cullMode = MTL::CullModeBack});
```

## Related decisions

- [P77](../design-notes/decisions.md#p77): import and raw descriptor writes.
- [P84](../design-notes/decisions.md#p84): native access is gated by the backend's header.
- [P119](../design-notes/decisions.md#p119): import on Metal.

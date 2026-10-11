---
symbols: [RI::Vk::importMemory, RI::Vk::importBuffer, RI::Vk::importTexture, RI::Vk::importPipeline, RI::Vk::writeNativeDescriptor, RI::D3D12::importMemory, RI::D3D12::importBuffer, RI::D3D12::importTexture, RI::D3D12::importPipeline, RI::D3D12::writeNativeDescriptor]
---

# Importing native objects

## What it does

Natively created objects can be used as RI objects ([P77](../design-notes/decisions.md#p77)):
`importMemory`, `importBuffer`, `importTexture`, `importPipeline`, in each backend's native
namespace. You describe the object (size, format, usage), since Vulkan cannot report how an
image was created.

**RI does not own imported objects.** Destroying the RI object only forgets it (RI's own
views or descriptors for an imported texture are freed); you destroy the native object
yourself, afterwards. On D3D12 RI holds no reference: keep the object alive while RI uses it.

`writeNativeDescriptor` puts a native descriptor into a heap slot, for view types RI does not
model: a `VkDescriptorImageInfo` / `VkDescriptorBufferInfo` with its descriptor type on
Vulkan, a CPU descriptor handle (copied from a non-shader-visible heap) on D3D12.

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

## Related decisions

- [P77](../design-notes/decisions.md#p77): import and raw descriptor writes.
- [P84](../design-notes/decisions.md#p84): native access is gated by the backend's header.

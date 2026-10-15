---
symbols: [RI::DeviceLimits]
---

# RI::DeviceLimits

## What it is

`device.limits` tells what this device allows, for the few things RI's own functions depend
on. It is filled in by [`RI::createDevice`](createDevice.md) and never changes. Anything else
about the GPU is queried natively ([P60](../design-notes/decisions.md#p60)).

| Field | Meaning | Vulkan | D3D12 |
|---|---|---|---|
| `maxResourceDescriptors` | the most view slots in a [descriptor heap](descriptorHeap.md) | the smallest update-after-bind descriptor limit of the GPU, at most 1,000,000 | 1,000,000 |
| `maxSamplerDescriptors` | the most sampler slots in a heap | the GPU's update-after-bind sampler limit, at most 2048 | 2048 |
| `maxRootDataSize` | the most bytes of [root data](../concepts/root-data.md) a shader may declare | `maxPushConstantsSize` (at least 128) | 256 (64 root constants) |
| `maxBufferViewSize` | the most bytes one read or read-write [buffer view](writeDescriptors.md) covers, besides `kMaxBufferViewElements` ([P106](../design-notes/decisions.md#p106)) | `maxStorageBufferRange` | no byte limit beyond the element limit: `kMaxBufferViewElements` × 2048 (the largest structure stride) |
| `maxConstantViewSize` | the most bytes one constant buffer view covers ([P106](../design-notes/decisions.md#p106)) | `maxUniformBufferRange`, at most 65,536 | 65,536 (4096 × 16 bytes) |
| `gpuUploadMemory` | whether [`MemoryKind::eGPUUpload`](memory.md) can be allocated | all of the largest device-local heap is host-visible (ReBAR) | `GPUUploadHeapSupported` |
| `timestampFrequency` | [timestamp](timestamps.md) ticks per second | `1e9 / timestampPeriod` (the period is in nanoseconds per tick) | `ID3D12CommandQueue::GetTimestampFrequency` of the graphics queue |
| `meshShaders` | task and mesh shaders, [`cmdDrawMesh`](cmdDraw.md) | `VK_EXT_mesh_shader` with `taskShader` and `meshShader` | `D3D12_OPTIONS7::MeshShaderTier` at least tier 1 |
| `geometryShaders` | [geometry shaders](graphicsPipeline.md) | `geometryShader` | always |
| `tessellation` | hull and domain shaders, `Topology::ePatchList` | `tessellationShader` | always |
| `indirectRootData` | indirect Level 2, [`cmdExecuteIndirectWithRootData`](indirect.md) | `VK_EXT_device_generated_commands` (Vulkan 1.4 devices) | always |
| `rayTracing`, `accelerationStructureScratchAlignment` | [inline ray tracing](raytracing.md) and the scratch address alignment | `VK_KHR_acceleration_structure` + `VK_KHR_ray_query`, and acceleration structures accepted as a mutable descriptor type; `minAccelerationStructureScratchOffsetAlignment` | raytracing tier 1.1; 256 bytes |
| `samplerMinMax` | [`Reduction::eMin` / `eMax` samplers](writeSamplers.md) | `samplerFilterMinmax` (Vulkan 1.2) | `D3D12_OPTIONS::TiledResourcesTier` at least 2 |
| `int64BufferAtomics` | 64-bit integer atomics (`InterlockedAdd`, `InterlockedMin`, … on `uint64_t`/`int64_t`) on buffers reached through the heap ([P105](../design-notes/decisions.md#p105)) | `shaderBufferInt64Atomics` and `shaderInt64` | `D3D12_OPTIONS11::AtomicInt64OnDescriptorHeapResourceSupported` and `D3D12_OPTIONS1::Int64ShaderOps` |
| `int64SharedAtomics` | the same on `groupshared` memory ([P105](../design-notes/decisions.md#p105)) | `shaderSharedInt64Atomics` and `shaderInt64` | `D3D12_OPTIONS9::AtomicInt64OnGroupSharedSupported` and `Int64ShaderOps` |
| `workGraphs` | [work graphs](workgraphs.md) | never (D3D12 only) | `D3D12_OPTIONS21::WorkGraphsTier` at least 1.0 |
| `depthResolveModes`, `stencilResolveModes` | the depth and stencil [resolve](resolve.md) modes, `resolveModeBit(mode)` each | `VkPhysicalDeviceDepthStencilResolveProperties` | programmable sample positions tier 2: depth average/min/max, stencil min/max |

## Signature

--8<-- "generated/signatures/RI.DeviceLimits.md"

## Example

```cpp
--8<-- "snippets/ri/compute.cpp:limits"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:limits"
    ```

=== "Vulkan"

    The limits come from `VkPhysicalDeviceLimits` and `VkPhysicalDeviceVulkan12Properties`
    (`maxDescriptorSetUpdateAfterBind*`, `maxPerStageDescriptorUpdateAfterBind*`), and from
    the memory heaps and types for `gpuUploadMemory`. RI also checks with
    `vkGetDescriptorSetLayoutSupport` that a heap of the maximum size can be created.
    The timestamp period is `VkPhysicalDeviceLimits::timestampPeriod`.

=== "D3D12"

    The heap limits are fixed by resource binding tier 3
    (`D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2`,
    `D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE`); a root signature holds 64 32-bit values;
    `gpuUploadMemory` is `D3D12_FEATURE_DATA_D3D12_OPTIONS16::GPUUploadHeapSupported`.
    The timestamp frequency belongs to a queue: RI asks its graphics queue.

## Rules and errors

- A heap larger than the limits is a **programmer error** at
  [`createDescriptorHeap`](descriptorHeap.md).
- A shader with more root data than `maxRootDataSize` fails
  [`createComputePipeline`](pipeline.md) with an **unsupported** error.
- `eGPUUpload` memory without `gpuUploadMemory` is an **unsupported** error.
- A buffer view larger than `maxBufferViewSize` or `maxConstantViewSize` is a **programmer
  error** at [`writeDescriptors`](writeDescriptors.md) in validation builds.
- `timestampFrequency` is a device value while RI has one queue; it will move to the queue when
  queues are added ([P71](../design-notes/decisions.md#p71)). Only differences of timestamps
  are meaningful.

## Related decisions

- [P59](../design-notes/decisions.md#p59): heap sizes within limits RI reports.
- [P44](../design-notes/decisions.md#p44): root data size.
- [P32](../design-notes/decisions.md#p32): `eGPUUpload` availability.
- [P94](../design-notes/decisions.md#p94): the limits' values.
- [P71](../design-notes/decisions.md#p71): the timestamp frequency is a device query.
- [P106](../design-notes/decisions.md#p106): buffer view sizes.

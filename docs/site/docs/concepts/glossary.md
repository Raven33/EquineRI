# Vulkan and D3D12 to RI

A conversion chart: each Vulkan and D3D12 concept, its RI counterpart, and how RI maps it. Use
it to read RI with Vulkan or D3D12 in mind, or to port code to RI. "—" means the concept has
no RI form: RI hides it, or leaves it to [native access](../native/index.md).

Features RI v1 does not have at all, with what to do instead, are listed at the end:
[Not in RI v1](#not-in-ri-v1).

## Devices and setup

| Vulkan | D3D12 | RI | How RI maps it |
|---|---|---|---|
| `VkInstance` | `IDXGIFactory` | — | Hidden: each device makes its own ([P92](../design-notes/decisions.md#p92)). |
| `VkPhysicalDevice` | `IDXGIAdapter` | [`RI::Gpu`](../reference/Gpu.md), [`listGpus`](../reference/listGpus.md) | A portable description; `gpu.id` is the device UUID / adapter LUID. |
| `VkDevice` | `ID3D12Device` | [`RI::Device`](../reference/createDevice.md) | Every feature the GPU reports is on; RI enables the extensions it uses. |
| Features, properties, limits | `CheckFeatureSupport` | [`device.limits`](../reference/DeviceLimits.md) | Flags for asymmetric features and the limits RI needs ([P11](../design-notes/decisions.md#p11)). |
| `vkGetPhysicalDeviceFormatProperties` | `CheckFeatureSupport(FORMAT_SUPPORT)` | [`formatSupported`](../reference/formatSupported.md) | Per format and usage. |
| `VkQueue` (graphics) | `ID3D12CommandQueue` (direct) | `device.graphicsQueue` | One queue in v1 ([P37](../design-notes/decisions.md#p37)). |
| Validation layer, GPU-AV | Debug layer, GBV | `EQUINE_VALIDATION` builds | Both turned on with RI's own checks ([P21](../design-notes/decisions.md#p21)). |

## Memory and resources

| Vulkan | D3D12 | RI | How RI maps it |
|---|---|---|---|
| `VkDeviceMemory` | `ID3D12Heap` | [`RI::Memory`](../reference/memory.md) | A block of one [memory kind](memory.md); you place resources in it ([P31](../design-notes/decisions.md#p31)). |
| Memory types (`DEVICE_LOCAL`, `HOST_VISIBLE`, …) | Heap types (`DEFAULT`, `UPLOAD`, `READBACK`, `GPU_UPLOAD`) | `MemoryKind` (`eGPU`, `eUpload`, `eReadback`, `eGPUUpload`) | RI picks the native type per kind ([P32](../design-notes/decisions.md#p32)). |
| `vkMapMemory` | `ID3D12Resource::Map` | `memory.mapped`, `buffer.mapped` | CPU kinds stay mapped for their lifetime ([P33](../design-notes/decisions.md#p33)). |
| `vkGet*MemoryRequirements` | `GetResourceAllocationInfo` | [`bufferRequirements`](../reference/bufferRequirements.md), [`textureRequirements`](../reference/textureRequirements.md) | Size, alignment, and the kinds that can hold it. |
| `VkBuffer` + `vkBindBufferMemory` | `CreatePlacedResource` (buffer) | [`RI::Buffer`](../reference/buffer.md) | Placed at an offset you choose; always has a GPU address. |
| `vkGetBufferDeviceAddress` | `GetGPUVirtualAddress` | `buffer.gpuAddress` | Always available ([P61](../design-notes/decisions.md#p61)). |
| `VkImage` + `vkBindImageMemory` | `CreatePlacedResource` (texture) | [`RI::Texture`](../reference/texture.md) | Usage flags decide its hidden resting layout ([P58](../design-notes/decisions.md#p58)). |
| Buffer and image usage flags | Resource flags | `BufferUsage`, `TextureUsage` | Explicit; also select the resting layout. |
| `VK_IMAGE_TYPE_3D`, `CUBE_COMPATIBLE` | `TEXTURE3D`, `TEXTURECUBE` views | `TextureShape::e3D`, `eCube` | Cubes are 6 layers per cube; views of whole cubes read as cubes ([shapes](../reference/texture.md#shapes)). |
| `VkImageView` | SRV / UAV / RTV / DSV | [`RI::TextureView`](../reference/view.md) | One view object; RI makes the RTV/DSV descriptors it needs ([P73](../design-notes/decisions.md#p73)). |
| Dedicated allocation | Committed resource | — | Not in RI; allocate a block for the one resource. |
| Sparse binding | Reserved resources | — | Not in v1 (F1). |

## Descriptors and binding

| Vulkan | D3D12 | RI | How RI maps it |
|---|---|---|---|
| Descriptor set + layout + pool | Shader-visible descriptor heap | [`RI::DescriptorHeap`](../reference/descriptorHeap.md) | One resource heap and one sampler heap; shaders index them (`ResourceDescriptorHeap[i]`) ([P94](../design-notes/decisions.md#p94)). |
| `vkUpdateDescriptorSets` | `CreateShaderResourceView` … into a heap | [`writeDescriptors`](../reference/writeDescriptors.md) | You choose the slot ([P42](../design-notes/decisions.md#p42)). |
| Sampled image / storage buffer (read) | SRV | `RI::ReadView` | |
| Storage image / storage buffer | UAV | `RI::ReadWriteView` | |
| Uniform buffer | CBV | `RI::ConstantBufferView` | |
| Acceleration structure descriptor | SRV (raytracing AS) | `RI::AccelerationStructureView` | |
| `VkSampler` | Sampler descriptor | `RI::SamplerDesc`, [`writeSamplers`](../reference/writeSamplers.md) | No sampler object: the description goes into a sampler-heap slot; Vulkan samplers are cached per description. |
| `vkCmdBindDescriptorSets` | `SetDescriptorHeaps` | [`cmdSetHeap`](../reference/cmdSetHeap.md) | Before the first pipeline ([P94](../design-notes/decisions.md#p94)). |
| Push constants | Root constants | **Root data**, [`cmdSetRootData`](../reference/cmdSetRootData.md) | Declared in the shader with `EQ_ROOT_DATA` ([root data](root-data.md)). |
| `VkPipelineLayout` | Root signature | — | Built by RI from the shader ([P44](../design-notes/decisions.md#p44)). |
| Bound vertex buffers | Vertex buffer views | — | Vertex pulling: read vertices from buffers in the shader ([P39](../design-notes/decisions.md#p39)). |
| `vkCmdBindIndexBuffer` | `IASetIndexBuffer` | [`cmdSetIndexBuffer`](../reference/cmdDraw.md) | |

## Pipelines and shaders

| Vulkan | D3D12 | RI | How RI maps it |
|---|---|---|---|
| SPIR-V `VkShaderModule` | DXIL bytecode | `RI::Bytecode` | RI takes bytecode only; one HLSL source, compiled for both ([compiling shaders](shaders.md)). |
| Compute pipeline | Compute PSO | [`createComputePipeline`](../reference/pipeline.md) | |
| Graphics pipeline (dynamic rendering) | Graphics PSO | [`createGraphicsPipeline`](../reference/graphicsPipeline.md) | Formats, blend, depth, stencil (reference included), cull, topology. |
| Mesh / task pipeline | Mesh shader PSO | `createGraphicsPipeline` with task and mesh shaders | `device.limits.meshShaders`. |
| Tessellation, geometry shaders | HS, DS, GS | `hull`, `domain`, `geometry` in `GraphicsPipelineDesc` | `device.limits.tessellation`, `geometryShaders` ([P102](../design-notes/decisions.md#p102)). |
| Polygon mode, depth bias, depth clamp | `FillMode`, `DepthBias`, `DepthClipEnable` | `fill`, `depthBias`, `depthClamp` | Fixed in the pipeline. |
| Dynamic state | Command-list state | — | Fixed in the pipeline ([P38](../design-notes/decisions.md#p38)), except the viewport, scissor and stencil reference: they come from the attachments and the pipeline until [`cmdSetViewport`, `cmdSetScissor`, `cmdSetStencilReference`](../reference/cmdSetViewport.md) change them. |
| Pipeline cache | Pipeline library | — | Not in RI. |
| `vkCmdBindPipeline` | `SetPipelineState` + root signature | [`cmdSetPipeline`](../reference/cmdSetPipeline.md) | |

## Commands and synchronization

| Vulkan | D3D12 | RI | How RI maps it |
|---|---|---|---|
| `VkCommandPool` + `VkCommandBuffer` | Allocator + command list | [`RI::CommandList`](../reference/commandList.md) | One pool each; record, submit once, reset after it completes ([P56](../design-notes/decisions.md#p56)). |
| Secondary command buffers | Bundles | — | Not in RI. |
| `vkQueueSubmit2` | `ExecuteCommandLists` + `Signal` | [`submit`](../reference/submit.md) | Returns a [timeline point](timeline.md). |
| Timeline semaphore | Fence | `RI::TimelinePoint`, `wait`, `isReached` | One timeline per queue ([P20](../design-notes/decisions.md#p20)). |
| Binary semaphores | — | — | Hidden inside swapchains. |
| `VkEvent` | — | — | Not in RI. |
| `vkCmdPipelineBarrier2` (memory barrier) | Global barrier | [`cmdBarrier(cmd, before, after)`](../reference/cmdBarrier.md) | Stage masks only: you name what must finish and what waits ([barriers](barriers.md)). |
| Image layouts, layout transitions | Resource states, layouts | — | Never visible: textures rest in one layout; RI switches around its own operations ([layouts](layouts.md), [P16](../design-notes/decisions.md#p16)). |
| `VK_IMAGE_LAYOUT_UNDEFINED` to drop contents | `DISCARD` barrier, `DiscardResource` | [`cmdDiscard`](../reference/cmdDiscard.md) | Required when a texture takes over memory ([P69](../design-notes/decisions.md#p69)). |

## Rendering

| Vulkan | D3D12 | RI | How RI maps it |
|---|---|---|---|
| `vkCmdBeginRendering` | `OMSetRenderTargets` | [`cmdBeginRendering`](../reference/cmdBeginRendering.md) / `cmdEndRendering` | A **rendering section** with load and store operations ([rendering](rendering.md)). |
| Render pass, subpasses, framebuffer | Render pass (`BeginRenderPass`) | — | Not in RI: rendering sections only. |
| `loadOp` / `storeOp` | Beginning / ending access | `LoadOp`, `StoreOp` | |
| Resolve attachments | `ResolveSubresource(Region)` | `resolveTo` on an attachment, [`cmdResolve`](../reference/resolve.md) | Color averages; depth and stencil use the modes the GPU reports ([P76](../design-notes/decisions.md#p76)). |
| `vkCmdDraw`, `vkCmdDrawIndexed` | `DrawInstanced`, `DrawIndexedInstanced` | [`cmdDraw`, `cmdDrawIndexed`](../reference/cmdDraw.md) | |
| `vkCmdDrawMeshTasksEXT` | `DispatchMesh` | `cmdDrawMesh` | |
| `vkCmdDispatch` | `Dispatch` | [`cmdDispatch`](../reference/cmdDispatch.md) | |
| `vkCmdClearColorImage`, `vkCmdFillBuffer` | `Clear*View` | [`cmdClearTexture`, `cmdClearBuffer`](../reference/cmdClear.md) | No heap slot needed ([P65](../design-notes/decisions.md#p65)). |
| Viewport y-down (Vulkan) | Viewport y-up | — | Clip space is y-up on both: RI flips Vulkan's viewport. |

## Copies, queries, indirect

| Vulkan | D3D12 | RI | How RI maps it |
|---|---|---|---|
| `vkCmdCopyBuffer` | `CopyBufferRegion` | [`cmdCopyBuffer`](../reference/cmdCopyBuffer.md) | |
| `vkCmdCopyBufferToImage` / `ImageToBuffer` | `CopyTextureRegion` (footprint) | [`cmdCopyBufferToTexture`, `cmdCopyTextureToBuffer`](../reference/copies.md), `copyLayout` | `copyLayout` gives the row pitch both backends accept. |
| `vkCmdCopyImage` | `CopyTextureRegion` | `cmdCopyTexture` | |
| `VkQueryPool` | `ID3D12QueryHeap` | [`RI::QueryPool`](../reference/queryPool.md) | Timestamps, occlusion, pipeline statistics. |
| `vkCmdWriteTimestamp2` | `EndQuery(TIMESTAMP)` | [`cmdWriteTimestamp`](../reference/timestamps.md) | |
| `vkCmdCopyQueryPoolResults` | `ResolveQueryData` | `cmdResolveQueries` | Into a buffer you own. |
| `vkCmdDraw*Indirect(Count)` | `ExecuteIndirect` | [`cmdExecuteIndirect`](../reference/indirect.md) | Level 1. |
| `VK_EXT_device_generated_commands` | `ExecuteIndirect` with root constants | `cmdExecuteIndirectWithRootData` | Level 2: root data per draw; scratch is yours (`indirectScratchRequirements`). |

## Ray tracing and work graphs

| Vulkan | D3D12 | RI | How RI maps it |
|---|---|---|---|
| `VkAccelerationStructureKHR` | Acceleration structure in a buffer | [`RI::AccelerationStructure`](../reference/raytracing.md) | Placed in your memory. |
| `vkCmdBuildAccelerationStructuresKHR` | `BuildRaytracingAccelerationStructure` | `cmdBuildBlas`, `cmdBuildTlas` | With update; scratch is yours. |
| Ray query (`VK_KHR_ray_query`) | Inline ray tracing (tier 1.1) | `RayQuery` in any shader | Inline only ([P49](../design-notes/decisions.md#p49)). |
| Ray tracing pipelines, SBT | State objects, shader tables | — | Not in v1. |
| (`VK_AMDX_shader_enqueue`) | Work graphs | [`RI::WorkGraph`](../reference/workgraphs.md) | D3D12 only ([P50](../design-notes/decisions.md#p50)). |

## Presentation and debugging

| Vulkan | D3D12 | RI | How RI maps it |
|---|---|---|---|
| `VkSurfaceKHR` | `HWND` | [`RI::Window`](../reference/Window.md) | OS handles; RI creates no window ([P43](../design-notes/decisions.md#p43)). |
| `VkSwapchainKHR` | `IDXGISwapChain` | [`RI::Swapchain`](../reference/swapchain.md) | |
| `vkAcquireNextImageKHR` | `GetCurrentBackBufferIndex` | `acquire` | Returns the screen texture, ready to render. |
| `vkQueuePresentKHR` | `Present` | `present` | |
| `vkSetDebugUtilsObjectNameEXT` | `SetName` | [`setName`](../reference/debugging.md) | |
| Debug labels | PIX events | `cmdBeginMarker`, `cmdEndMarker` | |
| Device fault | DRED | — | Printed when the device is lost (validation builds) ([P53](../design-notes/decisions.md#p53)). |
| Any native handle | Any native object | `RI::Vk::native(...)`, `RI::D3D12::native(...)` | Through the backend's `Native.hpp` ([native access](../native/index.md)). |

## Not in RI v1

What a Vulkan or D3D12 program may use that RI v1 does not offer. "Native" means a
[native pipeline](../native/import.md) (`importPipeline`) or a
[native section](../native/sections.md), written once per backend.

| Vulkan | D3D12 | Instead |
|---|---|---|
| Compressed formats (BC, ETC2, ASTC) | BC formats | Uncompressed formats ([`Format`](../reference/texture.md)); decompress when loading. |
| Line width, point size | — (1 pixel only) | Quads: one per point or line segment. |
| Alpha to coverage, sample mask, conservative rasterization, shading rate | The same | Native. |
| `vkCmdBlitImage`, mip generation | — | A compute or pixel shader that downsamples; [`cmdCopyTexture`](../reference/copies.md) copies at the same size only. |
| Specialization constants | — | Root data, or `#define`s with the [shader compiler helper](../helpers/shader-compiler.md). |
| Multiview, viewport arrays | View instancing, viewport arrays | One viewport per draw: [`cmdSetViewport`](../reference/cmdSetViewport.md) between draws, or one section per view. |
| Recording on several threads | Recording on several threads | One thread in v1 ([timeline](timeline.md), [P56](../design-notes/decisions.md#p56)). |
| Compute and transfer queues | Compute and copy queues | The graphics queue only ([P37](../design-notes/decisions.md#p37)). |
| Ray tracing pipelines | State objects | Inline ray queries ([P49](../design-notes/decisions.md#p49)). |

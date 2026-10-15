# Reference

One page per public function or structure of RI. Every page has the same shape:

1. **What it does**: a plain explanation.
2. **Signature**: generated from the header, so it is always current.
3. **Example**: RI code taken from a source file that compiles.
4. **The same in native code**: tabs with a canonical Vulkan and D3D12 version.
5. **Rules and errors**: what RI checks and which error it reports.
6. **Related decisions**: the design decisions behind it.

## How names work

You write `RI::createDevice`, whatever the backend. Each backend defines its own functions
in its own namespace (`RI::Vk::`, `RI::D3D12::`), and `<RI/RI.hpp>` makes `RI::` name the
backend your target links ([P83](../design-notes/decisions.md#p83),
[P90](../design-notes/decisions.md#p90)). Functions that expose Vulkan or D3D12 types are
not reachable through `RI::`; they are under [Native access](../native/index.md).

RI objects are **plain structs with public fields** and operations are **free functions**
([P81](../design-notes/decisions.md#p81), [P82](../design-notes/decisions.md#p82)).
Descriptions are filled with C++20 designated initializers, and fields you leave out keep
their defaults ([P86](../design-notes/decisions.md#p86)):

```cpp
RI::Device device = RI::createDevice({.gpu = chosen});
```

## GPUs and devices

| Page | What it is for |
|---|---|
| [`RI::listGpus`](listGpus.md) | The GPUs that can run RI. |
| [`RI::createDevice`, `RI::destroyDevice`](createDevice.md) | Open a GPU for use, and close it. |
| [`RI::Gpu`](Gpu.md) | The facts about one GPU: name, vendor, memory, ID. |
| [`RI::DeviceDesc`, `RI::Device`](DeviceDesc.md) | What you pass to `createDevice`, and what you get back. |
| [`RI::DeviceLimits`](DeviceLimits.md) | Heap sizes, root data size, `eGPUUpload` availability, timestamp frequency. |
| [`RI::Window`](Window.md) | A window's OS handles, for the future swapchain. |

## Memory and buffers

| Page | What it is for |
|---|---|
| [`RI::allocateMemory`, `RI::freeMemory`](memory.md) | Memory blocks of the four kinds. |
| [`RI::bufferRequirements`](bufferRequirements.md) | Size and alignment of a buffer before placing it. |
| [`RI::createBuffer`, `RI::destroyBuffer`](buffer.md) | Buffers placed in memory, with usage flags. |

## Textures and views

| Page | What it is for |
|---|---|
| [`RI::formatSupported`](formatSupported.md) | Whether the GPU supports a format for a set of uses. |
| [`RI::textureRequirements`, `RI::fits`](textureRequirements.md) | Size, alignment and memory kinds of a texture before placing it. |
| [`RI::createTexture`, `RI::destroyTexture`](texture.md) | Textures placed in memory, with usage flags and formats. |
| [`RI::createView`, `RI::destroyView`](view.md) | Mip ranges, layer ranges, aspects and other formats of a texture. |

## Descriptors

| Page | What it is for |
|---|---|
| [`RI::createDescriptorHeap`, `RI::destroyDescriptorHeap`](descriptorHeap.md) | The heap bindless shaders index. |
| [`RI::writeDescriptors`](writeDescriptors.md) | Texture and buffer views in heap slots. |

## Pipelines

| Page | What it is for |
|---|---|
| [`RI::createComputePipeline`, `RI::destroyPipeline`](pipeline.md) | A compute shader ready to run. |
| [`RI::createGraphicsPipeline`](graphicsPipeline.md) | Vertex and pixel shaders with all their state. |

## Command lists

| Page | What it is for |
|---|---|
| [`RI::createCommandList`, `RI::resetCommandList`, `RI::destroyCommandList`](commandList.md) | Lists to record GPU work into, reused after their timeline point. |
| [`RI::cmdBegin`, `RI::cmdEnd`](cmdBegin.md) | Open and close a list for recording. |
| [`RI::cmdSetHeap`](cmdSetHeap.md) | The descriptor heap shaders index (before the pipeline). |
| [`RI::cmdSetPipeline`](cmdSetPipeline.md) | The shader that dispatches run. |
| [`RI::cmdSetRootData`](cmdSetRootData.md) | The shader's root data. |
| [`RI::cmdDispatch`](cmdDispatch.md) | Run a compute shader. |
| [`RI::cmdBarrier`](cmdBarrier.md) | Make earlier writes visible to later commands. |
| [`RI::cmdCopyBuffer`](cmdCopyBuffer.md) | Copy bytes between buffers. |
| [`RI::cmdBeginRendering`, `RI::cmdEndRendering`](cmdBeginRendering.md) | Rendering sections: attachments, load and store operations. |
| [`RI::cmdDraw`](cmdDraw.md) | Draw vertices in a rendering section. |
| [`RI::cmdDiscard`](cmdDiscard.md) | Reset a texture that takes over another resource's memory. |
| [`RI::cmdClearTexture`, `RI::cmdClearBuffer`](cmdClear.md) | Clears outside rendering sections. |
| [`RI::cmdWriteSmallData`](cmdWriteSmallData.md) | A few values written into a buffer from the command list. |
| [`RI::cmdWriteTimestamp`, `RI::cmdResolveQueries`, `RI::cmdResetQueries`](timestamps.md) | Measure GPU time. |

## Queries

| Page | What it is for |
|---|---|
| [`RI::createQueryPool`, `RI::destroyQueryPool`, `RI::resetQueries`](queryPool.md) | Pools of timestamp queries. |

## Submission

| Page | What it is for |
|---|---|
| [`RI::submit`, `RI::wait`, `RI::isReached`](submit.md) | Send work to the GPU; know when it is done. |

What is still missing is listed in the [coverage report](../about/coverage.md).

// Backend conformance (P83): compiled once per backend, never run. Every
// backend must offer the same portable functions with the same signatures and
// the same portable fields, so this file builds against each of them.

#include <RI/RI.hpp>

#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

#define EQ_SIGNATURE(name, type) static_assert(std::is_same_v<decltype(static_cast<type>(&RI::name)), type>)
#define EQ_FIELD(type, field, fieldType) static_assert(std::is_same_v<decltype(RI::type::field), fieldType>)

// Devices (P60, P92)
EQ_SIGNATURE(listGpus, std::vector<RI::Gpu> (*)());
EQ_SIGNATURE(createDevice, RI::Device (*)(const RI::DeviceDesc &));
EQ_SIGNATURE(destroyDevice, void (*)(RI::Device &));
EQ_FIELD(Device, gpu, RI::Gpu);
EQ_FIELD(Device, limits, RI::DeviceLimits);
EQ_FIELD(Device, graphicsQueue, RI::Queue);

// Memory and buffers (P31, P32, P33, P58, P61)
EQ_SIGNATURE(allocateMemory, RI::Memory (*)(RI::Device &, RI::MemoryKind, uint64, const RI::MemoryRequirements *));
EQ_SIGNATURE(freeMemory, void (*)(RI::Device &, RI::Memory &));
EQ_SIGNATURE(bufferRequirements, RI::MemoryRequirements (*)(RI::Device &, const RI::BufferDesc &));
EQ_SIGNATURE(createBuffer, RI::Buffer (*)(RI::Device &, const RI::Memory &, uint64, const RI::BufferDesc &));
EQ_SIGNATURE(destroyBuffer, void (*)(RI::Device &, RI::Buffer &));
EQ_FIELD(Memory, kind, RI::MemoryKind);
EQ_FIELD(Memory, size, uint64);
EQ_FIELD(Memory, mapped, void *);
EQ_FIELD(Buffer, size, uint64);
EQ_FIELD(Buffer, usage, RI::BufferUsage);
EQ_FIELD(Buffer, gpuAddress, uint64);
EQ_FIELD(Buffer, mapped, void *);

// Descriptors (P42, P59, P87, P94)
EQ_SIGNATURE(createDescriptorHeap, RI::DescriptorHeap (*)(RI::Device &, const RI::DescriptorHeapDesc &));
EQ_SIGNATURE(destroyDescriptorHeap, void (*)(RI::Device &, RI::DescriptorHeap &));
EQ_SIGNATURE(writeDescriptors, void (*)(RI::Device &, RI::DescriptorHeap &, std::span<const RI::DescriptorWrite>));
EQ_SIGNATURE(writeDescriptors, void (*)(RI::Device &, RI::DescriptorHeap &, std::initializer_list<RI::DescriptorWrite>));
EQ_SIGNATURE(writeSamplers, void (*)(RI::Device &, RI::DescriptorHeap &, std::span<const RI::SamplerWrite>));
EQ_SIGNATURE(writeSamplers, void (*)(RI::Device &, RI::DescriptorHeap &, std::initializer_list<RI::SamplerWrite>));
EQ_FIELD(DescriptorHeap, resourceCount, uint32);
EQ_FIELD(DescriptorHeap, samplerCount, uint32);
EQ_FIELD(ReadView, buffer, RI::Buffer);
EQ_FIELD(ReadView, offset, uint64);
EQ_FIELD(ReadView, size, uint64);
EQ_FIELD(ReadView, stride, uint32);
EQ_FIELD(ReadWriteView, buffer, RI::Buffer);
EQ_FIELD(ReadWriteView, offset, uint64);
EQ_FIELD(ReadWriteView, size, uint64);
EQ_FIELD(ReadWriteView, stride, uint32);
EQ_FIELD(ConstantBufferView, buffer, RI::Buffer);
EQ_FIELD(ConstantBufferView, offset, uint64);
EQ_FIELD(ConstantBufferView, size, uint64);
EQ_FIELD(DescriptorWrite, slot, uint32);
EQ_FIELD(DescriptorWrite, descriptor, RI::Descriptor);

// Pipelines (P38, P45)
EQ_SIGNATURE(createComputePipeline, RI::Pipeline (*)(RI::Device &, const RI::ComputePipelineDesc &));
EQ_SIGNATURE(destroyPipeline, void (*)(RI::Device &, RI::Pipeline &));
EQ_FIELD(Pipeline, rootDataSize, uint32);

// Command lists and submission (P20, P34, P36, P47, P56, P85)
EQ_SIGNATURE(createCommandList, RI::CommandList (*)(RI::Device &));
EQ_SIGNATURE(destroyCommandList, void (*)(RI::Device &, RI::CommandList &));
EQ_SIGNATURE(resetCommandList, void (*)(RI::CommandList &));
EQ_SIGNATURE(cmdBegin, void (*)(RI::CommandList &));
EQ_SIGNATURE(cmdEnd, void (*)(RI::CommandList &));
EQ_SIGNATURE(cmdSetHeap, void (*)(RI::CommandList &, const RI::DescriptorHeap &));
EQ_SIGNATURE(cmdSetPipeline, void (*)(RI::CommandList &, const RI::Pipeline &));
EQ_SIGNATURE(cmdSetRootData, void (*)(RI::CommandList &, const void *, uint32));
EQ_SIGNATURE(cmdDispatch, void (*)(RI::CommandList &, uint32, uint32, uint32));
EQ_SIGNATURE(cmdBarrier, void (*)(RI::CommandList &, RI::Stage, RI::Stage));
EQ_SIGNATURE(cmdCopyBuffer,
             void (*)(RI::CommandList &, const RI::Buffer &, uint64, const RI::Buffer &, uint64, uint64));
EQ_SIGNATURE(submit, RI::TimelinePoint (*)(RI::Queue &, RI::CommandList &));
EQ_SIGNATURE(wait, void (*)(RI::Queue &, RI::TimelinePoint));
EQ_SIGNATURE(isReached, bool (*)(RI::Queue &, RI::TimelinePoint));

// Limits and requirements (P58, P71)
EQ_FIELD(DeviceLimits, timestampFrequency, uint64);
EQ_FIELD(DeviceLimits, int64BufferAtomics, bool);
EQ_FIELD(DeviceLimits, int64SharedAtomics, bool);
EQ_FIELD(MemoryRequirements, kinds, uint32);

// Textures and views (P58, P69, P73, P74)
EQ_SIGNATURE(formatSupported, bool (*)(RI::Device &, RI::Format, RI::TextureUsage, uint32));
EQ_SIGNATURE(textureRequirements, RI::MemoryRequirements (*)(RI::Device &, const RI::TextureDesc &));
EQ_SIGNATURE(createTexture, RI::Texture (*)(RI::Device &, const RI::Memory &, uint64, const RI::TextureDesc &));
EQ_SIGNATURE(destroyTexture, void (*)(RI::Device &, RI::Texture &));
EQ_SIGNATURE(createView, RI::TextureView (*)(RI::Device &, const RI::Texture &, const RI::TextureViewDesc &));
EQ_SIGNATURE(destroyView, void (*)(RI::Device &, RI::TextureView &));
EQ_FIELD(Texture, width, uint32);
EQ_FIELD(Texture, height, uint32);
EQ_FIELD(Texture, layers, uint32);
EQ_FIELD(Texture, mips, uint32);
EQ_FIELD(Texture, samples, uint32);
EQ_FIELD(Texture, format, RI::Format);
EQ_FIELD(Texture, usage, RI::TextureUsage);
EQ_FIELD(Texture, view, RI::TextureView);
EQ_FIELD(TextureView, format, RI::Format);
EQ_FIELD(TextureView, width, uint32);
EQ_FIELD(TextureView, height, uint32);
EQ_FIELD(TextureView, samples, uint32);
EQ_FIELD(ReadView, texture, RI::TextureView);
EQ_FIELD(ReadWriteView, texture, RI::TextureView);

// Queries (P51, P71)
EQ_SIGNATURE(createQueryPool, RI::QueryPool (*)(RI::Device &, const RI::QueryPoolDesc &));
EQ_SIGNATURE(destroyQueryPool, void (*)(RI::Device &, RI::QueryPool &));
EQ_SIGNATURE(resetQueries, void (*)(RI::Device &, RI::QueryPool &, uint32, uint32));
EQ_FIELD(QueryPool, type, RI::QueryType);
EQ_FIELD(QueryPool, count, uint32);

// Rendering and graphics pipelines (P38, P40, P75)
EQ_SIGNATURE(createGraphicsPipeline, RI::Pipeline (*)(RI::Device &, const RI::GraphicsPipelineDesc &));
EQ_FIELD(ColorAttachment, view, RI::TextureView);
EQ_FIELD(ColorAttachment, load, RI::LoadOp);
EQ_FIELD(ColorAttachment, store, RI::StoreOp);
EQ_FIELD(ColorAttachment, clear, RI::ClearValue);
EQ_FIELD(DepthAttachment, view, RI::TextureView);
EQ_FIELD(DepthAttachment, load, RI::LoadOp);
EQ_FIELD(DepthAttachment, store, RI::StoreOp);
EQ_FIELD(DepthAttachment, stencilLoad, RI::LoadOp);
EQ_FIELD(DepthAttachment, stencilStore, RI::StoreOp);
EQ_FIELD(DepthAttachment, clear, RI::ClearValue);
EQ_FIELD(DepthAttachment, depthReadOnly, bool);
EQ_FIELD(DepthAttachment, stencilReadOnly, bool);
EQ_FIELD(RenderingDesc, colors, std::vector<RI::ColorAttachment>);
EQ_FIELD(RenderingDesc, depth, std::optional<RI::DepthAttachment>);
EQ_SIGNATURE(cmdBeginRendering, void (*)(RI::CommandList &, const RI::RenderingDesc &));
EQ_SIGNATURE(cmdBeginRendering, void (*)(RI::CommandList &, RI::RenderingSize));
EQ_SIGNATURE(cmdEndRendering, void (*)(RI::CommandList &));
EQ_SIGNATURE(cmdDraw, void (*)(RI::CommandList &, uint32, uint32, uint32, uint32));
EQ_SIGNATURE(cmdDiscard, void (*)(RI::CommandList &, const RI::TextureView &));
EQ_SIGNATURE(cmdClearTexture, void (*)(RI::CommandList &, const RI::TextureView &, const RI::ClearValue &));
EQ_SIGNATURE(cmdClearBuffer, void (*)(RI::CommandList &, const RI::Buffer &, uint32));
EQ_SIGNATURE(cmdWriteTimestamp, void (*)(RI::CommandList &, const RI::QueryPool &, uint32));
EQ_SIGNATURE(cmdResetQueries, void (*)(RI::CommandList &, const RI::QueryPool &, uint32, uint32));
EQ_SIGNATURE(cmdResolveQueries,
             void (*)(RI::CommandList &, const RI::QueryPool &, uint32, uint32, const RI::Buffer &, uint64));

// Copies (P47) and queries (P71)
EQ_SIGNATURE(copyLayout, RI::CopyLayout (*)(RI::Device &, const RI::TextureView &));
EQ_SIGNATURE(cmdCopyBufferToTexture, void (*)(RI::CommandList &, const RI::Buffer &, uint64, const RI::TextureView &));
EQ_SIGNATURE(cmdCopyTextureToBuffer, void (*)(RI::CommandList &, const RI::TextureView &, const RI::Buffer &, uint64));
EQ_SIGNATURE(cmdCopyTexture, void (*)(RI::CommandList &, const RI::TextureView &, const RI::TextureView &));
EQ_SIGNATURE(cmdResolve, void (*)(RI::CommandList &, const RI::TextureView &, const RI::TextureView &, RI::ResolveMode));
EQ_FIELD(ColorAttachment, resolveTo, RI::TextureView);
EQ_FIELD(DepthAttachment, resolveTo, RI::TextureView);
EQ_SIGNATURE(pipelineStatisticsLayout, RI::PipelineStatisticsLayout (*)(RI::Device &));
EQ_SIGNATURE(cmdBeginQuery, void (*)(RI::CommandList &, const RI::QueryPool &, uint32));
EQ_SIGNATURE(cmdEndQuery, void (*)(RI::CommandList &, const RI::QueryPool &, uint32));
EQ_SIGNATURE(cmdSetIndexBuffer, void (*)(RI::CommandList &, const RI::Buffer &, uint64, RI::IndexFormat));
EQ_SIGNATURE(cmdDrawMesh, void (*)(RI::CommandList &, uint32, uint32, uint32));
EQ_SIGNATURE(cmdExecuteIndirect, void (*)(RI::CommandList &, RI::IndirectKind, const RI::Buffer &, uint64, uint32,
                                          const RI::Buffer *, uint64));
EQ_SIGNATURE(cmdExecuteIndirectWithRootData, void (*)(RI::CommandList &, RI::IndirectKind, const RI::Buffer &, uint64,
                                                      uint32, const RI::Buffer *, uint64, const RI::Buffer *, uint64));
EQ_SIGNATURE(indirectScratchRequirements,
             RI::MemoryRequirements (*)(RI::Device &, const RI::Pipeline &, RI::IndirectKind, uint32));
EQ_SIGNATURE(cmdDrawIndexed, void (*)(RI::CommandList &, uint32, uint32, uint32, int32, uint32));

// Swapchains (P43, P63, P64)
EQ_SIGNATURE(surfaceFormats, std::vector<RI::SurfaceFormat> (*)(RI::Device &, const RI::Window &));
EQ_SIGNATURE(presentModeSupported, bool (*)(RI::Device &, const RI::Window &, RI::PresentMode));
EQ_SIGNATURE(screenStorageSupported, bool (*)(RI::Device &, const RI::Window &, RI::Format));
EQ_SIGNATURE(createSwapchain, RI::Swapchain (*)(RI::Device &, const RI::SwapchainDesc &));
EQ_SIGNATURE(resizeSwapchain, void (*)(RI::Device &, RI::Swapchain &, uint32, uint32));
EQ_SIGNATURE(destroySwapchain, void (*)(RI::Device &, RI::Swapchain &));
EQ_SIGNATURE(acquire, RI::Texture (*)(RI::Device &, RI::Swapchain &));
EQ_SIGNATURE(present, void (*)(RI::Queue &, RI::Swapchain &));
EQ_FIELD(Swapchain, width, uint32);
EQ_FIELD(Swapchain, format, RI::SurfaceFormat);

// Ray tracing (P49)
EQ_SIGNATURE(blasSizes, RI::AccelerationStructureSizes (*)(RI::Device &, const RI::BlasDesc &));
EQ_SIGNATURE(tlasSizes, RI::AccelerationStructureSizes (*)(RI::Device &, const RI::TlasDesc &));
EQ_SIGNATURE(accelerationStructureRequirements, RI::MemoryRequirements (*)(RI::Device &, uint64));
EQ_SIGNATURE(createAccelerationStructure,
             RI::AccelerationStructure (*)(RI::Device &, const RI::Memory &, uint64, uint64, bool));
EQ_SIGNATURE(destroyAccelerationStructure, void (*)(RI::Device &, RI::AccelerationStructure &));
EQ_SIGNATURE(cmdBuildBlas, void (*)(RI::CommandList &, const RI::AccelerationStructure &, const RI::BlasDesc &,
                                    const RI::Buffer &, uint64, bool));
EQ_SIGNATURE(cmdBuildTlas, void (*)(RI::CommandList &, const RI::AccelerationStructure &, const RI::TlasDesc &,
                                    const RI::Buffer &, uint64, bool));
EQ_FIELD(AccelerationStructure, gpuAddress, uint64);

// Work graphs (P50, P62)
EQ_SIGNATURE(createWorkGraph, RI::WorkGraph (*)(RI::Device &, const RI::WorkGraphDesc &));
EQ_SIGNATURE(destroyWorkGraph, void (*)(RI::Device &, RI::WorkGraph &));
EQ_SIGNATURE(workGraphEntry, uint32 (*)(const RI::WorkGraph &, std::string_view, uint32));
EQ_SIGNATURE(bindWorkGraphMemory, void (*)(RI::Device &, RI::WorkGraph &, const RI::Buffer &, uint64, uint64));
EQ_SIGNATURE(cmdSetWorkGraph, void (*)(RI::CommandList &, const RI::WorkGraph &));
EQ_SIGNATURE(cmdDispatchGraph, void (*)(RI::CommandList &, std::span<const RI::GraphInput>));
EQ_SIGNATURE(cmdDispatchGraphIndirect, void (*)(RI::CommandList &, uint64, bool));

// Debugging (P53)
EQ_SIGNATURE(setName, void (*)(RI::Device &, const RI::Memory &, std::string_view));
EQ_SIGNATURE(setName, void (*)(RI::Device &, const RI::Buffer &, std::string_view));
EQ_SIGNATURE(setName, void (*)(RI::Device &, const RI::Texture &, std::string_view));
EQ_SIGNATURE(setName, void (*)(RI::Device &, const RI::TextureView &, std::string_view));
EQ_SIGNATURE(setName, void (*)(RI::Device &, const RI::DescriptorHeap &, std::string_view));
EQ_SIGNATURE(setName, void (*)(RI::Device &, const RI::QueryPool &, std::string_view));
EQ_SIGNATURE(setName, void (*)(RI::Device &, const RI::Pipeline &, std::string_view));
EQ_SIGNATURE(setName, void (*)(RI::Device &, const RI::CommandList &, std::string_view));
EQ_SIGNATURE(cmdBeginMarker, void (*)(RI::CommandList &, std::string_view));
EQ_SIGNATURE(cmdEndMarker, void (*)(RI::CommandList &));

// Thin values (P81).
static_assert(std::is_copy_constructible_v<RI::Device> && std::is_copy_constructible_v<RI::Buffer> &&
              std::is_copy_constructible_v<RI::Memory> && std::is_copy_constructible_v<RI::Pipeline> &&
              std::is_copy_constructible_v<RI::DescriptorHeap> && std::is_copy_constructible_v<RI::CommandList> &&
              std::is_copy_constructible_v<RI::Texture> && std::is_copy_constructible_v<RI::TextureView> &&
              std::is_copy_constructible_v<RI::QueryPool>);

struct Root {
    float value;
};

// The documented usage compiles (docs/RIApiProposal.md, sketch 1).
[[maybe_unused]] void usage(RI::Bytecode shader) {
    RI::Device device = RI::createDevice({.gpu = RI::listGpus()[0]});
    RI::Memory memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 1 << 20);
    RI::Buffer buffer = RI::createBuffer(device, memory, 0, {.size = 256, .usage = RI::BufferUsage::eReadWrite});
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 16});
    RI::writeDescriptors(device, heap, {{.slot = 0, .descriptor = RI::ReadWriteView{.buffer = buffer}}});
    RI::Pipeline pipeline = RI::createComputePipeline(device, {.shader = shader});
    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    RI::cmdSetHeap(cmd, heap);
    RI::cmdSetPipeline(cmd, pipeline);
    RI::cmdSetRootData(cmd, Root{1.0f});
    RI::cmdDispatch(cmd, 1, 1, 1);
    RI::cmdBarrier(cmd, RI::Stage::eCompute, RI::Stage::eCopy | RI::Stage::eIndirect);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
    RI::destroyDevice(device);
}

} // namespace

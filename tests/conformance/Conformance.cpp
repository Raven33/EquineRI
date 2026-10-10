// Backend conformance (P83): compiled once per backend, never run. Every
// backend must offer the same portable functions with the same signatures and
// the same portable fields, so this file builds against each of them.

#include <RI/RI.hpp>

#include <initializer_list>
#include <span>
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
EQ_SIGNATURE(allocateMemory, RI::Memory (*)(RI::Device &, RI::MemoryKind, uint64));
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

// Thin values (P81).
static_assert(std::is_copy_constructible_v<RI::Device> && std::is_copy_constructible_v<RI::Buffer> &&
              std::is_copy_constructible_v<RI::Memory> && std::is_copy_constructible_v<RI::Pipeline> &&
              std::is_copy_constructible_v<RI::DescriptorHeap> && std::is_copy_constructible_v<RI::CommandList>);

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

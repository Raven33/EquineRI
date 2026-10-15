// RI: buffer -> compute -> readback, the whole Milestone 2 API in one program
// (the same flow as samples/compute_readback). The reference pages show its
// marked sections. Compiled once per enabled backend; the shader is
// snippets/hlsl/doc_compute.hlsl, compiled to EQUINE_SHADER_DIR.

#include <RI/RI.hpp>
#include <RI/Helpers/TypedCommands.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef EQUINE_SHADER_DIR
#define EQUINE_SHADER_DIR "shaders"
#endif

namespace {

// --8<-- [start:load-shader]
// RI takes bytecode only (P29): SPIR-V on Vulkan, DXIL on D3D12.
std::vector<std::byte> loadShader(const std::string &name) {
#if EQUINE_BACKEND_VULKAN
    const std::string path = std::string(EQUINE_SHADER_DIR) + "/" + name + ".spv";
#else
    const std::string path = std::string(EQUINE_SHADER_DIR) + "/" + name + ".dxil";
#endif
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::vector<std::byte> out(bytes.size());
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}
// --8<-- [end:load-shader]

// --8<-- [start:align-up]
uint64 alignUp(uint64 value, uint64 alignment) { return (value + alignment - 1) / alignment * alignment; }
// --8<-- [end:align-up]

// --8<-- [start:root-struct]
// The C++ mirror of `Root` in the shader: same members, same order.
struct Root {
    uint32 inputSlot;
    uint32 outputSlot;
    uint32 paramsSlot;
    float scale;
    uint32 count;
};
// --8<-- [end:root-struct]

} // namespace

int main() {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    if (gpus.empty()) {
        return 1;
    }
    RI::Device device = RI::createDevice({.gpu = gpus[0]});

    // --8<-- [start:limits]
    const RI::DeviceLimits &limits = device.limits;
    std::printf("heap: up to %u resource and %u sampler slots; root data: up to %u bytes; eGPUUpload: %s\n",
                limits.maxResourceDescriptors, limits.maxSamplerDescriptors, limits.maxRootDataSize,
                limits.gpuUploadMemory ? "yes" : "no");
    // --8<-- [end:limits]

    constexpr uint32 kCount = 4096;
    constexpr uint64 kBytes = kCount * sizeof(float);

    // --8<-- [start:requirements]
    // How much memory, at what alignment, a buffer with this description needs.
    const RI::BufferDesc dataDesc{
        .size = kBytes,
        .usage = RI::BufferUsage::eRead | RI::BufferUsage::eReadWrite | RI::BufferUsage::eCopySrc |
                 RI::BufferUsage::eCopyDst,
    };
    const RI::MemoryRequirements dataReq = RI::bufferRequirements(device, dataDesc);
    // Two such buffers side by side: the second starts at an aligned offset.
    const uint64 outputOffset = alignUp(dataReq.size, dataReq.alignment);
    // --8<-- [end:requirements]
    const uint64 paramsOffset = alignUp(kBytes, dataReq.alignment);

    // --8<-- [start:allocate-memory]
    RI::Memory gpuMemory = RI::allocateMemory(device, RI::MemoryKind::eGPU, outputOffset + dataReq.size);
    RI::Memory uploadMemory = RI::allocateMemory(device, RI::MemoryKind::eUpload, paramsOffset + 256);
    RI::Memory readbackMemory = RI::allocateMemory(device, RI::MemoryKind::eReadback, kBytes);
    // --8<-- [end:allocate-memory]

    // --8<-- [start:create-buffers]
    RI::Buffer input = RI::createBuffer(device, gpuMemory, 0, dataDesc);
    RI::Buffer output = RI::createBuffer(device, gpuMemory, outputOffset, dataDesc);
    RI::Buffer staging = RI::createBuffer(device, uploadMemory, 0, {.size = kBytes, .usage = RI::BufferUsage::eCopySrc});
    RI::Buffer params =
        RI::createBuffer(device, uploadMemory, paramsOffset, {.size = 256, .usage = RI::BufferUsage::eConstant});
    RI::Buffer result = RI::createBuffer(device, readbackMemory, 0, {.size = kBytes, .usage = RI::BufferUsage::eCopyDst});
    // --8<-- [end:create-buffers]

    // --8<-- [start:write-mapped]
    // Upload memory stays mapped: write through buffer.mapped, no map call.
    auto *values = static_cast<float *>(staging.mapped);
    for (uint32 i = 0; i < kCount; ++i) {
        values[i] = static_cast<float>(i);
    }
    const float offset = 0.25f;
    std::memcpy(params.mapped, &offset, sizeof(offset));
    // --8<-- [end:write-mapped]

    // --8<-- [start:create-heap]
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 16});
    // --8<-- [end:create-heap]

    // --8<-- [start:write-descriptors]
    // Slots are yours to choose; the shader receives them in root data.
    RI::writeDescriptors(device, heap,
                         {{.slot = 0, .descriptor = RI::ReadView{.buffer = input, .stride = sizeof(float)}},
                          {.slot = 1, .descriptor = RI::ReadWriteView{.buffer = output, .stride = sizeof(float)}},
                          {.slot = 2, .descriptor = RI::ConstantBufferView{.buffer = params}}});
    // --8<-- [end:write-descriptors]

    // --8<-- [start:write-descriptors-span]
    // A list built at run time: pass a span (one write is a list of one).
    std::vector<RI::DescriptorWrite> writes;
    writes.push_back({.slot = 3, .descriptor = RI::ReadView{.buffer = output, .offset = 1024, .size = 1024, .stride = 4}});
    RI::writeDescriptors(device, heap, writes);
    // --8<-- [end:write-descriptors-span]

    // --8<-- [start:create-pipeline]
    const std::vector<std::byte> bytecode = loadShader("doc_compute");
    RI::Pipeline pipeline = RI::createComputePipeline(device, {.shader = bytecode});
    // pipeline.rootDataSize is 20: the size of Root, read from the shader.
    // --8<-- [end:create-pipeline]

    // --8<-- [start:create-command-list]
    RI::CommandList cmd = RI::createCommandList(device);
    // --8<-- [end:create-command-list]

    // --8<-- [start:record]
    RI::cmdBegin(cmd);
    // --8<-- [start:copy-in]
    RI::cmdCopyBuffer(cmd, staging, 0, input, 0, kBytes);
    // --8<-- [end:copy-in]
    // --8<-- [start:barrier-copy-compute]
    RI::cmdBarrier(cmd, RI::Stage::eCopy, RI::Stage::eCompute); // the copy's writes, then the shader's reads
    // --8<-- [end:barrier-copy-compute]
    // --8<-- [start:set-state]
    RI::cmdSetHeap(cmd, heap);
    RI::cmdSetPipeline(cmd, pipeline);
    RI::Helpers::cmdSetRootData(cmd, Root{.inputSlot = 0, .outputSlot = 1, .paramsSlot = 2, .scale = 3.0f, .count = kCount});
    // --8<-- [end:set-state]
    // --8<-- [start:dispatch]
    RI::cmdDispatch(cmd, (kCount + 63) / 64, 1, 1); // 64 threads per group, see [numthreads]
    // --8<-- [end:dispatch]
    // --8<-- [start:barrier-compute-copy]
    RI::cmdBarrier(cmd, RI::Stage::eCompute, RI::Stage::eCopy); // the shader's writes, then the copy's reads
    // --8<-- [end:barrier-compute-copy]
    RI::cmdCopyBuffer(cmd, output, 0, result, 0, kBytes);
    RI::cmdEnd(cmd);
    // --8<-- [end:record]

    // --8<-- [start:submit-wait]
    RI::Queue &queue = device.graphicsQueue;
    const RI::TimelinePoint done = RI::submit(queue, cmd);
    RI::wait(queue, done); // blocks until the GPU has finished this submission
    // --8<-- [end:submit-wait]

    // --8<-- [start:read-back]
    // Readback memory is mapped too; after the point is reached, reads see the GPU's writes.
    const auto *out = static_cast<const float *>(result.mapped);
    std::printf("out[1] = %g (expected 3.25)\n", out[1]);
    // --8<-- [end:read-back]

    // --8<-- [start:reuse]
    // The list's timeline point is reached, so it may be reset and recorded again.
    RI::resetCommandList(cmd);
    RI::cmdBegin(cmd);
    RI::cmdSetHeap(cmd, heap);
    RI::cmdSetPipeline(cmd, pipeline);
    RI::Helpers::cmdSetRootData(cmd, Root{.inputSlot = 0, .outputSlot = 1, .paramsSlot = 2, .scale = 2.0f, .count = kCount});
    RI::cmdDispatch(cmd, (kCount + 63) / 64, 1, 1);
    RI::cmdEnd(cmd);
    const RI::TimelinePoint again = RI::submit(queue, cmd);
    // --8<-- [end:reuse]

    // --8<-- [start:poll]
    // Do other CPU work while the GPU runs; isReached never blocks.
    uint32 polls = 0;
    while (!RI::isReached(queue, again)) {
        ++polls;
    }
    std::printf("reached after %u polls (point %llu)\n", polls, static_cast<unsigned long long>(again.value));
    // --8<-- [end:poll]

    // --8<-- [start:destroy]
    // Nothing is destroyed for you, and nothing waits: the GPU is done (point reached).
    RI::destroyCommandList(device, cmd);
    RI::destroyPipeline(device, pipeline);
    RI::destroyDescriptorHeap(device, heap);
    for (RI::Buffer *buffer : {&input, &output, &staging, &params, &result}) {
        RI::destroyBuffer(device, *buffer);
    }
    RI::freeMemory(device, gpuMemory); // after the buffers placed in it
    RI::freeMemory(device, uploadMemory);
    RI::freeMemory(device, readbackMemory);
    RI::destroyDevice(device);
    // --8<-- [end:destroy]
    return 0;
}

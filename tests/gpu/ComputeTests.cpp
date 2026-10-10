// Portable GPU tests: the same RI code, compiled once per backend (P83). The
// first case is the Milestone 2 exit: buffer -> compute -> readback.

#include <TestHarness.hpp>

#include <RI/RI.hpp>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

// The shader compiled for this backend: SPIR-V for Vulkan, DXIL for D3D12.
std::vector<std::byte> loadShader(const char *name) {
#if EQUINE_BACKEND_VULKAN
    const std::string path = std::string(EQUINE_SHADER_DIR) + "/" + name + ".spv";
#else
    const std::string path = std::string(EQUINE_SHADER_DIR) + "/" + name + ".dxil";
#endif
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::vector<std::byte> out(bytes.size());
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

// Mirrors FillRoot in tests/shaders/fill.hlsl.
struct FillRoot {
    uint32 inputSlot;
    uint32 outputSlot;
    uint32 paramsSlot;
    float scale;
    uint32 count;
};

RI::Device createTestDevice() { return RI::createDevice({.gpu = RI::listGpus()[0]}); }

} // namespace

// Upload -> copy -> compute -> copy -> readback, with coarse barriers between.
EQ_TEST(compute, buffer_compute_readback) {
    RI::Device device = createTestDevice();
    constexpr uint32 kCount = 1000;
    constexpr uint64 kBytes = kCount * sizeof(float);

    // Memory (P31, P32): placed buffers at offsets the requirements allow.
    const RI::BufferDesc dataDesc{.size = kBytes,
                                  .usage = RI::BufferUsage::eRead | RI::BufferUsage::eReadWrite |
                                           RI::BufferUsage::eCopySrc | RI::BufferUsage::eCopyDst};
    const RI::MemoryRequirements dataReq = RI::bufferRequirements(device, dataDesc);
    const uint64 outputOffset = (dataReq.size + dataReq.alignment - 1) / dataReq.alignment * dataReq.alignment;
    RI::Memory gpuMemory = RI::allocateMemory(device, RI::MemoryKind::eGPU, outputOffset + dataReq.size);
    RI::Memory uploadMemory = RI::allocateMemory(device, RI::MemoryKind::eUpload, 1 << 20);
    RI::Memory readbackMemory = RI::allocateMemory(device, RI::MemoryKind::eReadback, 1 << 20);
    EQ_CHECK(gpuMemory.mapped == nullptr);
    EQ_CHECK(uploadMemory.mapped != nullptr && readbackMemory.mapped != nullptr);

    RI::Buffer input = RI::createBuffer(device, gpuMemory, 0, dataDesc);
    RI::Buffer output = RI::createBuffer(device, gpuMemory, outputOffset, dataDesc);
    RI::Buffer staging = RI::createBuffer(device, uploadMemory, 0, {.size = kBytes, .usage = RI::BufferUsage::eCopySrc});
    const RI::MemoryRequirements paramsReq =
        RI::bufferRequirements(device, {.size = 256, .usage = RI::BufferUsage::eConstant});
    const uint64 paramsOffset = (kBytes + paramsReq.alignment - 1) / paramsReq.alignment * paramsReq.alignment;
    RI::Buffer params =
        RI::createBuffer(device, uploadMemory, paramsOffset, {.size = 256, .usage = RI::BufferUsage::eConstant});
    RI::Buffer result =
        RI::createBuffer(device, readbackMemory, 0, {.size = kBytes, .usage = RI::BufferUsage::eCopyDst});
    EQ_CHECK(input.gpuAddress != 0 && output.gpuAddress != 0 && input.gpuAddress != output.gpuAddress);

    auto *values = static_cast<float *>(staging.mapped);
    for (uint32 i = 0; i < kCount; ++i) {
        values[i] = static_cast<float>(i);
    }
    const float offset = 0.5f;
    std::memcpy(params.mapped, &offset, sizeof(offset));

    // Descriptors (P42, P87): one batched write (D1).
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 64, .samplerCount = 4});
    RI::writeDescriptors(device, heap,
                         {{.slot = 3, .descriptor = RI::ReadView{.buffer = input, .stride = sizeof(float)}},
                          {.slot = 7, .descriptor = RI::ReadWriteView{.buffer = output, .stride = sizeof(float)}},
                          {.slot = 9, .descriptor = RI::ConstantBufferView{.buffer = params}}});

    const std::vector<std::byte> bytecode = loadShader("fill");
    RI::Pipeline fill = RI::createComputePipeline(device, {.shader = bytecode});
    EQ_CHECK(fill.rootDataSize == sizeof(FillRoot));

    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    RI::cmdCopyBuffer(cmd, staging, 0, input, 0, kBytes);
    RI::cmdBarrier(cmd, RI::Stage::eCopy, RI::Stage::eCompute);
    RI::cmdSetHeap(cmd, heap);
    RI::cmdSetPipeline(cmd, fill);
    RI::cmdSetRootData(cmd, FillRoot{.inputSlot = 3, .outputSlot = 7, .paramsSlot = 9, .scale = 2.0f, .count = kCount});
    RI::cmdDispatch(cmd, (kCount + 63) / 64, 1, 1);
    RI::cmdBarrier(cmd, RI::Stage::eCompute, RI::Stage::eCopy);
    RI::cmdCopyBuffer(cmd, output, 0, result, 0, kBytes);
    RI::cmdEnd(cmd);

    const RI::TimelinePoint done = RI::submit(device.graphicsQueue, cmd);
    RI::wait(device.graphicsQueue, done);
    EQ_CHECK(RI::isReached(device.graphicsQueue, done));

    const auto *out = static_cast<const float *>(result.mapped);
    uint32 wrong = 0;
    for (uint32 i = 0; i < kCount; ++i) {
        if (out[i] != static_cast<float>(i) * 2.0f + offset) {
            ++wrong;
        }
    }
    EQ_CHECK(wrong == 0);

    RI::destroyCommandList(device, cmd);
    RI::destroyPipeline(device, fill);
    RI::destroyDescriptorHeap(device, heap);
    for (RI::Buffer *b : {&input, &output, &staging, &params, &result}) {
        RI::destroyBuffer(device, *b);
    }
    RI::freeMemory(device, gpuMemory);
    RI::freeMemory(device, uploadMemory);
    RI::freeMemory(device, readbackMemory);
    RI::destroyDevice(device);
}

// A command list is reused after its timeline point (P56); points grow (P20).
EQ_TEST(compute, command_list_reuse_and_timeline) {
    RI::Device device = createTestDevice();
    RI::CommandList cmd = RI::createCommandList(device);
    RI::TimelinePoint previous{};
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            RI::resetCommandList(cmd);
        }
        RI::cmdBegin(cmd);
        RI::cmdBarrier(cmd, RI::Stage::eAll, RI::Stage::eAll);
        RI::cmdEnd(cmd);
        const RI::TimelinePoint point = RI::submit(device.graphicsQueue, cmd);
        EQ_CHECK(point.value > previous.value);
        RI::wait(device.graphicsQueue, point);
        EQ_CHECK(RI::isReached(device.graphicsQueue, point));
        previous = point;
    }
    EQ_CHECK(RI::isReached(device.graphicsQueue, RI::TimelinePoint{}));
    RI::destroyCommandList(device, cmd);
    RI::destroyDevice(device);
}

// Every available memory kind can be allocated; CPU-visible kinds are mapped (P32, P33).
EQ_TEST(compute, memory_kinds) {
    RI::Device device = createTestDevice();
    EQ_CHECK(device.limits.maxResourceDescriptors >= 100'000);
    EQ_CHECK(device.limits.maxSamplerDescriptors >= 2048);
    EQ_CHECK(device.limits.maxRootDataSize >= 128);
    for (RI::MemoryKind kind : {RI::MemoryKind::eGPU, RI::MemoryKind::eUpload, RI::MemoryKind::eReadback,
                                RI::MemoryKind::eGPUUpload}) {
        if (kind == RI::MemoryKind::eGPUUpload && !device.limits.gpuUploadMemory) {
            continue;
        }
        RI::Memory memory = RI::allocateMemory(device, kind, 1 << 20);
        EQ_CHECK(memory.size == (1 << 20));
        EQ_CHECK((memory.mapped != nullptr) == (kind != RI::MemoryKind::eGPU));
        RI::freeMemory(device, memory);
    }
    RI::destroyDevice(device);
}

#if EQUINE_VALIDATION
// RI's own checks (P88, P91), in validation builds only. Each aborts by design;
// tests/CMakeLists.txt checks the message.
EQ_TEST(compute, root_data_size_mismatch_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    RI::Device device = createTestDevice();
    const std::vector<std::byte> bytecode = loadShader("fill");
    RI::Pipeline fill = RI::createComputePipeline(device, {.shader = bytecode});
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 8});
    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    RI::cmdSetHeap(cmd, heap);
    RI::cmdSetPipeline(cmd, fill);
    const uint32 tooSmall[2] = {1, 2};
    RI::cmdSetRootData(cmd, tooSmall);
}

EQ_TEST(compute, pipeline_before_heap_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    RI::Device device = createTestDevice();
    const std::vector<std::byte> bytecode = loadShader("fill");
    RI::Pipeline fill = RI::createComputePipeline(device, {.shader = bytecode});
    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    RI::cmdSetPipeline(cmd, fill);
}

EQ_TEST(compute, descriptor_slot_out_of_range_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    RI::Device device = createTestDevice();
    RI::Memory memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 1 << 16);
    RI::Buffer buffer = RI::createBuffer(device, memory, 0, {.size = 256, .usage = RI::BufferUsage::eRead});
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 8});
    RI::writeDescriptors(device, heap, {{.slot = 8, .descriptor = RI::ReadView{.buffer = buffer}}});
}

EQ_TEST(compute, buffer_used_after_destroy_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    RI::Device device = createTestDevice();
    RI::Memory memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 1 << 16);
    RI::Buffer buffer = RI::createBuffer(device, memory, 0, {.size = 256, .usage = RI::BufferUsage::eRead});
    const RI::Buffer copy = buffer;
    RI::destroyBuffer(device, buffer);
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 8});
    RI::writeDescriptors(device, heap, {{.slot = 0, .descriptor = RI::ReadView{.buffer = copy}}});
}
#endif

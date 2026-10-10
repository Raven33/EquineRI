// Buffer -> compute -> readback: the Milestone 2 sample (docs/Plan.md).
// The same code runs on Vulkan and on D3D12; the build picks the backend (P83).

#include <RI/RI.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if EQUINE_BACKEND_D3D12
#include <D3D12AgilityExports.hpp>
EQUINE_D3D12_AGILITY_SDK_EXPORTS()
#endif

namespace {

std::vector<std::byte> loadShader(const char *name) {
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

uint64 alignUp(uint64 value, uint64 alignment) { return (value + alignment - 1) / alignment * alignment; }

// Mirrors Root in compute_readback.hlsl.
struct Root {
    uint32 inputSlot;
    uint32 outputSlot;
    uint32 paramsSlot;
    float scale;
    uint32 count;
};

} // namespace

int main() {
    // 1. Pick a GPU and create the device (P60).
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    RI::Device device = RI::createDevice({.gpu = gpus[0]});
    std::printf("GPU: %s\n", device.gpu.name.c_str());

    // 2. Memory blocks, and buffers placed in them (P31, P32).
    constexpr uint32 kCount = 4096;
    constexpr uint64 kBytes = kCount * sizeof(float);
    const RI::BufferDesc dataDesc{.size = kBytes,
                                  .usage = RI::BufferUsage::eRead | RI::BufferUsage::eReadWrite |
                                           RI::BufferUsage::eCopySrc | RI::BufferUsage::eCopyDst};
    const RI::MemoryRequirements dataReq = RI::bufferRequirements(device, dataDesc);
    const uint64 outputOffset = alignUp(dataReq.size, dataReq.alignment);
    const uint64 paramsOffset = alignUp(kBytes, dataReq.alignment);
    RI::Memory gpuMemory = RI::allocateMemory(device, RI::MemoryKind::eGPU, outputOffset + dataReq.size);
    RI::Memory uploadMemory = RI::allocateMemory(device, RI::MemoryKind::eUpload, paramsOffset + 256);
    RI::Memory readbackMemory = RI::allocateMemory(device, RI::MemoryKind::eReadback, kBytes);
    RI::Buffer input = RI::createBuffer(device, gpuMemory, 0, dataDesc);
    RI::Buffer output = RI::createBuffer(device, gpuMemory, outputOffset, dataDesc);
    RI::Buffer staging = RI::createBuffer(device, uploadMemory, 0, {.size = kBytes, .usage = RI::BufferUsage::eCopySrc});
    RI::Buffer params =
        RI::createBuffer(device, uploadMemory, paramsOffset, {.size = 256, .usage = RI::BufferUsage::eConstant});
    RI::Buffer result = RI::createBuffer(device, readbackMemory, 0, {.size = kBytes, .usage = RI::BufferUsage::eCopyDst});

    // 3. Fill the upload memory: CPU-visible kinds stay mapped (P33).
    auto *values = static_cast<float *>(staging.mapped);
    for (uint32 i = 0; i < kCount; ++i) {
        values[i] = static_cast<float>(i);
    }
    const float offset = 0.25f;
    std::memcpy(params.mapped, &offset, sizeof(offset));

    // 4. Descriptors in heap slots the program chooses (P42, P87).
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 16});
    RI::writeDescriptors(device, heap,
                         {{.slot = 0, .descriptor = RI::ReadView{.buffer = input, .stride = sizeof(float)}},
                          {.slot = 1, .descriptor = RI::ReadWriteView{.buffer = output, .stride = sizeof(float)}},
                          {.slot = 2, .descriptor = RI::ConstantBufferView{.buffer = params}}});

    // 5. The pipeline: bytecode in, root data size from the shader (P38, P45).
    const std::vector<std::byte> bytecode = loadShader("compute_readback");
    RI::Pipeline pipeline = RI::createComputePipeline(device, {.shader = bytecode});

    // 6. Record: copy in, compute, copy out, with coarse barriers (P36).
    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    RI::cmdCopyBuffer(cmd, staging, 0, input, 0, kBytes);
    RI::cmdBarrier(cmd, RI::Stage::eCopy, RI::Stage::eCompute);
    RI::cmdSetHeap(cmd, heap);
    RI::cmdSetPipeline(cmd, pipeline);
    RI::cmdSetRootData(cmd, Root{.inputSlot = 0, .outputSlot = 1, .paramsSlot = 2, .scale = 3.0f, .count = kCount});
    RI::cmdDispatch(cmd, (kCount + 63) / 64, 1, 1);
    RI::cmdBarrier(cmd, RI::Stage::eCompute, RI::Stage::eCopy);
    RI::cmdCopyBuffer(cmd, output, 0, result, 0, kBytes);
    RI::cmdEnd(cmd);

    // 7. Submit, wait on the timeline point, read back (P20).
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
    const auto *out = static_cast<const float *>(result.mapped);
    uint32 wrong = 0;
    for (uint32 i = 0; i < kCount; ++i) {
        wrong += out[i] == static_cast<float>(i) * 3.0f + offset ? 0 : 1;
    }
    std::printf("out[0..3] = %g %g %g %g; %u of %u values wrong\n", out[0], out[1], out[2], out[3], wrong, kCount);

    // 8. Explicit, immediate destruction (P20, P81).
    RI::destroyCommandList(device, cmd);
    RI::destroyPipeline(device, pipeline);
    RI::destroyDescriptorHeap(device, heap);
    for (RI::Buffer *b : {&input, &output, &staging, &params, &result}) {
        RI::destroyBuffer(device, *b);
    }
    RI::freeMemory(device, gpuMemory);
    RI::freeMemory(device, uploadMemory);
    RI::freeMemory(device, readbackMemory);
    RI::destroyDevice(device);
    return wrong == 0 ? 0 : 1;
}

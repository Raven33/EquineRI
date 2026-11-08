// Buffer -> compute -> readback: the Milestone 2 sample (docs/Plan.md).
// The same code runs on Vulkan, D3D12 and Metal; the build picks the backend (P83).

#include <RI/RI.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
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
#elif EQUINE_BACKEND_METAL
    const std::string path = std::string(EQUINE_SHADER_DIR) + "/" + name + ".mtl.dxil";
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
    // 1. Pick a GPU and create the device (P60): EQUINE_GPU=<index> or nvidia/amd/intel, else
    //    the first listed (the same rule as the tests).
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    std::string want;
#if defined(_WIN32)
    char *value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, "EQUINE_GPU") == 0 && value != nullptr) {
        want = value;
        std::free(value);
    }
#else
    if (const char *value = std::getenv("EQUINE_GPU"); value != nullptr) {
        want = value;
    }
#endif
    RI::Gpu gpu = gpus[0];
    if (!want.empty()) {
        const size_t index = std::strtoul(want.c_str(), nullptr, 10);
        gpu = gpus[index < gpus.size() ? index : 0];
        for (const RI::Gpu &g : gpus) {
            if ((want == "nvidia" && g.vendor == RI::GpuVendor::eNvidia) ||
                (want == "amd" && g.vendor == RI::GpuVendor::eAMD) ||
                (want == "intel" && g.vendor == RI::GpuVendor::eIntel)) {
                gpu = g;
                break;
            }
        }
    }
    RI::Device device = RI::createDevice({.gpu = gpu});
    std::printf("GPU: %s\n", device.gpu.name.c_str());

    // 2. Memory blocks, and buffers placed in them (P31, P32).
    constexpr uint32 kCount = 4096;
    constexpr uint64 kBytes = kCount * sizeof(float);
    const RI::BufferDesc dataDesc{.size = kBytes,
                                  .usage = RI::BufferUsage::eRead | RI::BufferUsage::eReadWrite |
                                           RI::BufferUsage::eCopySrc | RI::BufferUsage::eCopyDst};
    const RI::BufferDesc stagingDesc{.size = kBytes, .usage = RI::BufferUsage::eCopySrc};
    const RI::BufferDesc paramsDesc{.size = 256, .usage = RI::BufferUsage::eConstant};
    const RI::BufferDesc resultDesc{.size = kBytes, .usage = RI::BufferUsage::eCopyDst};
    // Every placement follows its own buffer's requirements (size and alignment
    // differ per usage and per GPU).
    const RI::MemoryRequirements dataReq = RI::bufferRequirements(device, dataDesc);
    const RI::MemoryRequirements stagingReq = RI::bufferRequirements(device, stagingDesc);
    const RI::MemoryRequirements paramsReq = RI::bufferRequirements(device, paramsDesc);
    const RI::MemoryRequirements resultReq = RI::bufferRequirements(device, resultDesc);
    const uint64 outputOffset = alignUp(dataReq.size, dataReq.alignment);
    const uint64 paramsOffset = alignUp(stagingReq.size, paramsReq.alignment);
    RI::Memory gpuMemory = RI::allocateMemory(device, RI::MemoryKind::eGPU, outputOffset + dataReq.size);
    RI::Memory uploadMemory = RI::allocateMemory(device, RI::MemoryKind::eUpload, paramsOffset + paramsReq.size);
    RI::Memory readbackMemory = RI::allocateMemory(device, RI::MemoryKind::eReadback, resultReq.size);
    RI::Buffer input = RI::createBuffer(device, gpuMemory, 0, dataDesc);
    RI::Buffer output = RI::createBuffer(device, gpuMemory, outputOffset, dataDesc);
    RI::Buffer staging = RI::createBuffer(device, uploadMemory, 0, stagingDesc);
    RI::Buffer params = RI::createBuffer(device, uploadMemory, paramsOffset, paramsDesc);
    RI::Buffer result = RI::createBuffer(device, readbackMemory, 0, resultDesc);

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
    const Root root{.inputSlot = 0, .outputSlot = 1, .paramsSlot = 2, .scale = 3.0f, .count = kCount};
    RI::cmdSetRootData(cmd, std::as_bytes(std::span(&root, 1)));
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

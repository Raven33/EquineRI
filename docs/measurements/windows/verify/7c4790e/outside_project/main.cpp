// Outside project: RI as a subproject, one D3D12 dispatch and a readback.
#include <RI/RI.hpp>

#include <D3D12AgilityExports.hpp>
EQUINE_D3D12_AGILITY_SDK_EXPORTS()

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

struct Root {
    uint32 outputSlot;
    uint32 count;
};

int main() {
    std::ifstream file(std::string(EQUINE_SHADER_DIR) + "/fill.dxil", std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::vector<std::byte> shader(bytes.size());
    std::memcpy(shader.data(), bytes.data(), bytes.size());

    int failures = 0;
    for (const RI::Gpu &gpu : RI::listGpus()) {
        RI::Device device = RI::createDevice({.gpu = gpu});
        constexpr uint32 kCount = 1024;
        const RI::BufferDesc outputDesc{.size = kCount * 4, .usage = RI::BufferUsage::eReadWrite | RI::BufferUsage::eCopySrc};
        const RI::BufferDesc resultDesc{.size = kCount * 4, .usage = RI::BufferUsage::eCopyDst};
        RI::Memory gpuMemory = RI::allocateMemory(device, RI::MemoryKind::eGPU, RI::bufferRequirements(device, outputDesc).size);
        RI::Memory readback = RI::allocateMemory(device, RI::MemoryKind::eReadback, RI::bufferRequirements(device, resultDesc).size);
        RI::Buffer output = RI::createBuffer(device, gpuMemory, 0, outputDesc);
        RI::Buffer result = RI::createBuffer(device, readback, 0, resultDesc);
        RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 4});
        RI::writeDescriptors(device, heap, {{.slot = 0, .descriptor = RI::ReadWriteView{.buffer = output, .stride = 4}}});
        RI::Pipeline pipeline = RI::createComputePipeline(device, {.shader = shader});
        RI::CommandList cmd = RI::createCommandList(device);
        RI::cmdBegin(cmd);
        RI::cmdSetHeap(cmd, heap);
        RI::cmdSetPipeline(cmd, pipeline);
        RI::cmdSetRootData(cmd, Root{.outputSlot = 0, .count = kCount});
        RI::cmdDispatch(cmd, kCount / 64, 1, 1);
        RI::cmdBarrier(cmd, RI::Stage::eCompute, RI::Stage::eCopy);
        RI::cmdCopyBuffer(cmd, output, 0, result, 0, kCount * 4);
        RI::cmdEnd(cmd);
        RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
        const auto *out = static_cast<const uint32 *>(result.mapped);
        uint32 wrong = 0;
        for (uint32 i = 0; i < kCount; ++i) {
            wrong += out[i] == i * 2 + 1 ? 0 : 1;
        }
        std::printf("%s: out[0..3] = %u %u %u %u; %u of %u wrong\n", device.gpu.name.c_str(), out[0], out[1], out[2], out[3],
                    wrong, kCount);
        failures += wrong != 0;
        RI::destroyCommandList(device, cmd);
        RI::destroyPipeline(device, pipeline);
        RI::destroyDescriptorHeap(device, heap);
        RI::destroyBuffer(device, output);
        RI::destroyBuffer(device, result);
        RI::freeMemory(device, gpuMemory);
        RI::freeMemory(device, readback);
        RI::destroyDevice(device);
    }
    return failures;
}

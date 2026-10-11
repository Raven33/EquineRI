// RI: object names and command markers (P53). Compiled once per enabled backend.

#include <RI/RI.hpp>

#include <vector>

int main() {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    if (gpus.empty()) {
        return 1;
    }
    RI::Device device = RI::createDevice({.gpu = gpus[0]});
    RI::Memory memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 1 << 20);
    RI::Buffer particles = RI::createBuffer(device, memory, 0, {.size = 4096, .usage = RI::BufferUsage::eCopyDst});
    RI::CommandList cmd = RI::createCommandList(device);

    // --8<-- [start:names]
    // Names appear in RenderDoc, PIX, Nsight and in validation messages.
    RI::setName(device, memory, "scene memory");
    RI::setName(device, particles, "particles");
    RI::setName(device, cmd, "frame commands");
    // --8<-- [end:names]

    RI::cmdBegin(cmd);
    // --8<-- [start:markers]
    // Regions group commands in a capture; they nest, and each closes in the
    // command list that opened it.
    RI::cmdBeginMarker(cmd, "particles");
    RI::cmdBeginMarker(cmd, "reset");
    RI::cmdClearBuffer(cmd, particles, 0);
    RI::cmdEndMarker(cmd);
    RI::cmdEndMarker(cmd);
    // --8<-- [end:markers]
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));

    RI::destroyCommandList(device, cmd);
    RI::destroyBuffer(device, particles);
    RI::freeMemory(device, memory);
    RI::destroyDevice(device);
    return 0;
}

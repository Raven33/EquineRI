// RI: object names and command markers (P53, P106). Compiled once per enabled backend.

#include <RI/RI.hpp>

#include <vector>

int main() {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    if (gpus.empty()) {
        return 1;
    }
    RI::Device device = RI::createDevice({.gpu = gpus[0]});
    // --8<-- [start:names]
    // Names appear in RenderDoc, PIX, Nsight and in validation messages. Give
    // them at creation: in the desc, or as the last argument where there is none.
    RI::Memory memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 1 << 20, nullptr, "scene memory");
    RI::Buffer particles = RI::createBuffer(
        device, memory, 0, {.size = 4096, .usage = RI::BufferUsage::eCopyDst, .name = "particles"});
    RI::CommandList cmd = RI::createCommandList(device, "frame commands");
    // setName renames, for example a command list reused for another pass.
    RI::setName(device, cmd, "frame 1 commands");
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

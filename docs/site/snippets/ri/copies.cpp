// RI: uploading texels with a copy, reading them back, and occlusion and
// pipeline-statistics queries (P47, P71). Compiled once per enabled backend.

#include <RI/Helpers/TypedCommands.hpp>
#include <RI/RI.hpp>

#include <cstring>
#include <vector>

// --8<-- [start:small-write]
// Resets a 64-bit counter at the start of the frame: a few bytes, so they go
// into the command list itself; no upload buffer, no shader.
void resetCounter(RI::CommandList &cmd, const RI::Buffer &counters, uint64 start) {
    RI::Helpers::cmdWriteSmallData(cmd, counters, 0, start); // checked at compile time: 8 bytes
    // The same bytes through core, for data built at run time:
    // RI::cmdWriteSmallData(cmd, counters, 0, std::as_bytes(std::span(&start, 1)));
    RI::cmdBarrier(cmd, RI::Stage::eCopy, RI::Stage::eCompute); // a small write counts as a copy
}
// --8<-- [end:small-write]

int main() {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    if (gpus.empty()) {
        return 1;
    }
    RI::Device device = RI::createDevice({.gpu = gpus[0]});
    RI::Memory gpuMemory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 16 << 20);
    RI::Memory uploadMemory = RI::allocateMemory(device, RI::MemoryKind::eUpload, 4 << 20);
    RI::Memory readbackMemory = RI::allocateMemory(device, RI::MemoryKind::eReadback, 4 << 20);
    RI::Texture texture = RI::createTexture(device, gpuMemory, 0,
                                            {.width = 256,
                                             .height = 256,
                                             .format = RI::Format::eRGBA8Unorm,
                                             .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eCopySrc |
                                                      RI::TextureUsage::eCopyDst});
    const std::vector<uint32> image(256 * 256, 0xff00ff00u); // your decoded image, 4 bytes per texel

    // --8<-- [start:upload]
    // Ask where each row goes, then write the rows at that pitch.
    const RI::CopyLayout layout = RI::copyLayout(device, texture.view);
    RI::Buffer staging = RI::createBuffer(device, uploadMemory, 0,
                                          {.size = layout.size, .usage = RI::BufferUsage::eCopySrc});
    for (uint32 y = 0; y < layout.height; ++y) {
        std::memcpy(static_cast<std::byte *>(staging.mapped) + y * layout.rowPitch, &image[y * 256],
                    uint64{layout.width} * layout.bytesPerTexel);
    }
    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    RI::cmdCopyBufferToTexture(cmd, staging, 0, texture.view);
    // Copies count as Stage::eCopy: order them before the shaders that read.
    RI::cmdBarrier(cmd, RI::Stage::eCopy, RI::Stage::eCompute | RI::Stage::ePixel);
    // --8<-- [end:upload]

    // --8<-- [start:readback]
    RI::Buffer readback = RI::createBuffer(device, readbackMemory, 0,
                                           {.size = layout.size, .usage = RI::BufferUsage::eCopyDst});
    RI::cmdBarrier(cmd, RI::Stage::eAll, RI::Stage::eCopy);
    RI::cmdCopyTextureToBuffer(cmd, texture.view, readback, 0);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
    // Row y is at readback.mapped + y * layout.rowPitch.
    // --8<-- [end:readback]

    RI::resetCommandList(cmd);
    // --8<-- [start:queries]
    RI::QueryPool statistics = RI::createQueryPool(device, {.type = RI::QueryType::ePipelineStatistics, .count = 1});
    const RI::PipelineStatisticsLayout stats = RI::pipelineStatisticsLayout(device);
    RI::Buffer results = RI::createBuffer(device, readbackMemory, 1 << 20,
                                          {.size = stats.size, .usage = RI::BufferUsage::eCopyDst});
    RI::cmdBegin(cmd);
    RI::cmdBeginQuery(cmd, statistics, 0);
    // ... draws and dispatches to measure ...
    RI::cmdEndQuery(cmd, statistics, 0);
    RI::cmdResolveQueries(cmd, statistics, 0, 1, results, 0);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
    uint64 computeInvocations = 0;
    std::memcpy(&computeInvocations, static_cast<std::byte *>(results.mapped) + stats.computeShaderInvocations, 8);
    // --8<-- [end:queries]

    RI::destroyBuffer(device, results);
    RI::destroyQueryPool(device, statistics);
    RI::destroyCommandList(device, cmd);
    RI::destroyBuffer(device, readback);
    RI::destroyBuffer(device, staging);
    RI::destroyTexture(device, texture);
    RI::freeMemory(device, readbackMemory);
    RI::freeMemory(device, uploadMemory);
    RI::freeMemory(device, gpuMemory);
    RI::destroyDevice(device);
    return 0;
}

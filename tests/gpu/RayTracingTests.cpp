// Inline ray tracing (P49): build a bottom and a top level, trace from a
// compute shader with RayQuery, refit the bottom level, trace again.

#include "GpuTestCommon.hpp"

#include <TestHarness.hpp>

#include <RI/RI.hpp>
#include <RI/Helpers/TypedCommands.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

using gputest::Arena;
using gputest::createTestDevice;
using gputest::placeBuffer;

struct TraceRoot {
    uint32 tlasSlot;
    uint32 outputSlot;
    uint32 width;
    uint32 height;
};

constexpr uint32 kSize = 16;

void setTriangle(void *mapped, float z) {
    const float vertices[] = {-0.5f, -0.5f, z, 0.5f, -0.5f, z, 0.0f, 0.5f, z};
    std::memcpy(mapped, vertices, sizeof(vertices));
}

RI::AccelerationStructure placeStructure(RI::Device &device, Arena &arena, uint64 size, bool topLevel) {
    const RI::MemoryRequirements req = RI::accelerationStructureRequirements(device, size);
    const uint64 offset = gputest::alignUp(arena.used, req.alignment);
    arena.used = offset + req.size;
    return RI::createAccelerationStructure(device, arena.memory, offset, size, topLevel);
}

} // namespace

EQ_TEST(raytracing, build_trace_update) {
    RI::Device device = createTestDevice();
    if (!device.limits.rayTracing) {
        RI::destroyDevice(device);
        return;
    }
    Arena gpu{.memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 16 << 20)};
    Arena upload{.memory = RI::allocateMemory(device, RI::MemoryKind::eUpload, 1 << 20)};
    Arena readback{.memory = RI::allocateMemory(device, RI::MemoryKind::eReadback, 1 << 20)};
    RI::Buffer vertices = placeBuffer(device, upload, {.size = 36, .usage = RI::BufferUsage::eAccelerationStructureInput});
    setTriangle(vertices.mapped, 0.0f);
    const RI::BlasDesc blasDesc{.triangles = {{.vertexAddress = vertices.gpuAddress, .vertexCount = 3}},
                                .flags = RI::AccelerationStructureFlags::eAllowUpdate};
    const RI::AccelerationStructureSizes blasSize = RI::blasSizes(device, blasDesc);
    EQ_CHECK(blasSize.size > 0 && blasSize.buildScratchSize > 0);
    RI::AccelerationStructure blas = placeStructure(device, gpu, blasSize.size, false);

    RI::Buffer instances = placeBuffer(device, upload, {.size = sizeof(RI::AccelerationStructureInstance),
                                                         .usage = RI::BufferUsage::eAccelerationStructureInput});
    RI::AccelerationStructureInstance instance{};
    instance.transform[0][0] = instance.transform[1][1] = instance.transform[2][2] = 1.0f;
    instance.mask = 0xff;
    instance.blasAddress = blas.gpuAddress;
    std::memcpy(instances.mapped, &instance, sizeof(instance));
    const RI::TlasDesc tlasDesc{.instancesAddress = instances.gpuAddress, .instanceCount = 1};
    const RI::AccelerationStructureSizes tlasSize = RI::tlasSizes(device, tlasDesc);
    RI::AccelerationStructure tlas = placeStructure(device, gpu, tlasSize.size, true);

    const uint64 scratchSize = std::max<uint64>({blasSize.buildScratchSize, blasSize.updateScratchSize, tlasSize.buildScratchSize});
    // Scratch addresses are aligned to the device's scratch alignment.
    const RI::BufferDesc scratchDesc{.size = scratchSize, .usage = RI::BufferUsage::eReadWrite};
    const uint64 scratchOffset = gputest::alignUp(
        gpu.used, std::max<uint64>(RI::bufferRequirements(device, scratchDesc).alignment,
                                   device.limits.accelerationStructureScratchAlignment));
    RI::Buffer scratch = RI::createBuffer(device, gpu.memory, scratchOffset, scratchDesc);
    gpu.used = scratchOffset + RI::bufferRequirements(device, scratchDesc).size;
    RI::Buffer output = placeBuffer(device, gpu, {.size = kSize * kSize * 4,
                                                  .usage = RI::BufferUsage::eReadWrite | RI::BufferUsage::eCopySrc});
    RI::Buffer result = placeBuffer(device, readback, {.size = kSize * kSize * 4, .usage = RI::BufferUsage::eCopyDst});
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 4});
    RI::writeDescriptors(device, heap, {{.slot = 0, .descriptor = RI::AccelerationStructureView{.tlas = tlas}},
                                        {.slot = 1, .descriptor = RI::ReadWriteView{.buffer = output, .stride = 4}}});
    const std::vector<std::byte> shader = gputest::loadShader("ray_query");
    RI::Pipeline trace = RI::createComputePipeline(device, {.shader = shader});
    RI::CommandList cmd = RI::createCommandList(device);

    const auto run = [&](bool update) {
        RI::cmdBegin(cmd);
        RI::cmdBuildBlas(cmd, blas, blasDesc, scratch, 0, update);
        RI::cmdBarrier(cmd, RI::Stage::eAccelerationStructure, RI::Stage::eAccelerationStructure);
        RI::cmdBuildTlas(cmd, tlas, tlasDesc, scratch, 0);
        RI::cmdBarrier(cmd, RI::Stage::eAccelerationStructure, RI::Stage::eCompute);
        RI::cmdSetHeap(cmd, heap);
        RI::cmdSetPipeline(cmd, trace);
        RI::Helpers::cmdSetRootData(cmd, TraceRoot{.tlasSlot = 0, .outputSlot = 1, .width = kSize, .height = kSize});
        RI::cmdDispatch(cmd, kSize / 8, kSize / 8, 1);
        RI::cmdBarrier(cmd, RI::Stage::eCompute, RI::Stage::eCopy);
        RI::cmdCopyBuffer(cmd, output, 0, result, 0, output.size);
        RI::cmdEnd(cmd);
        RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
        RI::resetCommandList(cmd);
        return static_cast<const float *>(result.mapped);
    };
    // Rays start at z = -1: the triangle at z = 0 is hit at t = 1.
    const float *t = run(false);
    const auto at = [&](uint32 x, uint32 y) { return t[y * kSize + x]; };
    EQ_CHECK(std::fabs(at(kSize / 2, kSize / 2) - 1.0f) < 1e-3f);
    EQ_CHECK(at(0, 0) < 0.0f && at(kSize - 1, kSize - 1) < 0.0f);
    // Refit after moving the triangle to z = 0.5: t = 1.5.
    setTriangle(vertices.mapped, 0.5f);
    t = run(true);
    EQ_CHECK(std::fabs(at(kSize / 2, kSize / 2) - 1.5f) < 1e-3f);

    RI::destroyCommandList(device, cmd);
    RI::destroyPipeline(device, trace);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroyBuffer(device, result);
    RI::destroyBuffer(device, output);
    RI::destroyBuffer(device, scratch);
    RI::destroyAccelerationStructure(device, tlas);
    RI::destroyBuffer(device, instances);
    RI::destroyAccelerationStructure(device, blas);
    RI::destroyBuffer(device, vertices);
    RI::freeMemory(device, readback.memory);
    RI::freeMemory(device, upload.memory);
    RI::freeMemory(device, gpu.memory);
    RI::destroyDevice(device);
}

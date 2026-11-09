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

// A top level of 0 instances builds, and every ray misses (D3D12 and Vulkan
// allow it; Metal's debug layer refuses an empty instance range, so the Metal
// backend keeps room for one record).
EQ_TEST(raytracing, empty_top_level) {
    RI::Device device = createTestDevice();
    if (!device.limits.rayTracing) {
        RI::destroyDevice(device);
        return;
    }
    Arena gpu{.memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 4 << 20)};
    Arena upload{.memory = RI::allocateMemory(device, RI::MemoryKind::eUpload, 1 << 16)};
    Arena readback{.memory = RI::allocateMemory(device, RI::MemoryKind::eReadback, 1 << 16)};
    // A valid, unread instance address: some drivers check it even for 0 instances.
    RI::Buffer instances = placeBuffer(device, upload, {.size = sizeof(RI::AccelerationStructureInstance),
                                                         .usage = RI::BufferUsage::eAccelerationStructureInput});
    std::memset(instances.mapped, 0, sizeof(RI::AccelerationStructureInstance));
    const RI::TlasDesc tlasDesc{.instancesAddress = instances.gpuAddress, .instanceCount = 0};
    const RI::AccelerationStructureSizes tlasSize = RI::tlasSizes(device, tlasDesc);
    RI::AccelerationStructure tlas = placeStructure(device, gpu, tlasSize.size, true);
    const RI::BufferDesc scratchDesc{.size = std::max<uint64>(tlasSize.buildScratchSize, 1),
                                     .usage = RI::BufferUsage::eReadWrite};
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
    RI::cmdBegin(cmd);
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
    const auto *t = static_cast<const float *>(result.mapped);
    EQ_CHECK(std::all_of(t, t + kSize * kSize, [](float v) { return v == -1.0f; }));

    RI::destroyCommandList(device, cmd);
    RI::destroyPipeline(device, trace);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroyBuffer(device, result);
    RI::destroyBuffer(device, output);
    RI::destroyBuffer(device, scratch);
    RI::destroyAccelerationStructure(device, tlas);
    RI::destroyBuffer(device, instances);
    RI::freeMemory(device, readback.memory);
    RI::freeMemory(device, upload.memory);
    RI::freeMemory(device, gpu.memory);
    RI::destroyDevice(device);
}

// A scene of two geometries and two instances (P49): geometry 0 a triangle at
// z = 0 in the lower-left quadrant, wound counter-clockwise as the rays see it;
// geometry 1 an indexed quad (16-bit indices, not opaque) at z = 0.25 in the
// lower-right quadrant. Instance 0 (ID 7, mask 1) as is; instance 1 (ID 9,
// mask 2, front faces counter-clockwise) moved up by 1. Checked: hit
// distances, instance IDs and indices, geometry and primitive indices, front
// faces, the ray mask, a fast-build bottom level, and a top-level refit.
EQ_TEST(raytracing, scene) {
    RI::Device device = createTestDevice();
    if (!device.limits.rayTracing) {
        RI::destroyDevice(device);
        return;
    }
    Arena gpu{.memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 16 << 20)};
    Arena upload{.memory = RI::allocateMemory(device, RI::MemoryKind::eUpload, 1 << 20)};
    Arena readback{.memory = RI::allocateMemory(device, RI::MemoryKind::eReadback, 1 << 20)};
    const float triangle[] = {-1, -1, 0, 0, -1, 0, -1, 0, 0};
    const float quad[] = {0.1f, -0.9f, 0.25f, 0.9f, -0.9f, 0.25f, 0.9f, -0.1f, 0.25f, 0.1f, -0.1f, 0.25f};
    const uint16 quadIndices[] = {0, 1, 2, 0, 2, 3};
    const auto input = RI::BufferUsage::eAccelerationStructureInput;
    RI::Buffer v0 = placeBuffer(device, upload, {.size = sizeof(triangle), .usage = input});
    RI::Buffer v1 = placeBuffer(device, upload, {.size = sizeof(quad), .usage = input});
    RI::Buffer indices = placeBuffer(device, upload, {.size = 16, .usage = input});
    std::memcpy(v0.mapped, triangle, sizeof(triangle));
    std::memcpy(v1.mapped, quad, sizeof(quad));
    std::memcpy(indices.mapped, quadIndices, sizeof(quadIndices));
    const RI::BlasDesc blasDesc{
        .triangles = {{.vertexAddress = v0.gpuAddress, .vertexCount = 3},
                      {.vertexAddress = v1.gpuAddress, .vertexCount = 4, .indexAddress = indices.gpuAddress,
                       .indexFormat = RI::IndexFormat::eUint16, .indexCount = 6, .opaque = false}},
        .flags = RI::AccelerationStructureFlags::ePreferFastBuild};
    const RI::AccelerationStructureSizes blasSize = RI::blasSizes(device, blasDesc);
    RI::AccelerationStructure blas = placeStructure(device, gpu, blasSize.size, false);

    RI::Buffer instances = placeBuffer(device, upload, {.size = 2 * sizeof(RI::AccelerationStructureInstance), .usage = input});
    auto *instance = static_cast<RI::AccelerationStructureInstance *>(instances.mapped);
    std::memset(instance, 0, 2 * sizeof(RI::AccelerationStructureInstance));
    for (int i = 0; i < 2; ++i) {
        instance[i].transform[0][0] = instance[i].transform[1][1] = instance[i].transform[2][2] = 1.0f;
        instance[i].blasAddress = blas.gpuAddress;
    }
    instance[0].instanceId = 7;
    instance[0].mask = 0x01;
    instance[1].instanceId = 9;
    instance[1].mask = 0x02;
    instance[1].transform[1][3] = 1.0f;
    instance[1].flags = 0x2; // D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE
    const RI::TlasDesc tlasDesc{.instancesAddress = instances.gpuAddress, .instanceCount = 2,
                                .flags = RI::AccelerationStructureFlags::eAllowUpdate};
    const RI::AccelerationStructureSizes tlasSize = RI::tlasSizes(device, tlasDesc);
    RI::AccelerationStructure tlas = placeStructure(device, gpu, tlasSize.size, true);

    const RI::BufferDesc scratchDesc{
        .size = std::max<uint64>({blasSize.buildScratchSize, tlasSize.buildScratchSize, tlasSize.updateScratchSize}),
        .usage = RI::BufferUsage::eReadWrite};
    const uint64 scratchOffset = gputest::alignUp(
        gpu.used, std::max<uint64>(RI::bufferRequirements(device, scratchDesc).alignment,
                                   device.limits.accelerationStructureScratchAlignment));
    RI::Buffer scratch = RI::createBuffer(device, gpu.memory, scratchOffset, scratchDesc);
    gpu.used = scratchOffset + RI::bufferRequirements(device, scratchDesc).size;
    RI::Buffer output = placeBuffer(device, gpu, {.size = kSize * kSize * 16,
                                                  .usage = RI::BufferUsage::eReadWrite | RI::BufferUsage::eCopySrc});
    RI::Buffer result = placeBuffer(device, readback, {.size = kSize * kSize * 16, .usage = RI::BufferUsage::eCopyDst});
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 4});
    RI::writeDescriptors(device, heap, {{.slot = 0, .descriptor = RI::AccelerationStructureView{.tlas = tlas}},
                                        {.slot = 1, .descriptor = RI::ReadWriteView{.buffer = output, .stride = 16}}});
    const std::vector<std::byte> shader = gputest::loadShader("ray_query_scene");
    RI::Pipeline trace = RI::createComputePipeline(device, {.shader = shader});
    RI::CommandList cmd = RI::createCommandList(device);
    struct SceneRoot {
        uint32 tlasSlot, outputSlot, width, height, mask;
    };
    enum class Build { eNone, eAll, eRefit };
    const auto run = [&](Build build, uint32 mask) {
        RI::cmdBegin(cmd);
        if (build == Build::eAll) {
            RI::cmdBuildBlas(cmd, blas, blasDesc, scratch, 0);
            RI::cmdBarrier(cmd, RI::Stage::eAccelerationStructure, RI::Stage::eAccelerationStructure);
        }
        if (build != Build::eNone) {
            RI::cmdBuildTlas(cmd, tlas, tlasDesc, scratch, 0, build == Build::eRefit);
            RI::cmdBarrier(cmd, RI::Stage::eAccelerationStructure, RI::Stage::eCompute);
        }
        RI::cmdSetHeap(cmd, heap);
        RI::cmdSetPipeline(cmd, trace);
        RI::Helpers::cmdSetRootData(cmd, SceneRoot{.tlasSlot = 0, .outputSlot = 1, .width = kSize, .height = kSize, .mask = mask});
        RI::cmdDispatch(cmd, kSize / 8, kSize / 8, 1);
        RI::cmdBarrier(cmd, RI::Stage::eCompute, RI::Stage::eCopy);
        RI::cmdCopyBuffer(cmd, output, 0, result, 0, output.size);
        RI::cmdEnd(cmd);
        RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
        RI::resetCommandList(cmd);
    };
    struct Hit {
        float t;
        uint32 id, index, geometry, primitive;
        bool front;
    };
    constexpr uint32 kMiss = ~0u;
    // Pixel (x, y), row 0 at the top: a hit, or t < 0 for a miss.
    const auto at = [&](uint32 x, uint32 y) {
        const uint32 *p = static_cast<const uint32 *>(result.mapped) + 4 * (y * kSize + x);
        if (p[0] == kMiss) {
            return Hit{-1.0f, 0, 0, 0, 0, false};
        }
        Hit h{};
        std::memcpy(&h.t, p, 4);
        h.id = p[1];
        h.index = p[2] & 0xffff;
        h.geometry = p[2] >> 16;
        h.primitive = p[3] & 0x7fffffff;
        h.front = (p[3] >> 31) != 0;
        return h;
    };
    const auto is = [](const Hit &h, float t, uint32 id, uint32 index, uint32 geometry, uint32 primitive, bool front) {
        return std::fabs(h.t - t) < 1e-3f && h.id == id && h.index == index && h.geometry == geometry &&
               h.primitive == primitive && h.front == front;
    };
    // (2, 13) lies on the triangle, (12, 12) on the quad's first triangle; four
    // rows up, (2, 5) and (12, 4) the same points of instance 1; (9, 4) the
    // quad's second triangle of instance 1; (8, 8) nothing.
    run(Build::eAll, 0xff);
    EQ_CHECK(is(at(2, 13), 1.0f, 7, 0, 0, 0, false));
    EQ_CHECK(is(at(12, 12), 1.25f, 7, 0, 1, 0, false));
    EQ_CHECK(is(at(2, 5), 1.0f, 9, 1, 0, 0, true));
    EQ_CHECK(is(at(12, 4), 1.25f, 9, 1, 1, 0, true));
    EQ_CHECK(is(at(9, 4), 1.25f, 9, 1, 1, 1, true));
    EQ_CHECK(at(8, 8).t < 0.0f);
    // The ray mask: instance 1 (mask 2) is skipped.
    run(Build::eNone, 0x01);
    EQ_CHECK(is(at(2, 13), 1.0f, 7, 0, 0, 0, false) && is(at(12, 12), 1.25f, 7, 0, 1, 0, false));
    EQ_CHECK(at(2, 5).t < 0.0f && at(12, 4).t < 0.0f);
    // A refit moving instance 1 right by 0.5: (2, 5), (12, 4) and (9, 4) now
    // miss; (14, 4) hits the quad's second triangle.
    instance[1].transform[0][3] = 0.5f;
    run(Build::eRefit, 0xff);
    EQ_CHECK(at(2, 5).t < 0.0f && at(12, 4).t < 0.0f && at(9, 4).t < 0.0f);
    EQ_CHECK(is(at(14, 4), 1.25f, 9, 1, 1, 1, true));
    EQ_CHECK(is(at(2, 13), 1.0f, 7, 0, 0, 0, false));

    RI::destroyCommandList(device, cmd);
    RI::destroyPipeline(device, trace);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroyBuffer(device, result);
    RI::destroyBuffer(device, output);
    RI::destroyBuffer(device, scratch);
    RI::destroyAccelerationStructure(device, tlas);
    RI::destroyBuffer(device, instances);
    RI::destroyAccelerationStructure(device, blas);
    RI::destroyBuffer(device, indices);
    RI::destroyBuffer(device, v1);
    RI::destroyBuffer(device, v0);
    RI::freeMemory(device, readback.memory);
    RI::freeMemory(device, upload.memory);
    RI::freeMemory(device, gpu.memory);
    RI::destroyDevice(device);
}

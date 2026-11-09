// Metal-only GPU tests: native access (P84) and command memory (P115).
// Portable tests live in tests/gpu/.

#include <TestHarness.hpp>

#include "../gpu/GpuTestCommon.hpp"

#include <RI/RI.hpp>
#include <RI/Metal/Native.hpp>

#include <vector>

using gputest::Arena;
using gputest::createTestDevice;
using gputest::placeBuffer;
using gputest::placeTexture;

// Every accessor returns the object RI uses (P84).
EQ_TEST(metal, native_access) {
    RI::Device device = createTestDevice();
    const RI::Metal::NativeDevice n = RI::Metal::native(device);
    EQ_CHECK(n.device != nullptr && n.graphicsQueue != nullptr && n.residencySet != nullptr && n.timeline != nullptr);
    EQ_CHECK(n.graphicsQueue->device() == n.device);

    Arena gpu{.memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 1 << 20)};
    Arena upload{.memory = RI::allocateMemory(device, RI::MemoryKind::eUpload, 1 << 16)};
    EQ_CHECK(RI::Metal::native(gpu.memory) != nullptr && RI::Metal::native(gpu.memory)->size() >= (1 << 20));
    RI::Buffer buffer = placeBuffer(device, upload, {.size = 256, .usage = RI::BufferUsage::eRead});
    EQ_CHECK(RI::Metal::native(buffer)->gpuAddress() == buffer.gpuAddress);
    EQ_CHECK(RI::Metal::native(buffer)->contents() == buffer.mapped);
    RI::Texture texture = placeTexture(device, gpu, {.width = 8, .height = 4, .format = RI::Format::eRGBA8Unorm,
                                                     .usage = RI::TextureUsage::eSampled});
    EQ_CHECK(RI::Metal::native(texture)->width() == 8 && RI::Metal::native(texture)->height() == 4);
    EQ_CHECK(RI::Metal::native(texture.view)->pixelFormat() == MTL::PixelFormatRGBA8Unorm);

    RI::QueryPool timestamps = RI::createQueryPool(device, {.type = RI::QueryType::eTimestamp, .count = 2});
    RI::QueryPool occlusion = RI::createQueryPool(device, {.type = RI::QueryType::eOcclusion, .count = 3});
    EQ_CHECK(RI::Metal::native(timestamps).counters != nullptr && RI::Metal::native(timestamps).results == nullptr);
    EQ_CHECK(RI::Metal::native(timestamps).counters->count() >= 2);
    EQ_CHECK(RI::Metal::native(occlusion).counters == nullptr && RI::Metal::native(occlusion).results != nullptr);
    EQ_CHECK(RI::Metal::native(occlusion).results->length() >= 3 * 8);

    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 4, .samplerCount = 2});
    const RI::Metal::NativeDescriptorHeap h = RI::Metal::native(heap);
    EQ_CHECK(h.resources != nullptr && h.resources->length() >= 4 * 24);
    EQ_CHECK(h.samplers != nullptr && h.samplers->length() >= 2 * 24);

    const std::vector<std::byte> bytecode = gputest::loadShader("fill");
    RI::Pipeline pipeline = RI::createComputePipeline(device, {.shader = bytecode});
    const RI::Metal::NativePipeline p = RI::Metal::native(pipeline);
    EQ_CHECK(p.compute != nullptr && p.render == nullptr && p.depthStencil == nullptr);

    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    EQ_CHECK(RI::Metal::native(cmd) != nullptr);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));

    RI::destroyCommandList(device, cmd);
    RI::destroyPipeline(device, pipeline);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroyQueryPool(device, occlusion);
    RI::destroyQueryPool(device, timestamps);
    RI::destroyTexture(device, texture);
    RI::destroyBuffer(device, buffer);
    RI::freeMemory(device, upload.memory);
    RI::freeMemory(device, gpu.memory);
    RI::destroyDevice(device);
}

// While an occlusion pool exists, each section takes a 32 KiB visibility page
// of command memory (P115): 64 sections in one list take 2 MiB of pages, and
// recording them again after a reset takes nothing more. Pages once shared the
// list's growing chunks, which then doubled about every 8 sections (63 MiB for
// 64 sections).
EQ_TEST(metal, visibility_pages_bounded) {
    RI::Device device = createTestDevice();
    const MTL::ResidencySet *residency = RI::Metal::native(device).residencySet;
    RI::QueryPool occlusion = RI::createQueryPool(device, {.type = RI::QueryType::eOcclusion, .count = 1});
    RI::CommandList cmd = RI::createCommandList(device);
    const auto record = [&] {
        RI::cmdBegin(cmd);
        for (int i = 0; i < 64; ++i) {
            RI::cmdBeginRendering(cmd, RI::RenderingSize{.width = 8, .height = 8});
            RI::cmdEndRendering(cmd);
        }
        RI::cmdEnd(cmd);
        RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
        RI::resetCommandList(cmd);
        return residency->allocatedSize();
    };
    const uint64 before = residency->allocatedSize();
    const uint64 first = record();
    const uint64 second = record();
    EQ_CHECK(first - before <= 3ull << 20); // 2 MiB of pages and the first chunk
    EQ_CHECK(second == first);

    RI::destroyCommandList(device, cmd);
    RI::destroyQueryPool(device, occlusion);
    RI::destroyDevice(device);
}

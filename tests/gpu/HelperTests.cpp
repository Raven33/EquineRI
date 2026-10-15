// Helpers (P41): each one exercised through RI's public functions only.

#include "GpuTestCommon.hpp"

#include <TestHarness.hpp>

#include <RI/Helpers/FrameRing.hpp>
#include <RI/Helpers/Grid.hpp>
#include <RI/Helpers/MemoryAllocator.hpp>
#include <RI/Helpers/Profiler.hpp>
#include <RI/Helpers/ShaderCompiler.hpp>
#include <RI/Helpers/SlotAllocator.hpp>
#include <RI/Helpers/Transfers.hpp>
#include <RI/Helpers/TypedCommands.hpp>
#include <RI/RI.hpp>

#include <array>
#include <cstring>
#include <vector>

using gputest::createTestDevice;
using gputest::loadShader;

// The typed commands' constraints (P107), at compile time.
namespace {
struct Bytes12 {
    uint32 a, b, c;
};
struct Bytes6 {
    uint16 a, b, c;
};
} // namespace
static_assert(RI::Helpers::SmallWriteValue<uint32> && RI::Helpers::SmallWriteValue<std::array<uint32, 16>>);
static_assert(!RI::Helpers::SmallWriteValue<std::array<uint32, 17>>);
static_assert(!RI::Helpers::SmallWriteValue<Bytes6> && !RI::Helpers::SmallWriteValue<uint32 *>);
static_assert(RI::Helpers::RootDataValue<Bytes12> && RI::Helpers::RootDataValue<std::array<uint32, 64>>);
static_assert(!RI::Helpers::RootDataValue<std::array<uint32, 65>>);
static_assert(!RI::Helpers::RootDataValue<std::span<const std::byte>> && !RI::Helpers::RootDataValue<std::span<const uint32>>);

// Allocation, deferred release and reuse; slots reused after their point.
EQ_TEST(helpers, allocators) {
    RI::Device device = createTestDevice();
    RI::Helpers::MemoryAllocator allocator = RI::Helpers::createMemoryAllocator(device, 4 << 20);
    RI::Helpers::Allocation a, b, c;
    RI::Buffer bufferA = RI::Helpers::createBuffer(allocator, RI::MemoryKind::eGPU,
                                                   {.size = 1 << 20, .usage = RI::BufferUsage::eReadWrite}, a);
    RI::Buffer bufferB = RI::Helpers::createBuffer(allocator, RI::MemoryKind::eGPU,
                                                   {.size = 1 << 20, .usage = RI::BufferUsage::eReadWrite}, b);
    EQ_CHECK(a.block == b.block && a.offset + a.size <= b.offset && !a.needsDiscard);
    RI::destroyBuffer(device, bufferA);
    RI::Helpers::release(allocator, a); // no point: free at once
    const RI::TextureDesc desc{.width = 64, .height = 64, .format = RI::Format::eRGBA8Unorm,
                               .usage = RI::TextureUsage::eRenderTarget};
    RI::Texture texture = RI::Helpers::createTexture(allocator, desc, c);
    EQ_CHECK(c.offset < b.offset && c.needsDiscard); // reused A's range: a discard is due (P69)
    RI::destroyTexture(device, texture);
    RI::Helpers::release(allocator, c);
    RI::destroyBuffer(device, bufferB);
    RI::Helpers::release(allocator, b);
    RI::Helpers::destroyMemoryAllocator(allocator);

    RI::Helpers::SlotAllocator slots = RI::Helpers::createSlotAllocator(device.graphicsQueue, 4, 2);
    const uint32 first = RI::Helpers::allocateSlot(slots);
    const uint32 second = RI::Helpers::allocateSlot(slots);
    EQ_CHECK(first == 4 && second == 5);
    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    RI::cmdEnd(cmd);
    const RI::TimelinePoint point = RI::submit(device.graphicsQueue, cmd);
    RI::Helpers::releaseSlot(slots, first, point);
    RI::wait(device.graphicsQueue, point);
    EQ_CHECK(RI::Helpers::allocateSlot(slots) == first);
    RI::destroyCommandList(device, cmd);
    RI::destroyDevice(device);
}

// One-call uploads and readbacks of a buffer and a two-layer texture.
EQ_TEST(helpers, transfers) {
    RI::Device device = createTestDevice();
    RI::Helpers::MemoryAllocator allocator = RI::Helpers::createMemoryAllocator(device);
    RI::Helpers::Allocation bufferAllocation, textureAllocation;
    RI::Buffer buffer = RI::Helpers::createBuffer(allocator, RI::MemoryKind::eGPU,
                                                  {.size = 4096, .usage = RI::BufferUsage::eCopySrc | RI::BufferUsage::eCopyDst},
                                                  bufferAllocation);
    std::vector<uint32> words(1024);
    for (uint32 i = 0; i < words.size(); ++i) {
        words[i] = i * 3 + 1;
    }
    RI::Helpers::uploadBuffer(device, buffer, 0, words.data(), 4096);
    const std::vector<std::byte> back = RI::Helpers::readbackBuffer(device, buffer, 0, 4096);
    EQ_CHECK(std::memcmp(back.data(), words.data(), 4096) == 0);

    RI::Texture texture = RI::Helpers::createTexture(
        allocator, {.width = 20, .height = 10, .layers = 2, .format = RI::Format::eRGBA8Unorm,
                    .usage = RI::TextureUsage::eCopySrc | RI::TextureUsage::eCopyDst},
        textureAllocation);
    std::vector<uint8> texels(20 * 10 * 4 * 2);
    for (size_t i = 0; i < texels.size(); ++i) {
        texels[i] = static_cast<uint8>(i * 13);
    }
    RI::Helpers::uploadTexture(device, texture.view, texels.data());
    const std::vector<std::byte> read = RI::Helpers::readbackTexture(device, texture.view);
    EQ_CHECK(read.size() == texels.size() && std::memcmp(read.data(), texels.data(), texels.size()) == 0);

    RI::destroyTexture(device, texture);
    RI::destroyBuffer(device, buffer);
    RI::Helpers::destroyMemoryAllocator(allocator);
    RI::destroyDevice(device);
}

// Three frames on a two-frame ring, each profiled.
EQ_TEST(helpers, frame_ring_and_profiler) {
    RI::Device device = createTestDevice();
    RI::Helpers::MemoryAllocator allocator = RI::Helpers::createMemoryAllocator(device);
    RI::Helpers::Allocation allocation;
    RI::Texture target = RI::Helpers::createTexture(
        allocator, {.width = 512, .height = 512, .format = RI::Format::eRGBA8Unorm, .usage = RI::TextureUsage::eRenderTarget},
        allocation);
    RI::Helpers::FrameRing ring = RI::Helpers::createFrameRing(device, 2);
    RI::Helpers::Profiler profiler = RI::Helpers::createProfiler(device);
    for (int frame = 0; frame < 3; ++frame) {
        RI::CommandList &cmd = RI::Helpers::beginFrame(ring);
        RI::Helpers::beginScope(profiler, cmd, "frame");
        RI::Helpers::beginScope(profiler, cmd, "clear");
        RI::cmdBeginRendering(cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}}});
        RI::cmdEndRendering(cmd);
        RI::Helpers::endScope(profiler, cmd);
        RI::Helpers::endScope(profiler, cmd);
        RI::Helpers::resolveProfiler(profiler, cmd);
        RI::wait(device.graphicsQueue, RI::Helpers::endFrame(ring));
        const std::vector<RI::Helpers::ScopeTime> times = RI::Helpers::profilerResults(profiler);
        EQ_CHECK(times.size() == 2 && times[0].name == "frame" && times[1].depth == 1);
        EQ_CHECK(times[0].milliseconds >= times[1].milliseconds && times[1].milliseconds >= 0.0);
    }
    RI::Helpers::destroyProfiler(profiler);
    RI::Helpers::destroyFrameRing(ring);
    RI::destroyTexture(device, target);
    RI::Helpers::destroyMemoryAllocator(allocator);
    RI::destroyDevice(device);
}

// Runtime HLSL compilation with RI's rule: the pipeline reads its root data.
EQ_TEST(helpers, shader_compiler) {
    RI::Device device = createTestDevice();
    RI::Helpers::ShaderCompiler compiler = RI::Helpers::createShaderCompiler();
    const char *code = R"(#include <equine/root.hlsli>
struct Root { uint slot; float value; };
EQ_ROOT_DATA(Root, root);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    RWStructuredBuffer<float> output = ResourceDescriptorHeap[root.slot];
    output[id.x] = root.value * VALUE_SCALE;
})";
    const RI::Helpers::CompiledShader shader = RI::Helpers::compileShader(
        compiler, {.code = code, .profile = "cs_6_6", .defines = {"VALUE_SCALE=2.0"}});
    EQ_CHECK(!shader.bytecode.empty());
    RI::Pipeline pipeline = RI::createComputePipeline(device, {.shader = shader.bytecode});
    EQ_CHECK(pipeline.rootDataSize == 8);
    const RI::Helpers::CompiledShader broken =
        RI::Helpers::compileShader(compiler, {.code = "void main( {", .profile = "cs_6_6"});
    EQ_CHECK(broken.bytecode.empty() && !broken.messages.empty());
    RI::destroyPipeline(device, pipeline);
    RI::Helpers::destroyShaderCompiler(compiler);
    RI::destroyDevice(device);
}

// Group grids (P106): counts above kMaxGroupsPerDimension fold into rows, and
// eqLinearGroup numbers every group once.
EQ_TEST(helpers, group_grid) {
    const RI::DispatchIndirectArgs small = RI::Helpers::groupGrid(100, 64);
    EQ_CHECK(small.x == 2 && small.y == 1 && small.z == 1);
    const RI::DispatchIndirectArgs exact = RI::Helpers::groupGrid(uint64{65535} * 64, 64);
    EQ_CHECK(exact.x == 65535 && exact.y == 1);
    const RI::DispatchIndirectArgs none = RI::Helpers::groupGrid(0, 64);
    EQ_CHECK(none.x == 0);

    constexpr uint32 kGroups = 70000;
    const RI::DispatchIndirectArgs grid = RI::Helpers::groupGrid(kGroups, 1);
    EQ_CHECK(grid.x == 65535 && grid.y == 2 && grid.z == 1);

    RI::Device device = createTestDevice();
    const RI::BufferDesc counterDesc{.size = 256,
                                     .usage = RI::BufferUsage::eReadWrite | RI::BufferUsage::eCopySrc |
                                              RI::BufferUsage::eCopyDst};
    const RI::MemoryRequirements requirements = RI::bufferRequirements(device, counterDesc);
    RI::Memory memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, requirements.size, &requirements);
    RI::Buffer counter = RI::createBuffer(device, memory, 0, counterDesc);
    const uint32 zero[64] = {};
    RI::Helpers::uploadBuffer(device, counter, 0, zero, sizeof(zero));
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 4});
    RI::writeDescriptors(device, heap, {{.slot = 1, .descriptor = RI::ReadWriteView{.buffer = counter}}});
    const std::vector<std::byte> bytecode = loadShader("grid_count");
    RI::Pipeline pipeline = RI::createComputePipeline(device, {.shader = bytecode});

    struct GridRoot {
        uint32 counterSlot;
        uint32 groups;
    };
    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    RI::cmdSetHeap(cmd, heap);
    RI::cmdSetPipeline(cmd, pipeline);
    RI::Helpers::cmdSetRootData(cmd, GridRoot{.counterSlot = 1, .groups = kGroups});
    RI::cmdDispatch(cmd, grid.x, grid.y, grid.z);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
    const std::vector<std::byte> back = RI::Helpers::readbackBuffer(device, counter, 0, 4);
    uint32 counted = 0;
    std::memcpy(&counted, back.data(), sizeof(counted));
    EQ_CHECK(counted == kGroups);

    RI::destroyCommandList(device, cmd);
    RI::destroyPipeline(device, pipeline);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroyBuffer(device, counter);
    RI::freeMemory(device, memory);
    RI::destroyDevice(device);
}

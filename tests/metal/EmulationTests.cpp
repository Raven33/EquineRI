// Metal-only tests of geometry and tessellation through MSC's emulation
// (P116 (1) amended): what the portable tests (texture.geometry_shader,
// texture.tessellation, texture.cube_layered_render) leave out: indexed and
// instanced draws, a depth-only pipeline (RI's empty fragment function), and
// the indirect draws Metal refuses.

#include <TestHarness.hpp>

#include "../gpu/GpuTestCommon.hpp"

#include <RI/Helpers/TypedCommands.hpp>
#include <RI/RI.hpp>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using gputest::Arena;
using gputest::loadShader;
using gputest::placeBuffer;
using gputest::placeTexture;

constexpr uint32 kSize = 32; // grid_instances_vs.hlsl's grid

struct DrawRoot {
    float color[4];
    float depth;
};

struct Fixture {
    RI::Device device = gputest::createTestDevice();
    Arena gpu{.memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 4 << 20)};
    Arena upload{.memory = RI::allocateMemory(device, RI::MemoryKind::eUpload, 1 << 20)};
    Arena readback{.memory = RI::allocateMemory(device, RI::MemoryKind::eReadback, 1 << 20)};
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 4, .samplerCount = 0});
    RI::CommandList cmd = RI::createCommandList(device);
    std::vector<RI::Buffer> buffers;

    Fixture() { begin(); }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
    ~Fixture() {
        for (RI::Buffer &buffer : buffers) {
            RI::destroyBuffer(device, buffer);
        }
        RI::destroyCommandList(device, cmd);
        RI::destroyDescriptorHeap(device, heap);
        RI::freeMemory(device, readback.memory);
        RI::freeMemory(device, upload.memory);
        RI::freeMemory(device, gpu.memory);
        RI::destroyDevice(device);
    }

    void begin() {
        RI::cmdBegin(cmd);
        RI::cmdSetHeap(cmd, heap);
    }

    const RI::Buffer &buffer(Arena &arena, uint64 size, RI::BufferUsage usage) {
        buffers.push_back(placeBuffer(device, arena, {.size = size, .usage = usage}));
        return buffers.back();
    }

    // Ends the recording, submits, waits, and returns the view's texels as
    // 32-bit words (RGBA8 or D32); the list is recording again after.
    std::vector<uint32> readBack(const RI::TextureView &view) {
        const RI::CopyLayout layout = RI::copyLayout(device, view);
        const RI::Buffer &out = buffer(readback, layout.size, RI::BufferUsage::eCopyDst);
        RI::cmdBarrier(cmd, RI::Stage::eRenderTarget | RI::Stage::eDepthStencil, RI::Stage::eCopy);
        RI::cmdCopyTextureToBuffer(cmd, view, out, 0);
        RI::cmdEnd(cmd);
        RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
        RI::resetCommandList(cmd);
        begin();
        std::vector<uint32> texels(kSize * kSize);
        for (uint32 y = 0; y < kSize; ++y) {
            std::memcpy(&texels[y * kSize], static_cast<const std::byte *>(out.mapped) + y * layout.rowPitch, kSize * 4);
        }
        return texels;
    }
};

} // namespace

// Indexed and instanced draws through a geometry shader that passes points on:
// indices {5, 3, 9}, two instances (the second 16 points further), with a
// first instance of 4 (SV_InstanceID starts at 0, as on D3D12) and a vertex
// offset of 2, which SV_VertexID leaves out as in RI's other Metal draws
// (Backend.hpp, cmdSetIndexBuffer). Then a draw that is not indexed, from
// vertex 20: SV_VertexID counts from firstVertex on, as in RI's other Metal draws.
EQ_TEST(metal, geometry_indexed_instanced) {
    Fixture f;
    RI::Texture target = placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .format = RI::Format::eRGBA8Unorm,
                                                        .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eCopySrc});
    const std::vector<std::byte> vs = loadShader("grid_instances_vs"), gs = loadShader("point_pass_gs"),
                                 ps = loadShader("color_ps");
    RI::Pipeline pipeline = RI::createGraphicsPipeline(f.device, {.vertex = vs, .geometry = gs, .pixel = ps,
                                                                  .colorFormats = {RI::Format::eRGBA8Unorm},
                                                                  .topology = RI::Topology::ePointList});
    const RI::Buffer &indices = f.buffer(f.upload, 16, RI::BufferUsage::eIndex);
    const uint16 values[] = {5, 3, 9, 0};
    std::memcpy(indices.mapped, values, sizeof(values));

    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::Helpers::cmdSetRootData(f.cmd, DrawRoot{.color = {1, 1, 1, 1}, .depth = 0.5f});
    RI::cmdSetIndexBuffer(f.cmd, indices, 0, RI::IndexFormat::eUint16);
    RI::cmdDrawIndexed(f.cmd, 3, 2, 0, 2, 4);
    RI::cmdEndRendering(f.cmd);
    std::vector<uint32> texels = f.readBack(target.view);
    for (uint32 i = 0; i < kSize * kSize; ++i) {
        const bool lit = i == 5 || i == 3 || i == 9 || i == 21 || i == 19 || i == 25;
        EQ_CHECK(texels[i] == (lit ? 0xffffffffu : 0u));
    }

    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::Helpers::cmdSetRootData(f.cmd, DrawRoot{.color = {1, 1, 1, 1}, .depth = 0.5f});
    RI::cmdDraw(f.cmd, 2, 1, 20, 0);
    RI::cmdEndRendering(f.cmd);
    texels = f.readBack(target.view);
    for (uint32 i = 0; i < kSize * kSize; ++i) {
        EQ_CHECK(texels[i] == (i < 2 ? 0xffffffffu : 0u));
    }
    RI::destroyPipeline(f.device, pipeline);
    RI::destroyTexture(f.device, target);
}

// A depth-only tessellation pipeline (no pixel shader: RI's empty fragment
// function), drawn with an index buffer: the patch covers the target at
// depth 0.25.
EQ_TEST(metal, tessellation_indexed_depth_only) {
    Fixture f;
    RI::Texture depth = placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .format = RI::Format::eD32Float,
                                                       .usage = RI::TextureUsage::eDepthStencil | RI::TextureUsage::eCopySrc});
    const std::vector<std::byte> vs = loadShader("patch_vs"), hs = loadShader("patch_hs"), ds = loadShader("patch_ds");
    RI::Pipeline pipeline = RI::createGraphicsPipeline(f.device, {.vertex = vs, .hull = hs, .domain = ds,
                                                                  .depthFormat = RI::Format::eD32Float,
                                                                  .topology = RI::Topology::ePatchList,
                                                                  .patchControlPoints = 3,
                                                                  .depth = {.test = true, .write = true}});
    const RI::Buffer &indices = f.buffer(f.upload, 16, RI::BufferUsage::eIndex);
    const uint32 values[] = {0, 1, 2, 0};
    std::memcpy(indices.mapped, values, sizeof(values));
    RI::cmdBeginRendering(f.cmd, {.depth = RI::DepthAttachment{.view = depth.view,
                                                              .load = RI::LoadOp::eClear,
                                                              .clear = {.depth = 1.0f}}});
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::Helpers::cmdSetRootData(f.cmd, DrawRoot{.color = {1, 1, 1, 1}, .depth = 0.25f});
    RI::cmdSetIndexBuffer(f.cmd, indices, 0, RI::IndexFormat::eUint32);
    RI::cmdDrawIndexed(f.cmd, 3);
    RI::cmdEndRendering(f.cmd);
    const std::vector<uint32> texels = f.readBack(depth.view);
    float quarter = 0.25f;
    uint32 bits = 0;
    std::memcpy(&bits, &quarter, sizeof(bits));
    for (uint32 i = 0; i < kSize * kSize; ++i) {
        EQ_CHECK(texels[i] == bits);
    }
    RI::destroyPipeline(f.device, pipeline);
    RI::destroyTexture(f.device, depth);
}

// Indirect draws of an emulated pipeline report unsupported (P116 (1)).
EQ_TEST(metal, geometry_indirect_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    Fixture f;
    RI::Texture target = placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .format = RI::Format::eRGBA8Unorm,
                                                        .usage = RI::TextureUsage::eRenderTarget});
    const std::vector<std::byte> vs = loadShader("grid_instances_vs"), gs = loadShader("point_pass_gs"),
                                 ps = loadShader("color_ps");
    RI::Pipeline pipeline = RI::createGraphicsPipeline(f.device, {.vertex = vs, .geometry = gs, .pixel = ps,
                                                                  .colorFormats = {RI::Format::eRGBA8Unorm},
                                                                  .topology = RI::Topology::ePointList});
    const RI::Buffer &args = f.buffer(f.upload, sizeof(RI::DrawIndirectArgs), RI::BufferUsage::eIndirect);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::cmdExecuteIndirect(f.cmd, RI::IndirectKind::eDraw, args, 0, 1);
}

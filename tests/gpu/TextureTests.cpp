// Portable GPU tests for textures, rendering, clears, discard and timestamps
// (Milestone 3). Every case checks texel values read back through a compute
// pass, so RI's layout handling (A', L1) is exercised end to end.

#include "GpuTestCommon.hpp"
#include "TextureChecks.hpp"

#include <TestHarness.hpp>

#include <RI/RI.hpp>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <vector>

namespace {

using gputest::Arena;
using gputest::createTestDevice;
using gputest::loadShader;
using gputest::placeBuffer;
using gputest::placeTexture;

struct DrawRoot {
    float color[4];
    float depth;
};

struct ReadRoot {
    uint32 textureSlot;
    uint32 outputSlot;
    uint32 width;
    uint32 height;
    uint32 multisampled;
    uint32 sampleIndex;
};

struct WriteRoot {
    uint32 textureSlot;
    uint32 width;
    uint32 height;
    float value;
};

constexpr uint32 kSize = 32;
constexpr uint32 kOutputSlot = 0;
constexpr uint32 kTextureSlot = 1;

// Everything a test needs: device, memory, a heap, an output buffer read back
// after the test's own commands, and the reading pipeline.
struct Fixture {
    RI::Device device;
    Arena gpu;
    Arena readback;
    RI::DescriptorHeap heap;
    RI::Buffer output;
    RI::Buffer result;
    std::vector<std::byte> readShader;
    RI::Pipeline read;
    RI::CommandList cmd;

    bool ownsDevice = true;

    Fixture() : Fixture(createTestDevice(), true) {}

    // `owns`: the fixture destroys the device at the end.
    Fixture(RI::Device existing, bool owns) {
        device = existing;
        ownsDevice = owns;
        gpu.memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 64ull << 20);
        readback.memory = RI::allocateMemory(device, RI::MemoryKind::eReadback, 1 << 20);
        heap = RI::createDescriptorHeap(device, {.resourceCount = 64});
        const uint64 bytes = kSize * kSize * 16;
        output = placeBuffer(device, gpu,
                             {.size = bytes,
                              .usage = RI::BufferUsage::eReadWrite | RI::BufferUsage::eCopySrc |
                                       RI::BufferUsage::eCopyDst});
        result = placeBuffer(device, readback, {.size = bytes, .usage = RI::BufferUsage::eCopyDst});
        RI::writeDescriptors(device, heap,
                             {{.slot = kOutputSlot, .descriptor = RI::ReadWriteView{.buffer = output, .stride = 16}}});
        readShader = loadShader("read_texture");
        read = RI::createComputePipeline(device, {.shader = readShader});
        cmd = RI::createCommandList(device);
        RI::cmdBegin(cmd);
        RI::cmdSetHeap(cmd, heap);
    }

    // Reads `view` (sample `sample` if multisampled) into `result`, submits,
    // waits, and returns the texels.
    std::vector<float> readBack(const RI::TextureView &view, uint32 sample = 0) {
        RI::writeDescriptors(device, heap, {{.slot = kTextureSlot, .descriptor = RI::ReadView{.texture = view}}});
        RI::cmdBarrier(cmd, RI::Stage::eAll, RI::Stage::eCompute);
        RI::cmdSetPipeline(cmd, read);
        RI::cmdSetRootData(cmd, ReadRoot{.textureSlot = kTextureSlot,
                                         .outputSlot = kOutputSlot,
                                         .width = kSize,
                                         .height = kSize,
                                         .multisampled = view.samples > 1 ? 1u : 0u,
                                         .sampleIndex = sample});
        RI::cmdDispatch(cmd, kSize / 8, kSize / 8, 1);
        RI::cmdBarrier(cmd, RI::Stage::eCompute, RI::Stage::eCopy);
        RI::cmdCopyBuffer(cmd, output, 0, result, 0, output.size);
        RI::cmdEnd(cmd);
        RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
        const auto *texels = static_cast<const float *>(result.mapped);
        return {texels, texels + kSize * kSize * 4};
    }

    ~Fixture() {
        RI::destroyCommandList(device, cmd);
        RI::destroyPipeline(device, read);
        RI::destroyDescriptorHeap(device, heap);
        RI::destroyBuffer(device, output);
        RI::destroyBuffer(device, result);
        RI::freeMemory(device, gpu.memory);
        RI::freeMemory(device, readback.memory);
        if (ownsDevice) {
            RI::destroyDevice(device);
        }
    }
};

bool near(float a, float b) { return std::fabs(a - b) < 1.0f / 128.0f; }

// Every texel's channel `channel` equals `value`.
bool all(const std::vector<float> &texels, uint32 channel, float value) {
    for (size_t i = channel; i < texels.size(); i += 4) {
        if (!near(texels[i], value)) {
            return false;
        }
    }
    return true;
}

RI::Pipeline drawPipeline(RI::Device &device, RI::Format color, RI::Format depth, uint32 samples, bool depthWrite,
                          std::vector<std::byte> &vs, std::vector<std::byte> &ps) {
    vs = loadShader("fullscreen_vs");
    ps = loadShader("solid_ps");
    RI::GraphicsPipelineDesc desc{.vertex = vs, .pixel = ps, .depthFormat = depth, .samples = samples};
    if (color != RI::Format::eUndefined) {
        desc.colorFormats = {color};
    } else {
        desc.pixel = {};
    }
    desc.depth = {.test = depth != RI::Format::eUndefined, .write = depthWrite, .compare = RI::CompareOp::eAlways};
    return RI::createGraphicsPipeline(device, desc);
}

} // namespace

// A texture created with each usage reports its facts and a whole view (P58, P73).
EQ_TEST(texture, create_and_views) {
    Fixture f;
    const RI::TextureDesc desc{.width = 64,
                               .height = 16,
                               .mips = 3,
                               .format = RI::Format::eRGBA8Unorm,
                               .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eRenderTarget,
                               .viewFormats = {RI::Format::eRGBA8Srgb}};
    const RI::MemoryRequirements req = RI::textureRequirements(f.device, desc);
    EQ_CHECK(req.size > 0 && req.alignment > 0 && RI::fits(req, RI::MemoryKind::eGPU));
    RI::Texture texture = placeTexture(f.device, f.gpu, desc);
    EQ_CHECK(texture.width == 64 && texture.height == 16 && texture.mips == 3);
    EQ_CHECK(texture.view.format == RI::Format::eRGBA8Unorm && texture.view.width == 64);
    RI::TextureView mip1 = RI::createView(f.device, texture, {.format = RI::Format::eRGBA8Srgb, .baseMip = 1, .mipCount = 1});
    EQ_CHECK(mip1.width == 32 && mip1.height == 8 && mip1.format == RI::Format::eRGBA8Srgb);
    RI::destroyView(f.device, mip1);
    RI::destroyTexture(f.device, texture);
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));
}

// Render (clear + draw) -> sample, for several color formats (L1 item 2).
void gputest::renderThenSample(RI::Device &device) {
    Fixture f(device, false);
    for (RI::Format format : {RI::Format::eRGBA8Unorm, RI::Format::eRGBA16Float, RI::Format::eR11G11B10Float}) {
        if (!RI::formatSupported(f.device, format, RI::TextureUsage::eSampled | RI::TextureUsage::eRenderTarget)) {
            continue;
        }
        RI::Texture texture = placeTexture(f.device, f.gpu,
                                           {.width = kSize,
                                            .height = kSize,
                                            .format = format,
                                            .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eRenderTarget});
        std::vector<std::byte> vs, ps;
        RI::Pipeline pipeline = drawPipeline(f.device, format, RI::Format::eUndefined, 1, false, vs, ps);
        RI::cmdBeginRendering(f.cmd, {.colors = {{.view = texture.view, .load = RI::LoadOp::eClear}}});
        RI::cmdSetPipeline(f.cmd, pipeline);
        RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.25f, 0.5f, 0.75f, 1.0f}, .depth = 0.5f});
        RI::cmdDraw(f.cmd, 3);
        RI::cmdEndRendering(f.cmd);
        const std::vector<float> texels = f.readBack(texture.view);
        EQ_CHECK(all(texels, 0, 0.25f) && all(texels, 1, 0.5f) && all(texels, 2, 0.75f));
        RI::resetCommandList(f.cmd);
        RI::cmdBegin(f.cmd);
        RI::cmdSetHeap(f.cmd, f.heap);
        RI::destroyPipeline(f.device, pipeline);
        RI::destroyTexture(f.device, texture);
    }
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));
}

EQ_TEST(texture, render_then_sample) {
    RI::Device device = createTestDevice();
    gputest::renderThenSample(device);
    RI::destroyDevice(device);
}

// A cleared attachment that is not drawn to keeps its clear color (load op).
EQ_TEST(texture, clear_load_op) {
    Fixture f;
    RI::Texture texture = placeTexture(f.device, f.gpu,
                                       {.width = kSize,
                                        .height = kSize,
                                        .format = RI::Format::eRGBA16Float,
                                        .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eRenderTarget});
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = texture.view,
                                              .load = RI::LoadOp::eClear,
                                              .clear = {.color = {1.0f, 0.0f, 0.5f, 1.0f}}}}});
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(texture.view);
    EQ_CHECK(all(texels, 0, 1.0f) && all(texels, 1, 0.0f) && all(texels, 2, 0.5f));
    RI::destroyTexture(f.device, texture);
}

// Depth write -> sample (L1 item 4).
void gputest::depthWriteThenSample(RI::Device &device) {
    Fixture f(device, false);
    for (RI::Format format : {RI::Format::eD32Float, RI::Format::eD32FloatS8Uint, RI::Format::eD24UnormS8Uint}) {
        const RI::TextureUsage usage = RI::TextureUsage::eSampled | RI::TextureUsage::eDepthStencil;
        if (!RI::formatSupported(f.device, format, usage)) {
            continue;
        }
        RI::Texture depth = placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .format = format, .usage = usage});
        RI::TextureView depthOnly = RI::createView(f.device, depth, {.aspect = RI::Aspect::eDepth});
        std::vector<std::byte> vs, ps;
        RI::Pipeline pipeline = drawPipeline(f.device, RI::Format::eUndefined, format, 1, true, vs, ps);
        RI::cmdBeginRendering(f.cmd, {.depth = RI::DepthAttachment{.view = depth.view,
                                                                    .load = RI::LoadOp::eClear,
                                                                    .stencilLoad = RI::LoadOp::eClear,
                                                                    .clear = {.depth = 1.0f}}});
        RI::cmdSetPipeline(f.cmd, pipeline);
        RI::cmdSetRootData(f.cmd, DrawRoot{.color = {}, .depth = 0.25f});
        RI::cmdDraw(f.cmd, 3);
        RI::cmdEndRendering(f.cmd);
        const std::vector<float> texels = f.readBack(depthOnly);
        EQ_CHECK(all(texels, 0, 0.25f));
        RI::resetCommandList(f.cmd);
        RI::cmdBegin(f.cmd);
        RI::cmdSetHeap(f.cmd, f.heap);
        RI::destroyPipeline(f.device, pipeline);
        RI::destroyView(f.device, depthOnly);
        RI::destroyTexture(f.device, depth);
    }
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));
}

EQ_TEST(texture, depth_write_then_sample) {
    RI::Device device = createTestDevice();
    gputest::depthWriteThenSample(device);
    RI::destroyDevice(device);
}

// Read-only depth (P75): a section keeps the depth contents, with and without
// eReadOnlyDepthSampling (P97: without it, Vulkan mode T switches the texture
// in and out of the section).
void readOnlyDepthSection(RI::TextureUsage extra) {
    Fixture f;
    const RI::TextureUsage usage = RI::TextureUsage::eSampled | RI::TextureUsage::eDepthStencil | extra;
    RI::Texture depth =
        placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .format = RI::Format::eD32Float, .usage = usage});
    RI::cmdBeginRendering(f.cmd, {.depth = RI::DepthAttachment{.view = depth.view, .load = RI::LoadOp::eClear,
                                                                .clear = {.depth = 0.75f}}});
    RI::cmdEndRendering(f.cmd);
    // A read-only section with no draw: the texture keeps its contents.
    RI::cmdBeginRendering(f.cmd, {.depth = RI::DepthAttachment{.view = depth.view, .depthReadOnly = true}});
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(depth.view);
    EQ_CHECK(all(texels, 0, 0.75f));
    RI::destroyTexture(f.device, depth);
}

EQ_TEST(texture, read_only_depth_section) { readOnlyDepthSection(RI::TextureUsage::eNone); }

EQ_TEST(texture, read_only_depth_section_sampling) {
    readOnlyDepthSection(RI::TextureUsage::eReadOnlyDepthSampling);
}

// 4x MSAA render -> sample one sample (L1 item 2, MSAA).
EQ_TEST(texture, msaa_render_then_sample) {
    Fixture f;
    const RI::TextureUsage usage = RI::TextureUsage::eSampled | RI::TextureUsage::eRenderTarget;
    if (!RI::formatSupported(f.device, RI::Format::eRGBA16Float, usage, 4)) {
        return;
    }
    RI::Texture texture = placeTexture(
        f.device, f.gpu, {.width = kSize, .height = kSize, .samples = 4, .format = RI::Format::eRGBA16Float, .usage = usage});
    EQ_CHECK(texture.view.samples == 4);
    std::vector<std::byte> vs, ps;
    RI::Pipeline pipeline = drawPipeline(f.device, RI::Format::eRGBA16Float, RI::Format::eUndefined, 4, false, vs, ps);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = texture.view, .load = RI::LoadOp::eDontCare}}});
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.5f, 0.25f, 1.0f, 1.0f}, .depth = 0.5f});
    RI::cmdDraw(f.cmd, 3);
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(texture.view, 3);
    EQ_CHECK(all(texels, 0, 0.5f) && all(texels, 1, 0.25f));
    RI::destroyPipeline(f.device, pipeline);
    RI::destroyTexture(f.device, texture);
}

// Storage write -> sample (L1 item 3).
void gputest::storageWriteThenSample(RI::Device &device) {
    Fixture f(device, false);
    const RI::TextureUsage usage = RI::TextureUsage::eSampled | RI::TextureUsage::eStorage;
    RI::Texture texture =
        placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .format = RI::Format::eRGBA16Float, .usage = usage});
    RI::writeDescriptors(f.device, f.heap, {{.slot = 2, .descriptor = RI::ReadWriteView{.texture = texture.view}}});
    const std::vector<std::byte> shader = loadShader("write_storage");
    RI::Pipeline write = RI::createComputePipeline(f.device, {.shader = shader});
    RI::cmdSetPipeline(f.cmd, write);
    RI::cmdSetRootData(f.cmd, WriteRoot{.textureSlot = 2, .width = kSize, .height = kSize, .value = 7.0f});
    RI::cmdDispatch(f.cmd, kSize / 8, kSize / 8, 1);
    const std::vector<float> texels = f.readBack(texture.view);
    EQ_CHECK(all(texels, 2, 7.0f));
    EQ_CHECK(near(texels[(5 * kSize + 3) * 4 + 0], 3.0f) && near(texels[(5 * kSize + 3) * 4 + 1], 5.0f));
    RI::destroyPipeline(f.device, write);
    RI::destroyTexture(f.device, texture);
}

EQ_TEST(texture, storage_write_then_sample) {
    RI::Device device = createTestDevice();
    gputest::storageWriteThenSample(device);
    RI::destroyDevice(device);
}

// Clears outside rendering sections (P65): a render target, a storage texture,
// a depth texture, and a buffer.
EQ_TEST(texture, clear_commands) {
    Fixture f;
    RI::Texture color = placeTexture(f.device, f.gpu,
                                     {.width = kSize,
                                      .height = kSize,
                                      .format = RI::Format::eRGBA8Unorm,
                                      .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eRenderTarget});
    RI::Texture storage = placeTexture(f.device, f.gpu,
                                       {.width = kSize,
                                        .height = kSize,
                                        .format = RI::Format::eRGBA16Float,
                                        .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eStorage});
    RI::Texture depth = placeTexture(f.device, f.gpu,
                                     {.width = kSize,
                                      .height = kSize,
                                      .format = RI::Format::eD32Float,
                                      .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eDepthStencil});
    RI::cmdClearTexture(f.cmd, color.view, {.color = {0.0f, 1.0f, 0.0f, 1.0f}});
    RI::cmdClearTexture(f.cmd, storage.view, {.color = {2.0f, 3.0f, 4.0f, 5.0f}});
    RI::cmdClearTexture(f.cmd, depth.view, {.depth = 0.125f});
    std::vector<float> texels = f.readBack(color.view);
    EQ_CHECK(all(texels, 1, 1.0f) && all(texels, 0, 0.0f));

    for (const RI::Texture *t : {&storage, &depth}) {
        RI::resetCommandList(f.cmd);
        RI::cmdBegin(f.cmd);
        RI::cmdSetHeap(f.cmd, f.heap);
        texels = f.readBack(t->view);
        EQ_CHECK(t == &storage ? all(texels, 3, 5.0f) : all(texels, 0, 0.125f));
    }

    // A buffer clear, read back directly.
    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdClearBuffer(f.cmd, f.output, 0x3f800000u); // 1.0f
    RI::cmdBarrier(f.cmd, RI::Stage::eCopy | RI::Stage::eCompute, RI::Stage::eCopy);
    RI::cmdCopyBuffer(f.cmd, f.output, 0, f.result, 0, f.output.size);
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));
    const auto *values = static_cast<const float *>(f.result.mapped);
    EQ_CHECK(values[0] == 1.0f && values[kSize * kSize * 4 - 1] == 1.0f);
    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);

    RI::destroyTexture(f.device, color);
    RI::destroyTexture(f.device, storage);
    RI::destroyTexture(f.device, depth);
}

// A texture placed over another one's memory is discarded before use (P69).
EQ_TEST(texture, discard_after_aliasing) {
    Fixture f;
    const RI::TextureDesc desc{.width = kSize,
                               .height = kSize,
                               .format = RI::Format::eRGBA8Unorm,
                               .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eRenderTarget};
    const uint64 offset = gputest::alignUp(f.gpu.used, RI::textureRequirements(f.device, desc).alignment);
    RI::Texture first = RI::createTexture(f.device, f.gpu.memory, offset, desc);
    RI::cmdClearTexture(f.cmd, first.view, {.color = {1.0f, 1.0f, 1.0f, 1.0f}});
    // Destruction is immediate (P20): only after the GPU is done with `first`.
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));
    RI::destroyTexture(f.device, first);
    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::Texture second = RI::createTexture(f.device, f.gpu.memory, offset, desc);
    RI::cmdDiscard(f.cmd, second.view);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = second.view,
                                              .load = RI::LoadOp::eClear,
                                              .clear = {.color = {0.0f, 0.0f, 1.0f, 1.0f}}}}});
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(second.view);
    EQ_CHECK(all(texels, 2, 1.0f) && all(texels, 0, 0.0f));
    RI::destroyTexture(f.device, second);
}

// Timestamps (P51, P71): increasing ticks around GPU work; frequency reported.
EQ_TEST(texture, timestamps) {
    Fixture f;
    EQ_CHECK(f.device.limits.timestampFrequency > 0);
    RI::QueryPool pool = RI::createQueryPool(f.device, {.type = RI::QueryType::eTimestamp, .count = 4});
    Arena readback{.memory = RI::allocateMemory(f.device, RI::MemoryKind::eReadback, 1 << 16)};
    RI::Buffer ticks = placeBuffer(f.device, readback, {.size = 32, .usage = RI::BufferUsage::eCopyDst});
    RI::Texture texture = placeTexture(f.device, f.gpu,
                                       {.width = 1024,
                                        .height = 1024,
                                        .format = RI::Format::eRGBA16Float,
                                        .usage = RI::TextureUsage::eRenderTarget});
    RI::cmdWriteTimestamp(f.cmd, pool, 0);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = texture.view, .load = RI::LoadOp::eClear}}});
    RI::cmdEndRendering(f.cmd);
    RI::cmdWriteTimestamp(f.cmd, pool, 1);
    RI::cmdResolveQueries(f.cmd, pool, 0, 2, ticks, 0);
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));
    const auto *values = static_cast<const uint64 *>(ticks.mapped);
    EQ_CHECK(values[1] >= values[0] && values[0] != 0);
    RI::resetQueries(f.device, pool, 0, 2);

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::destroyTexture(f.device, texture);
    RI::destroyBuffer(f.device, ticks);
    RI::freeMemory(f.device, readback.memory);
    RI::destroyQueryPool(f.device, pool);
}

// An unlisted view format is a readable error in every build (P74).
EQ_TEST(texture, unlisted_view_format_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    Fixture f;
    RI::Texture texture = placeTexture(f.device, f.gpu,
                                       {.width = kSize,
                                        .height = kSize,
                                        .format = RI::Format::eRGBA8Unorm,
                                        .usage = RI::TextureUsage::eSampled});
    RI::createView(f.device, texture, {.format = RI::Format::eRGBA8Srgb});
}

#if EQUINE_VALIDATION
// eReadOnlyDepthSampling on a color texture (P97).
EQ_TEST(texture, read_only_depth_sampling_on_color_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    Fixture f;
    placeTexture(f.device, f.gpu,
                 {.width = kSize,
                  .height = kSize,
                  .format = RI::Format::eRGBA8Unorm,
                  .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eReadOnlyDepthSampling});
}

// A depth-writing pipeline in a read-only depth section (P75).
EQ_TEST(texture, depth_write_in_read_only_section_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    Fixture f;
    RI::Texture depth = placeTexture(f.device, f.gpu,
                                     {.width = kSize,
                                      .height = kSize,
                                      .format = RI::Format::eD32Float,
                                      .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eDepthStencil});
    std::vector<std::byte> vs, ps;
    RI::Pipeline pipeline = drawPipeline(f.device, RI::Format::eUndefined, RI::Format::eD32Float, 1, true, vs, ps);
    RI::cmdBeginRendering(f.cmd, {.depth = RI::DepthAttachment{.view = depth.view, .depthReadOnly = true}});
    RI::cmdSetPipeline(f.cmd, pipeline);
}

// A texture placed over used memory and rendered without discard (P69).
EQ_TEST(texture, missing_discard_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    Fixture f;
    const RI::TextureDesc desc{.width = kSize,
                               .height = kSize,
                               .format = RI::Format::eRGBA8Unorm,
                               .usage = RI::TextureUsage::eRenderTarget};
    const uint64 offset = gputest::alignUp(f.gpu.used, RI::textureRequirements(f.device, desc).alignment);
    RI::Texture first = RI::createTexture(f.device, f.gpu.memory, offset, desc);
    RI::destroyTexture(f.device, first);
    RI::Texture second = RI::createTexture(f.device, f.gpu.memory, offset, desc);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = second.view, .load = RI::LoadOp::eClear}}});
}
#endif

namespace {

// The value of byte `c` of texel (x, y) in `layer` for the copy tests.
uint8 copyPattern(uint32 x, uint32 y, uint32 layer, uint32 c) { return static_cast<uint8>(x + y * 3 + layer * 7 + c * 11); }

} // namespace

// Buffer -> texture -> texture -> buffer, on one mip of a two-layer texture
// (P47): the bytes come back where copyLayout says.
EQ_TEST(texture, copy_round_trip) {
    Fixture f;
    const RI::TextureDesc desc{.width = 64,
                               .height = 48,
                               .layers = 2,
                               .mips = 2,
                               .format = RI::Format::eRGBA8Unorm,
                               .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eCopySrc |
                                        RI::TextureUsage::eCopyDst};
    RI::Texture a = placeTexture(f.device, f.gpu, desc);
    RI::Texture b = placeTexture(f.device, f.gpu, desc);
    RI::TextureView aMip = RI::createView(f.device, a, {.baseMip = 1, .mipCount = 1});
    RI::TextureView bMip = RI::createView(f.device, b, {.baseMip = 1, .mipCount = 1});
    const RI::CopyLayout layout = RI::copyLayout(f.device, aMip);
    EQ_CHECK(layout.width == 32 && layout.height == 24 && layout.layers == 2 && layout.bytesPerTexel == 4);
    EQ_CHECK(layout.rowPitch >= 32 * 4 && layout.layerPitch >= layout.rowPitch * 24);
    EQ_CHECK(layout.layerPitch % layout.alignment == 0);

    Arena upload{.memory = RI::allocateMemory(f.device, RI::MemoryKind::eUpload, 1 << 20)};
    RI::Buffer staging = placeBuffer(f.device, upload, {.size = layout.size, .usage = RI::BufferUsage::eCopySrc});
    RI::Buffer back = placeBuffer(f.device, f.readback, {.size = layout.size, .usage = RI::BufferUsage::eCopyDst});
    auto *bytes = static_cast<uint8 *>(staging.mapped);
    for (uint32 layer = 0; layer < 2; ++layer) {
        for (uint32 y = 0; y < 24; ++y) {
            for (uint32 x = 0; x < 32; ++x) {
                for (uint32 c = 0; c < 4; ++c) {
                    bytes[layer * layout.layerPitch + y * layout.rowPitch + x * 4 + c] = copyPattern(x, y, layer, c);
                }
            }
        }
    }
    RI::cmdCopyBufferToTexture(f.cmd, staging, 0, aMip);
    RI::cmdBarrier(f.cmd, RI::Stage::eCopy, RI::Stage::eCopy);
    RI::cmdCopyTexture(f.cmd, aMip, bMip);
    RI::cmdBarrier(f.cmd, RI::Stage::eCopy, RI::Stage::eCopy);
    RI::cmdCopyTextureToBuffer(f.cmd, bMip, back, 0);
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));

    const auto *out = static_cast<const uint8 *>(back.mapped);
    uint32 wrong = 0;
    for (uint32 layer = 0; layer < 2; ++layer) {
        for (uint32 y = 0; y < 24; ++y) {
            for (uint32 x = 0; x < 32; ++x) {
                for (uint32 c = 0; c < 4; ++c) {
                    wrong += out[layer * layout.layerPitch + y * layout.rowPitch + x * 4 + c] !=
                             copyPattern(x, y, layer, c);
                }
            }
        }
    }
    EQ_CHECK(wrong == 0);

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::destroyBuffer(f.device, staging);
    RI::destroyBuffer(f.device, back);
    RI::freeMemory(f.device, upload.memory);
    RI::destroyView(f.device, aMip);
    RI::destroyView(f.device, bMip);
    RI::destroyTexture(f.device, a);
    RI::destroyTexture(f.device, b);
}

// Depth values uploaded with a copy are what a shader then samples (P47).
EQ_TEST(texture, copy_depth_upload) {
    Fixture f;
    RI::Texture depth = placeTexture(f.device, f.gpu,
                                     {.width = kSize,
                                      .height = kSize,
                                      .format = RI::Format::eD32Float,
                                      .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eDepthStencil |
                                               RI::TextureUsage::eCopyDst});
    const RI::CopyLayout layout = RI::copyLayout(f.device, depth.view);
    EQ_CHECK(layout.bytesPerTexel == 4);
    Arena upload{.memory = RI::allocateMemory(f.device, RI::MemoryKind::eUpload, 1 << 20)};
    RI::Buffer staging = placeBuffer(f.device, upload, {.size = layout.size, .usage = RI::BufferUsage::eCopySrc});
    auto *bytes = static_cast<uint8 *>(staging.mapped);
    for (uint32 y = 0; y < kSize; ++y) {
        auto *row = reinterpret_cast<float *>(bytes + y * layout.rowPitch);
        for (uint32 x = 0; x < kSize; ++x) {
            row[x] = 0.25f;
        }
    }
    RI::cmdCopyBufferToTexture(f.cmd, staging, 0, depth.view);
    const std::vector<float> texels = f.readBack(depth.view);
    EQ_CHECK(all(texels, 0, 0.25f));
    RI::destroyBuffer(f.device, staging);
    RI::freeMemory(f.device, upload.memory);
    RI::destroyTexture(f.device, depth);
}

// Occlusion counts every covered sample; pipeline statistics count draws and
// dispatches at the offsets pipelineStatisticsLayout gives (P71).
EQ_TEST(texture, occlusion_and_statistics) {
    Fixture f;
    RI::Texture target = placeTexture(f.device, f.gpu,
                                      {.width = kSize,
                                       .height = kSize,
                                       .format = RI::Format::eRGBA8Unorm,
                                       .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
    std::vector<std::byte> vs, ps;
    RI::Pipeline pipeline = drawPipeline(f.device, RI::Format::eRGBA8Unorm, RI::Format::eUndefined, 1, false, vs, ps);
    RI::QueryPool occlusion = RI::createQueryPool(f.device, {.type = RI::QueryType::eOcclusion, .count = 1});
    RI::QueryPool statistics = RI::createQueryPool(f.device, {.type = RI::QueryType::ePipelineStatistics, .count = 2});
    const RI::PipelineStatisticsLayout stats = RI::pipelineStatisticsLayout(f.device);
    RI::Buffer results =
        placeBuffer(f.device, f.readback, {.size = 8 + 2 * uint64{stats.size}, .usage = RI::BufferUsage::eCopyDst});

    // Inside the section: the clear at cmdBeginRendering is GPU work that
    // drivers may run as compute, which statistics would count.
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdBeginQuery(f.cmd, statistics, 0);
    RI::cmdBeginQuery(f.cmd, occlusion, 0);
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {1.0f, 0.0f, 0.0f, 1.0f}, .depth = 0.5f});
    RI::cmdDraw(f.cmd, 3);
    RI::cmdEndQuery(f.cmd, occlusion, 0);
    RI::cmdEndQuery(f.cmd, statistics, 0);
    RI::cmdEndRendering(f.cmd);

    RI::writeDescriptors(f.device, f.heap, {{.slot = kTextureSlot, .descriptor = RI::ReadView{.texture = target.view}}});
    RI::cmdBarrier(f.cmd, RI::Stage::eRenderTarget, RI::Stage::eCompute);
    RI::cmdBeginQuery(f.cmd, statistics, 1);
    RI::cmdSetPipeline(f.cmd, f.read);
    RI::cmdSetRootData(f.cmd, ReadRoot{.textureSlot = kTextureSlot, .outputSlot = kOutputSlot, .width = kSize,
                                       .height = kSize, .multisampled = 0, .sampleIndex = 0});
    RI::cmdDispatch(f.cmd, kSize / 8, kSize / 8, 1);
    RI::cmdEndQuery(f.cmd, statistics, 1);
    RI::cmdResolveQueries(f.cmd, occlusion, 0, 1, results, 0);
    RI::cmdResolveQueries(f.cmd, statistics, 0, 2, results, 8);
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));

    const auto *base = static_cast<const uint8 *>(results.mapped);
    const auto value = [&](uint64 offset) { return *reinterpret_cast<const uint64 *>(base + offset); };
    EQ_CHECK(value(0) == kSize * kSize);
    const uint64 draw = 8;
    const uint64 dispatch = 8 + stats.size;
    EQ_CHECK(value(draw + stats.vertexShaderInvocations) >= 3);
    EQ_CHECK(value(draw + stats.inputPrimitives) == 1);
    // How pixel-shader invocations are counted is up to the GPU: AMD counts
    // 2x2 quads, so 1024 covered pixels may read 256.
    EQ_CHECK(value(draw + stats.pixelShaderInvocations) >= kSize * kSize / 4);
    EQ_CHECK(value(draw + stats.computeShaderInvocations) == 0);
    EQ_CHECK(value(dispatch + stats.computeShaderInvocations) == kSize * kSize);
    EQ_CHECK(value(dispatch + stats.pixelShaderInvocations) == 0);

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::destroyQueryPool(f.device, occlusion);
    RI::destroyQueryPool(f.device, statistics);
    RI::destroyBuffer(f.device, results);
    RI::destroyPipeline(f.device, pipeline);
    RI::destroyTexture(f.device, target);
}

#if EQUINE_VALIDATION
// A copy needs the texture's copy usage (P47).
EQ_TEST(texture, copy_without_usage_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    Fixture f;
    RI::Texture texture = placeTexture(f.device, f.gpu,
                                       {.width = kSize, .height = kSize, .format = RI::Format::eRGBA8Unorm,
                                        .usage = RI::TextureUsage::eSampled});
    RI::Buffer back = placeBuffer(f.device, f.readback, {.size = 1 << 16, .usage = RI::BufferUsage::eCopyDst});
    RI::cmdCopyTextureToBuffer(f.cmd, texture.view, back, 0);
}
#endif

// Indexed draws (P39) with additive blending: two instances of the same
// full-screen triangle, each adding 0.25, give 0.5.
EQ_TEST(texture, indexed_draw_with_blending) {
    Fixture f;
    RI::Texture target = placeTexture(f.device, f.gpu,
                                      {.width = kSize, .height = kSize, .format = RI::Format::eRGBA16Float,
                                       .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
    Arena upload{.memory = RI::allocateMemory(f.device, RI::MemoryKind::eUpload, 1 << 16)};
    RI::Buffer indices = placeBuffer(f.device, upload, {.size = 16, .usage = RI::BufferUsage::eIndex});
    const uint16 values[] = {0, 1, 2, 0};
    std::memcpy(indices.mapped, values, sizeof(values));

    std::vector<std::byte> vs = loadShader("fullscreen_vs"), ps = loadShader("solid_ps");
    const RI::BlendState additive{.enable = true,
                                  .srcColor = RI::BlendFactor::eOne,
                                  .dstColor = RI::BlendFactor::eOne,
                                  .srcAlpha = RI::BlendFactor::eOne,
                                  .dstAlpha = RI::BlendFactor::eOne};
    RI::Pipeline pipeline = RI::createGraphicsPipeline(
        f.device, {.vertex = vs, .pixel = ps, .colorFormats = {RI::Format::eRGBA16Float}, .blend = {additive}});
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.25f, 0.25f, 0.25f, 0.25f}, .depth = 0.5f});
    RI::cmdSetIndexBuffer(f.cmd, indices, 0, RI::IndexFormat::eUint16);
    RI::cmdDrawIndexed(f.cmd, 3, 2);
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(target.view);
    EQ_CHECK(all(texels, 0, 0.5f) && all(texels, 3, 0.5f));
    RI::destroyPipeline(f.device, pipeline);
    RI::destroyBuffer(f.device, indices);
    RI::freeMemory(f.device, upload.memory);
    RI::destroyTexture(f.device, target);
}

// The stencil test (P38): pass 1 writes 1 everywhere; a draw testing for 2 is
// stopped, one testing for 1 (writing only green) goes through.
EQ_TEST(texture, stencil_test) {
    Fixture f;
    const RI::Format depthFormat = RI::Format::eD32FloatS8Uint;
    if (!RI::formatSupported(f.device, depthFormat, RI::TextureUsage::eDepthStencil)) {
        return;
    }
    RI::Texture target = placeTexture(f.device, f.gpu,
                                      {.width = kSize, .height = kSize, .format = RI::Format::eRGBA8Unorm,
                                       .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
    RI::Texture depth = placeTexture(f.device, f.gpu,
                                     {.width = kSize, .height = kSize, .format = depthFormat,
                                      .usage = RI::TextureUsage::eDepthStencil});
    std::vector<std::byte> vs = loadShader("fullscreen_vs"), ps = loadShader("solid_ps");
    const auto pipelineWith = [&](RI::CompareOp compare, RI::StencilOp pass, uint8 reference, RI::ColorMask mask) {
        const RI::StencilFace face{.pass = pass, .compare = compare};
        return RI::createGraphicsPipeline(
            f.device, {.vertex = vs,
                       .pixel = ps,
                       .colorFormats = {RI::Format::eRGBA8Unorm},
                       .depthFormat = depthFormat,
                       .stencil = {.test = true, .reference = reference, .front = face, .back = face},
                       .blend = {{.writeMask = mask}}});
    };
    RI::Pipeline write = pipelineWith(RI::CompareOp::eAlways, RI::StencilOp::eReplace, 1, RI::ColorMask::eNone);
    RI::Pipeline red = pipelineWith(RI::CompareOp::eEqual, RI::StencilOp::eKeep, 2, RI::ColorMask::eAll);
    RI::Pipeline green = pipelineWith(RI::CompareOp::eEqual, RI::StencilOp::eKeep, 1, RI::ColorMask::eG);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}},
                                  .depth = RI::DepthAttachment{.view = depth.view,
                                                               .load = RI::LoadOp::eClear,
                                                               .stencilLoad = RI::LoadOp::eClear}});
    RI::cmdSetPipeline(f.cmd, write);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.0f, 0.0f, 0.0f, 0.0f}, .depth = 0.5f});
    RI::cmdDraw(f.cmd, 3);
    RI::cmdSetPipeline(f.cmd, red);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {1.0f, 0.0f, 0.0f, 1.0f}, .depth = 0.5f});
    RI::cmdDraw(f.cmd, 3);
    RI::cmdSetPipeline(f.cmd, green);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.0f, 1.0f, 0.0f, 1.0f}, .depth = 0.5f});
    RI::cmdDraw(f.cmd, 3);
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(target.view);
    EQ_CHECK(all(texels, 0, 0.0f) && all(texels, 1, 1.0f) && all(texels, 3, 0.0f));
    RI::destroyPipeline(f.device, write);
    RI::destroyPipeline(f.device, red);
    RI::destroyPipeline(f.device, green);
    RI::destroyTexture(f.device, depth);
    RI::destroyTexture(f.device, target);
}

// Mesh pipelines (P39): a mesh shader alone, then a task shader launching it,
// each drawing the full-screen triangle.
EQ_TEST(texture, mesh_shaders) {
    Fixture f;
    if (!f.device.limits.meshShaders) {
        return;
    }
    RI::Texture target = placeTexture(f.device, f.gpu,
                                      {.width = kSize, .height = kSize, .format = RI::Format::eRGBA8Unorm,
                                       .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
    // D3D12 requires a mesh shader's input payload to match its task shader's output.
    std::vector<std::byte> as = loadShader("fullscreen_as"), ms = loadShader("fullscreen_ms"),
                           payloadMs = loadShader("fullscreen_payload_ms"), ps = loadShader("solid_ps");
    RI::Pipeline meshOnly = RI::createGraphicsPipeline(
        f.device, {.pixel = ps, .mesh = ms, .colorFormats = {RI::Format::eRGBA8Unorm}});
    RI::Pipeline taskMesh = RI::createGraphicsPipeline(
        f.device, {.pixel = ps, .task = as, .mesh = payloadMs, .colorFormats = {RI::Format::eRGBA8Unorm},
                   .blend = {{.writeMask = RI::ColorMask::eG}}});
    EQ_CHECK(meshOnly.rootDataSize == sizeof(DrawRoot) && taskMesh.rootDataSize == sizeof(DrawRoot));
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(f.cmd, meshOnly);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {1.0f, 0.0f, 0.0f, 1.0f}, .depth = 0.5f});
    RI::cmdDrawMesh(f.cmd, 1, 1, 1);
    RI::cmdSetPipeline(f.cmd, taskMesh);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.0f, 1.0f, 0.0f, 1.0f}, .depth = 0.5f});
    RI::cmdDrawMesh(f.cmd, 1, 1, 1);
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(target.view);
    EQ_CHECK(all(texels, 0, 1.0f) && all(texels, 1, 1.0f) && all(texels, 3, 1.0f));
    RI::destroyPipeline(f.device, meshOnly);
    RI::destroyPipeline(f.device, taskMesh);
    RI::destroyTexture(f.device, target);
}

// Color resolves (P52): at cmdEndRendering through resolveTo, and with
// cmdResolve outside a section.
EQ_TEST(texture, resolve_color) {
    Fixture f;
    const RI::Format format = RI::Format::eRGBA16Float;
    if (!RI::formatSupported(f.device, format, RI::TextureUsage::eRenderTarget, 4)) {
        return;
    }
    RI::Texture msaa = placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .samples = 4,
                                                      .format = format, .usage = RI::TextureUsage::eRenderTarget});
    RI::Texture resolved = placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .format = format,
                                                          .usage = RI::TextureUsage::eRenderTarget |
                                                                   RI::TextureUsage::eSampled});
    std::vector<std::byte> vs, ps;
    RI::Pipeline pipeline = drawPipeline(f.device, format, RI::Format::eUndefined, 4, false, vs, ps);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = msaa.view, .load = RI::LoadOp::eClear,
                                              .resolveTo = resolved.view}}});
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.5f, 0.5f, 0.5f, 0.5f}, .depth = 0.5f});
    RI::cmdDraw(f.cmd, 3);
    RI::cmdEndRendering(f.cmd);
    std::vector<float> texels = f.readBack(resolved.view);
    EQ_CHECK(all(texels, 0, 0.5f) && all(texels, 3, 0.5f));

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = msaa.view, .load = RI::LoadOp::eClear,
                                              .clear = {.color = {0.25f, 0.25f, 0.25f, 0.25f}}}}});
    RI::cmdEndRendering(f.cmd);
    RI::cmdBarrier(f.cmd, RI::Stage::eRenderTarget, RI::Stage::eRenderTarget);
    RI::cmdResolve(f.cmd, msaa.view, resolved.view, RI::ResolveMode::eAverage);
    texels = f.readBack(resolved.view);
    EQ_CHECK(all(texels, 0, 0.25f) && all(texels, 3, 0.25f));

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::destroyPipeline(f.device, pipeline);
    RI::destroyTexture(f.device, resolved);
    RI::destroyTexture(f.device, msaa);
}

// Depth resolves (P76) with a mode the device reports, at cmdEndRendering and
// with cmdResolve.
EQ_TEST(texture, resolve_depth) {
    Fixture f;
    const RI::Format format = RI::Format::eD32Float;
    RI::ResolveMode mode = RI::ResolveMode::eMin;
    if ((f.device.limits.depthResolveModes & RI::resolveModeBit(mode)) == 0) {
        mode = RI::ResolveMode::eMax;
    }
    if ((f.device.limits.depthResolveModes & RI::resolveModeBit(mode)) == 0 ||
        !RI::formatSupported(f.device, format, RI::TextureUsage::eDepthStencil, 4)) {
        return;
    }
    RI::Texture msaa = placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .samples = 4,
                                                      .format = format, .usage = RI::TextureUsage::eDepthStencil});
    RI::Texture resolved = placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .format = format,
                                                          .usage = RI::TextureUsage::eDepthStencil |
                                                                   RI::TextureUsage::eSampled});
    RI::cmdBeginRendering(f.cmd, {.depth = RI::DepthAttachment{.view = msaa.view,
                                                                .load = RI::LoadOp::eClear,
                                                                .clear = {.depth = 0.75f},
                                                                .resolveTo = resolved.view,
                                                                .resolveMode = mode}});
    RI::cmdEndRendering(f.cmd);
    std::vector<float> texels = f.readBack(resolved.view);
    EQ_CHECK(all(texels, 0, 0.75f));

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::cmdBeginRendering(f.cmd, {.depth = RI::DepthAttachment{.view = msaa.view, .load = RI::LoadOp::eClear,
                                                                .clear = {.depth = 0.25f}}});
    RI::cmdEndRendering(f.cmd);
    RI::cmdBarrier(f.cmd, RI::Stage::eDepthStencil, RI::Stage::eDepthStencil);
    RI::cmdResolve(f.cmd, msaa.view, resolved.view, mode);
    texels = f.readBack(resolved.view);
    EQ_CHECK(all(texels, 0, 0.25f));

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::destroyTexture(f.device, resolved);
    RI::destroyTexture(f.device, msaa);
}

namespace {

// Additive draws of the full-screen triangle into a 16-bit float target.
struct AdditiveTarget {
    RI::Texture target;
    std::vector<std::byte> vs, ps;
    RI::Pipeline pipeline;

    explicit AdditiveTarget(Fixture &f) {
        target = placeTexture(f.device, f.gpu,
                              {.width = kSize, .height = kSize, .format = RI::Format::eRGBA16Float,
                               .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
        vs = loadShader("fullscreen_vs");
        ps = loadShader("solid_ps");
        const RI::BlendState additive{.enable = true,
                                      .srcColor = RI::BlendFactor::eOne,
                                      .dstColor = RI::BlendFactor::eOne,
                                      .srcAlpha = RI::BlendFactor::eOne,
                                      .dstAlpha = RI::BlendFactor::eOne};
        pipeline = RI::createGraphicsPipeline(
            f.device, {.vertex = vs, .pixel = ps, .colorFormats = {RI::Format::eRGBA16Float}, .blend = {additive}});
    }
    void destroy(Fixture &f) {
        RI::destroyPipeline(f.device, pipeline);
        RI::destroyTexture(f.device, target);
    }
};

} // namespace

// Indirect Level 1 (P48): indexed draws cut short by a GPU count, plain draws,
// and a dispatch, all from buffer arguments.
EQ_TEST(texture, indirect_level1) {
    Fixture f;
    AdditiveTarget t(f);
    Arena upload{.memory = RI::allocateMemory(f.device, RI::MemoryKind::eUpload, 1 << 16)};
    RI::Buffer indices = placeBuffer(f.device, upload, {.size = 16, .usage = RI::BufferUsage::eIndex});
    const uint16 indexValues[] = {0, 1, 2, 0};
    std::memcpy(indices.mapped, indexValues, sizeof(indexValues));
    RI::Buffer args = placeBuffer(f.device, upload, {.size = 256, .usage = RI::BufferUsage::eIndirect});
    auto *bytes = static_cast<std::byte *>(args.mapped);
    const RI::DrawIndexedIndirectArgs indexed{.indexCount = 3, .instanceCount = 1};
    std::memcpy(bytes, &indexed, sizeof(indexed));                  // record 0
    std::memcpy(bytes + sizeof(indexed), &indexed, sizeof(indexed)); // record 1, cut by the count
    const RI::DrawIndirectArgs plain{.vertexCount = 3, .instanceCount = 1};
    std::memcpy(bytes + 64, &plain, sizeof(plain));
    std::memcpy(bytes + 64 + sizeof(plain), &plain, sizeof(plain));
    const RI::DispatchIndirectArgs groups{kSize / 8, kSize / 8, 1};
    std::memcpy(bytes + 128, &groups, sizeof(groups));
    RI::Buffer count = placeBuffer(f.device, upload, {.size = 4, .usage = RI::BufferUsage::eIndirect});
    *static_cast<uint32 *>(count.mapped) = 1;

    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = t.target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(f.cmd, t.pipeline);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.25f, 0.25f, 0.25f, 0.25f}, .depth = 0.5f});
    RI::cmdSetIndexBuffer(f.cmd, indices, 0, RI::IndexFormat::eUint16);
    RI::cmdExecuteIndirect(f.cmd, RI::IndirectKind::eDrawIndexed, args, 0, 2, &count, 0); // one draw: +0.25
    RI::cmdExecuteIndirect(f.cmd, RI::IndirectKind::eDraw, args, 64, 2);                  // two draws: +0.5
    RI::cmdEndRendering(f.cmd);
    std::vector<float> texels = f.readBack(t.target.view);
    EQ_CHECK(all(texels, 0, 0.75f));

    // Dispatch: the storage-write shader fills a texture.
    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::Texture storage = placeTexture(f.device, f.gpu,
                                       {.width = kSize, .height = kSize, .format = RI::Format::eRGBA16Float,
                                        .usage = RI::TextureUsage::eStorage | RI::TextureUsage::eSampled});
    RI::writeDescriptors(f.device, f.heap, {{.slot = 2, .descriptor = RI::ReadWriteView{.texture = storage.view}}});
    const std::vector<std::byte> shader = loadShader("write_storage");
    RI::Pipeline write = RI::createComputePipeline(f.device, {.shader = shader});
    RI::cmdSetPipeline(f.cmd, write);
    RI::cmdSetRootData(f.cmd, WriteRoot{.textureSlot = 2, .width = kSize, .height = kSize, .value = 3.0f});
    RI::cmdExecuteIndirect(f.cmd, RI::IndirectKind::eDispatch, args, 128, 1);
    texels = f.readBack(storage.view);
    EQ_CHECK(all(texels, 2, 3.0f));

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::destroyPipeline(f.device, write);
    RI::destroyTexture(f.device, storage);
    RI::destroyBuffer(f.device, count);
    RI::destroyBuffer(f.device, args);
    RI::destroyBuffer(f.device, indices);
    RI::freeMemory(f.device, upload.memory);
    t.destroy(f);
}

// Indirect Level 2 (P48): each record sets the root data, then draws or
// dispatches; the GPU count stops before the third record.
EQ_TEST(texture, indirect_level2) {
    Fixture f;
    if (!f.device.limits.indirectRootData) {
        return;
    }
    AdditiveTarget t(f);
    Arena upload{.memory = RI::allocateMemory(f.device, RI::MemoryKind::eUpload, 1 << 16)};
    RI::Buffer indices = placeBuffer(f.device, upload, {.size = 16, .usage = RI::BufferUsage::eIndex});
    const uint16 indexValues[] = {0, 1, 2, 0};
    std::memcpy(indices.mapped, indexValues, sizeof(indexValues));
    struct DrawRecord {
        DrawRoot root;
        RI::DrawIndexedIndirectArgs args;
    };
    static_assert(sizeof(DrawRecord) == RI::indirectStride(RI::IndirectKind::eDrawIndexed, sizeof(DrawRoot)));
    RI::Buffer records = placeBuffer(f.device, upload, {.size = 3 * sizeof(DrawRecord), .usage = RI::BufferUsage::eIndirect});
    const RI::DrawIndexedIndirectArgs draw{.indexCount = 3, .instanceCount = 1};
    const DrawRecord list[3] = {{{.color = {0.25f, 0.0f, 0.0f, 0.0f}, .depth = 0.5f}, draw},
                                {{.color = {0.0f, 0.5f, 0.0f, 0.0f}, .depth = 0.5f}, draw},
                                {{.color = {0.0f, 0.0f, 1.0f, 0.0f}, .depth = 0.5f}, draw}};
    std::memcpy(records.mapped, list, sizeof(list));
    RI::Buffer count = placeBuffer(f.device, upload, {.size = 4, .usage = RI::BufferUsage::eIndirect});
    *static_cast<uint32 *>(count.mapped) = 2;

    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = t.target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(f.cmd, t.pipeline);
    RI::cmdSetIndexBuffer(f.cmd, indices, 0, RI::IndexFormat::eUint16);
    RI::cmdExecuteIndirectWithRootData(f.cmd, RI::IndirectKind::eDrawIndexed, records, 0, 3, &count, 0);
    RI::cmdEndRendering(f.cmd);
    std::vector<float> texels = f.readBack(t.target.view);
    EQ_CHECK(all(texels, 0, 0.25f) && all(texels, 1, 0.5f) && all(texels, 2, 0.0f));

    // A dispatch whose root data comes from the record.
    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::Texture storage = placeTexture(f.device, f.gpu,
                                       {.width = kSize, .height = kSize, .format = RI::Format::eRGBA16Float,
                                        .usage = RI::TextureUsage::eStorage | RI::TextureUsage::eSampled});
    RI::writeDescriptors(f.device, f.heap, {{.slot = 2, .descriptor = RI::ReadWriteView{.texture = storage.view}}});
    const std::vector<std::byte> shader = loadShader("write_storage");
    RI::Pipeline write = RI::createComputePipeline(f.device, {.shader = shader});
    struct DispatchRecord {
        WriteRoot root;
        RI::DispatchIndirectArgs args;
    };
    RI::Buffer dispatches = placeBuffer(f.device, upload, {.size = sizeof(DispatchRecord), .usage = RI::BufferUsage::eIndirect});
    const DispatchRecord record{{.textureSlot = 2, .width = kSize, .height = kSize, .value = 5.0f}, {kSize / 8, kSize / 8, 1}};
    std::memcpy(dispatches.mapped, &record, sizeof(record));
    RI::cmdSetPipeline(f.cmd, write);
    RI::cmdExecuteIndirectWithRootData(f.cmd, RI::IndirectKind::eDispatch, dispatches, 0, 1);
    texels = f.readBack(storage.view);
    EQ_CHECK(all(texels, 2, 5.0f));

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::destroyPipeline(f.device, write);
    RI::destroyTexture(f.device, storage);
    RI::destroyBuffer(f.device, dispatches);
    RI::destroyBuffer(f.device, count);
    RI::destroyBuffer(f.device, records);
    RI::destroyBuffer(f.device, indices);
    RI::freeMemory(f.device, upload.memory);
    t.destroy(f);
}

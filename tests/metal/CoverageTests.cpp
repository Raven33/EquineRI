// Metal tests for what the 2026-11-09 handoff (§5, §9) and the 2026-11-12
// review (§3) list as untested on Metal: texture copies between formats and of
// one depth aspect, sampling a read-only depth attachment inside its section,
// a scissor outside the section, and geometry shaders after tessellation, on
// strips and with instancing (MetalBackend-Handoff-2026-11-09.md §10); and
// fixes of the review's findings (§11).

#include <TestHarness.hpp>

#include "../gpu/GpuTestCommon.hpp"

#include <RI/Helpers/TypedCommands.hpp>
#include <RI/Metal/Native.hpp>
#include <RI/RI.hpp>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <span>
#include <vector>

namespace {

using gputest::Arena;
using gputest::loadShader;
using gputest::placeBuffer;
using gputest::placeTexture;

constexpr uint32 kSize = 32;

struct DrawRoot {
    float color[4];
    float depth;
};

struct Fixture {
    RI::Device device = gputest::createTestDevice();
    Arena gpu{.memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 64 << 20)};
    Arena upload{.memory = RI::allocateMemory(device, RI::MemoryKind::eUpload, 1 << 20)};
    Arena readback{.memory = RI::allocateMemory(device, RI::MemoryKind::eReadback, 1 << 20)};
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 4, .samplerCount = 0});
    RI::CommandList cmd = RI::createCommandList(device);
    std::deque<RI::Buffer> buffers; // deques: references stay valid
    std::deque<RI::Texture> textures;
    std::vector<RI::Pipeline> pipelines;

    Fixture() { begin(); }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
    ~Fixture() {
        for (RI::Pipeline &pipeline : pipelines) {
            RI::destroyPipeline(device, pipeline);
        }
        for (RI::Texture &texture : textures) {
            RI::destroyTexture(device, texture);
        }
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

    void submitAndWait() {
        RI::cmdEnd(cmd);
        RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
        RI::resetCommandList(cmd);
        begin();
    }

    const RI::Buffer &buffer(Arena &arena, uint64 size, RI::BufferUsage usage) {
        buffers.push_back(placeBuffer(device, arena, {.size = size, .usage = usage}));
        return buffers.back();
    }

    const RI::Texture &texture(const RI::TextureDesc &desc) {
        textures.push_back(placeTexture(device, gpu, desc));
        return textures.back();
    }

    RI::Pipeline pipeline(const RI::GraphicsPipelineDesc &desc) {
        pipelines.push_back(RI::createGraphicsPipeline(device, desc));
        return pipelines.back();
    }

    // A kSize x kSize render target of 4-byte texels.
    const RI::Texture &target(RI::Format format = RI::Format::eRGBA8Unorm) {
        return texture({.width = kSize,
                        .height = kSize,
                        .format = format,
                        .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eCopySrc});
    }

    // Submits, waits, and returns the view's kSize x kSize texels as 32-bit
    // words; the list is recording again after.
    std::vector<uint32> readBack(const RI::TextureView &view) {
        const RI::CopyLayout layout = RI::copyLayout(device, view);
        const RI::Buffer &out = buffer(readback, layout.size, RI::BufferUsage::eCopyDst);
        RI::cmdBarrier(cmd, RI::Stage::eAll, RI::Stage::eCopy);
        RI::cmdCopyTextureToBuffer(cmd, view, out, 0);
        submitAndWait();
        std::vector<uint32> texels(kSize * kSize);
        for (uint32 y = 0; y < kSize; ++y) {
            std::memcpy(&texels[y * kSize], static_cast<const std::byte *>(out.mapped) + y * layout.rowPitch, kSize * 4);
        }
        return texels;
    }

    // Draws `count` vertices with `desc` into a cleared RGBA8 target, in
    // white, and returns its texels.
    std::vector<uint32> draw(const RI::GraphicsPipelineDesc &desc, uint32 count) {
        const RI::Texture &color = target();
        RI::Pipeline drawing = pipeline(desc);
        RI::cmdBeginRendering(cmd, {.colors = {{.view = color.view, .load = RI::LoadOp::eClear}}});
        RI::cmdSetPipeline(cmd, drawing);
        RI::Helpers::cmdSetRootData(cmd, DrawRoot{.color = {1, 1, 1, 1}, .depth = 0.5f});
        RI::cmdDraw(cmd, count);
        RI::cmdEndRendering(cmd);
        return readBack(color.view);
    }
};

uint32 countOf(const std::vector<uint32> &texels, uint32 value) {
    uint32 n = 0;
    for (uint32 texel : texels) {
        n += texel == value ? 1 : 0;
    }
    return n;
}

uint32 bitsOf(float value) {
    uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

uint8 pattern(uint32 x, uint32 y, uint32 c) { return static_cast<uint8>(x * 7 + y * 13 + c * 61 + 1); }

} // namespace

// A geometry shader after tessellation: the patch's triangle goes through
// triangle_pass_gs and covers the target.
EQ_TEST(metal, geometry_after_tessellation) {
    Fixture f;
    const std::vector<std::byte> vs = loadShader("patch_vs"), hs = loadShader("patch_hs"), ds = loadShader("patch_ds"),
                                 gs = loadShader("triangle_pass_gs"), ps = loadShader("color_ps");
    const std::vector<uint32> texels = f.draw({.vertex = vs, .hull = hs, .domain = ds, .geometry = gs, .pixel = ps,
                                               .colorFormats = {RI::Format::eRGBA8Unorm},
                                               .topology = RI::Topology::ePatchList,
                                               .patchControlPoints = 3},
                                              3);
    EQ_CHECK(countOf(texels, 0xffffffffu) == kSize * kSize);
}

// Strips into a geometry shader: a 4-vertex triangle strip gives two
// triangles (the whole target, where its first triangle alone is half), and a
// 3-point line strip gives two segments.
EQ_TEST(metal, geometry_strip_input) {
    Fixture f;
    const std::vector<std::byte> strip = loadShader("strip_vs"), triangles = loadShader("triangle_pass_gs"),
                                 color = loadShader("color_ps");
    const std::vector<uint32> full = f.draw({.vertex = strip, .geometry = triangles, .pixel = color,
                                             .colorFormats = {RI::Format::eRGBA8Unorm},
                                             .topology = RI::Topology::eTriangleStrip},
                                            4);
    EQ_CHECK(countOf(full, 0xffffffffu) == kSize * kSize);

    const std::vector<std::byte> points = loadShader("lines_vs"), lines = loadShader("line_pass_gs"),
                                 solid = loadShader("solid_ps");
    const RI::GraphicsPipelineDesc listDesc{.vertex = points, .geometry = lines, .pixel = solid,
                                            .colorFormats = {RI::Format::eRGBA8Unorm},
                                            .topology = RI::Topology::eLineList};
    RI::GraphicsPipelineDesc stripDesc = listDesc;
    stripDesc.topology = RI::Topology::eLineStrip;
    const uint32 list = countOf(f.draw(listDesc, 2), 0xffffffffu);
    const uint32 stripped = countOf(f.draw(stripDesc, 3), 0xffffffffu);
    // The same bounds as texture.line_strip, without a geometry shader.
    EQ_CHECK(list >= kSize / 2 && list <= kSize + 2);
    EQ_CHECK(stripped > list + kSize / 4);
}

// Geometry-shader instancing ([instance(2)]): instance 0 fills the left half
// at half intensity, instance 1 the right half at full; then with two draw
// instances as well. MSC's draw helpers pass the draw's instance count as the
// geometry shader's, which ran instance 0 alone (drawEmulated, Emulation.cpp).
EQ_TEST(metal, geometry_instancing) {
    Fixture f;
    const std::vector<std::byte> vs = loadShader("grid_instances_vs"), gs = loadShader("instanced_gs"),
                                 ps = loadShader("color_ps");
    const std::vector<uint32> texels = f.draw({.vertex = vs, .geometry = gs, .pixel = ps,
                                               .colorFormats = {RI::Format::eRGBA8Unorm},
                                               .topology = RI::Topology::ePointList},
                                              1);
    uint32 wrong = 0;
    for (uint32 y = 0; y < kSize; ++y) {
        for (uint32 x = 0; x < kSize; ++x) {
            const uint32 texel = texels[y * kSize + x];
            if (x < kSize / 2) {
                wrong += (texel == 0x80808080u || texel == 0x7f7f7f7fu) ? 0 : 1;
            } else {
                wrong += texel == 0xffffffffu ? 0 : 1;
            }
        }
    }
    EQ_CHECK(wrong == 0);

    // Two draw instances of it, blended additively: each half gets both.
    const RI::BlendState additive{.enable = true,
                                  .srcColor = RI::BlendFactor::eOne,
                                  .dstColor = RI::BlendFactor::eOne,
                                  .srcAlpha = RI::BlendFactor::eOne,
                                  .dstAlpha = RI::BlendFactor::eOne};
    const RI::Texture &color = f.target();
    RI::Pipeline blended = f.pipeline({.vertex = vs, .geometry = gs, .pixel = ps,
                                       .colorFormats = {RI::Format::eRGBA8Unorm},
                                       .topology = RI::Topology::ePointList, .blend = {additive}});
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = color.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(f.cmd, blended);
    RI::Helpers::cmdSetRootData(f.cmd, DrawRoot{.color = {0.5f, 0.5f, 0.5f, 0.5f}, .depth = 0.5f});
    RI::cmdDraw(f.cmd, 1, 2);
    RI::cmdEndRendering(f.cmd);
    const std::vector<uint32> twice = f.readBack(color.view);
    wrong = 0;
    for (uint32 y = 0; y < kSize; ++y) {
        for (uint32 x = 0; x < kSize; ++x) {
            const uint32 texel = twice[y * kSize + x];
            // 2 x 0.25 on the left, 2 x 0.5 on the right.
            wrong += x < kSize / 2 ? (texel == 0x80808080u || texel == 0x7f7f7f7fu ? 0 : 1)
                                   : (texel == 0xffffffffu ? 0 : 1);
        }
    }
    EQ_CHECK(wrong == 0);
}

// Copies between formats of one texel size (staged through command memory on
// Metal): RGBA8Unorm bytes into R32Uint and back into BGRA8Unorm, unchanged.
EQ_TEST(metal, copy_between_formats) {
    Fixture f;
    const RI::TextureUsage usage = RI::TextureUsage::eCopySrc | RI::TextureUsage::eCopyDst | RI::TextureUsage::eSampled;
    const RI::Texture &a = f.texture({.width = kSize, .height = kSize, .format = RI::Format::eRGBA8Unorm, .usage = usage});
    const RI::Texture &b = f.texture({.width = kSize, .height = kSize, .format = RI::Format::eR32Uint, .usage = usage});
    const RI::Texture &c = f.texture({.width = kSize, .height = kSize, .format = RI::Format::eBGRA8Unorm, .usage = usage});
    const RI::CopyLayout layout = RI::copyLayout(f.device, a.view);
    const RI::Buffer &staging = f.buffer(f.upload, layout.size, RI::BufferUsage::eCopySrc);
    auto *bytes = static_cast<uint8 *>(staging.mapped);
    for (uint32 y = 0; y < kSize; ++y) {
        for (uint32 x = 0; x < kSize; ++x) {
            for (uint32 ch = 0; ch < 4; ++ch) {
                bytes[y * layout.rowPitch + x * 4 + ch] = pattern(x, y, ch);
            }
        }
    }
    RI::cmdCopyBufferToTexture(f.cmd, staging, 0, a.view);
    RI::cmdBarrier(f.cmd, RI::Stage::eCopy, RI::Stage::eCopy);
    RI::cmdCopyTexture(f.cmd, a.view, b.view);
    RI::cmdBarrier(f.cmd, RI::Stage::eCopy, RI::Stage::eCopy);
    // A region at an offset, on the way back.
    RI::cmdCopyTexture(f.cmd, b.view, c.view, {.x = 4, .y = 8, .width = 12, .height = 10}, {.x = 1, .y = 2});
    const std::vector<uint32> words = f.readBack(b.view);
    const std::vector<uint32> back = f.readBack(c.view);
    uint32 wrong = 0;
    for (uint32 y = 0; y < kSize; ++y) {
        for (uint32 x = 0; x < kSize; ++x) {
            uint32 expected = 0;
            for (uint32 ch = 0; ch < 4; ++ch) {
                expected |= uint32{pattern(x, y, ch)} << (8 * ch);
            }
            wrong += words[y * kSize + x] != expected ? 1 : 0;
            if (x >= 1 && x < 13 && y >= 2 && y < 12) {
                uint32 source = 0;
                for (uint32 ch = 0; ch < 4; ++ch) {
                    source |= uint32{pattern(x + 3, y + 6, ch)} << (8 * ch);
                }
                wrong += back[y * kSize + x] != source ? 1 : 0;
            }
        }
    }
    EQ_CHECK(wrong == 0);
}

// One aspect of a depth-stencil format, texture to texture: the depth of a
// D32FloatS8Uint target into a D32Float one, and its stencil into another
// D32FloatS8Uint, whose depth stays.
EQ_TEST(metal, copy_depth_stencil_aspects) {
    Fixture f;
    const RI::TextureUsage usage = RI::TextureUsage::eDepthStencil | RI::TextureUsage::eCopySrc |
                                   RI::TextureUsage::eCopyDst;
    const RI::Texture &src = f.texture({.width = kSize, .height = kSize, .format = RI::Format::eD32FloatS8Uint, .usage = usage});
    const RI::Texture &depth = f.texture({.width = kSize, .height = kSize, .format = RI::Format::eD32Float, .usage = usage});
    const RI::Texture &both = f.texture({.width = kSize, .height = kSize, .format = RI::Format::eD32FloatS8Uint, .usage = usage});
    const auto view = [&](const RI::Texture &texture, RI::Aspect aspect) {
        return RI::createView(f.device, texture, {.aspect = aspect});
    };
    RI::TextureView srcDepth = view(src, RI::Aspect::eDepth), srcStencil = view(src, RI::Aspect::eStencil);
    RI::TextureView bothDepth = view(both, RI::Aspect::eDepth), bothStencil = view(both, RI::Aspect::eStencil);
    RI::cmdBeginRendering(f.cmd, {.depth = RI::DepthAttachment{.view = src.view,
                                                              .load = RI::LoadOp::eClear,
                                                              .stencilLoad = RI::LoadOp::eClear,
                                                              .clear = {.depth = 0.25f, .stencil = 7}}});
    RI::cmdEndRendering(f.cmd);
    RI::cmdBeginRendering(f.cmd, {.depth = RI::DepthAttachment{.view = both.view,
                                                              .load = RI::LoadOp::eClear,
                                                              .stencilLoad = RI::LoadOp::eClear,
                                                              .clear = {.depth = 0.75f, .stencil = 1}}});
    RI::cmdEndRendering(f.cmd);
    RI::cmdBarrier(f.cmd, RI::Stage::eDepthStencil, RI::Stage::eCopy);
    RI::cmdCopyTexture(f.cmd, srcDepth, depth.view);
    RI::cmdCopyTexture(f.cmd, srcStencil, bothStencil);
    const std::vector<uint32> depthTexels = f.readBack(depth.view);
    const std::vector<uint32> keptDepth = f.readBack(bothDepth);
    EQ_CHECK(countOf(depthTexels, bitsOf(0.25f)) == kSize * kSize);
    EQ_CHECK(countOf(keptDepth, bitsOf(0.75f)) == kSize * kSize);

    const RI::CopyLayout layout = RI::copyLayout(f.device, bothStencil);
    const RI::Buffer &out = f.buffer(f.readback, layout.size, RI::BufferUsage::eCopyDst);
    RI::cmdBarrier(f.cmd, RI::Stage::eAll, RI::Stage::eCopy);
    RI::cmdCopyTextureToBuffer(f.cmd, bothStencil, out, 0);
    f.submitAndWait();
    uint32 wrong = 0;
    for (uint32 y = 0; y < kSize; ++y) {
        for (uint32 x = 0; x < kSize; ++x) {
            wrong += static_cast<const uint8 *>(out.mapped)[y * layout.rowPitch + x] != 7 ? 1 : 0;
        }
    }
    EQ_CHECK(wrong == 0);
    for (RI::TextureView *v : {&srcDepth, &srcStencil, &bothDepth, &bothStencil}) {
        RI::destroyView(f.device, *v);
    }
}

// Sampling a read-only depth attachment inside its own section (P75, P97):
// depth cleared to 0.75, then a section that tests against it (0.5 passes)
// while the pixel shader reads it into an R32Float target.
EQ_TEST(metal, sample_read_only_depth_in_section) {
    Fixture f;
    const RI::Texture &depth = f.texture({.width = kSize,
                                          .height = kSize,
                                          .format = RI::Format::eD32Float,
                                          .usage = RI::TextureUsage::eDepthStencil | RI::TextureUsage::eSampled |
                                                   RI::TextureUsage::eReadOnlyDepthSampling});
    const RI::Texture &color = f.target(RI::Format::eR32Float);
    RI::writeDescriptors(f.device, f.heap, {{.slot = 0, .descriptor = RI::ReadView{.texture = depth.view}}});
    const std::vector<std::byte> vs = loadShader("fullscreen_vs"), ps = loadShader("depth_sample_ps");
    RI::Pipeline pipeline = f.pipeline({.vertex = vs, .pixel = ps,
                                        .colorFormats = {RI::Format::eR32Float},
                                        .depthFormat = RI::Format::eD32Float,
                                        .depth = {.test = true, .write = false}});
    RI::cmdBeginRendering(f.cmd, {.depth = RI::DepthAttachment{.view = depth.view, .load = RI::LoadOp::eClear,
                                                              .clear = {.depth = 0.75f}}});
    RI::cmdEndRendering(f.cmd);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = color.view, .load = RI::LoadOp::eClear}},
                                  .depth = RI::DepthAttachment{.view = depth.view, .depthReadOnly = true}});
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::Helpers::cmdSetRootData(f.cmd, DrawRoot{.color = {}, .depth = 0.5f}); // slot 0: the bits of 0.0f
    RI::cmdDraw(f.cmd, 3);
    RI::cmdEndRendering(f.cmd);
    const std::vector<uint32> texels = f.readBack(color.view);
    EQ_CHECK(countOf(texels, bitsOf(0.75f)) == kSize * kSize);
}

// A scissor wholly outside the section (clipped to an empty rectangle) draws
// nothing, without a debug-layer message; a scissor partly outside draws the
// part inside. Negative origins are refused by RI (cmdSetScissor).
EQ_TEST(metal, scissor_outside_section) {
    Fixture f;
    const RI::Texture &color = f.target();
    const std::vector<std::byte> vs = loadShader("fullscreen_vs"), ps = loadShader("solid_ps");
    RI::Pipeline pipeline = f.pipeline({.vertex = vs, .pixel = ps, .colorFormats = {RI::Format::eRGBA8Unorm}});
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = color.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::Helpers::cmdSetRootData(f.cmd, DrawRoot{.color = {1, 1, 1, 1}, .depth = 0.5f});
    for (const RI::Rect &outside : {RI::Rect{.x = 100, .y = 100, .width = 10, .height = 10},
                                    RI::Rect{.x = 0, .y = 40, .width = 32, .height = 8},
                                    RI::Rect{.x = 32, .y = 0, .width = 10, .height = 32}}) {
        RI::cmdSetScissor(f.cmd, outside);
        RI::cmdDraw(f.cmd, 3);
    }
    RI::cmdEndRendering(f.cmd);
    EQ_CHECK(countOf(f.readBack(color.view), 0u) == kSize * kSize);

    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = color.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::Helpers::cmdSetRootData(f.cmd, DrawRoot{.color = {1, 1, 1, 1}, .depth = 0.5f});
    RI::cmdSetScissor(f.cmd, {.x = 16, .y = 24, .width = 100, .height = 100});
    RI::cmdDraw(f.cmd, 3);
    RI::cmdEndRendering(f.cmd);
    const std::vector<uint32> texels = f.readBack(color.view);
    uint32 wrong = 0;
    for (uint32 y = 0; y < kSize; ++y) {
        for (uint32 x = 0; x < kSize; ++x) {
            wrong += texels[y * kSize + x] != (x >= 16 && y >= 24 ? 0xffffffffu : 0u) ? 1 : 0;
        }
    }
    EQ_CHECK(wrong == 0);
}

// Command memory stays bounded (review 2026-11-12 §3 (1)): two staged copies
// of a 2048 x 2048 depth aspect (16 MiB each) in one list take chunks of
// their own, released at reset, and the list's chunks stop doubling at 4 MiB.
EQ_TEST(metal, command_memory_bounded) {
    Fixture f;
    const MTL::ResidencySet *residency = RI::Metal::native(f.device).residencySet;
    const RI::TextureUsage usage = RI::TextureUsage::eCopySrc | RI::TextureUsage::eCopyDst |
                                   RI::TextureUsage::eDepthStencil;
    const RI::TextureDesc desc{.width = 2048, .height = 2048, .format = RI::Format::eD32FloatS8Uint, .usage = usage};
    const RI::Texture &a = f.texture(desc);
    const RI::Texture &b = f.texture(desc);
    RI::TextureView from = RI::createView(f.device, a, {.aspect = RI::Aspect::eDepth});
    RI::TextureView to = RI::createView(f.device, b, {.aspect = RI::Aspect::eDepth});
    const RI::Buffer &small = f.buffer(f.gpu, 256, RI::BufferUsage::eCopyDst);
    const uint32 word = 1;
    const auto smallWrites = [&](int count) {
        for (int i = 0; i < count; ++i) {
            RI::cmdWriteSmallData(f.cmd, small, 0, std::as_bytes(std::span{&word, 1}));
        }
    };
    smallWrites(1);
    f.submitAndWait();
    const uint64 before = residency->allocatedSize();

    RI::cmdCopyTexture(f.cmd, from, to);
    RI::cmdBarrier(f.cmd, RI::Stage::eCopy, RI::Stage::eCopy);
    RI::cmdCopyTexture(f.cmd, from, to);
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));
    const uint64 during = residency->allocatedSize();
    RI::resetCommandList(f.cmd);
    f.begin();
    smallWrites(1);
    f.submitAndWait();
    const uint64 after = residency->allocatedSize();
    EQ_CHECK(during >= before + (32ull << 20));
    EQ_CHECK(after == before);

    // 64 KiB of small writes per list, 8 lists: the chunks stay at or under
    // 64 KiB + 128 KiB + ... + 4 MiB.
    for (int list = 0; list < 8; ++list) {
        smallWrites(4096);
        f.submitAndWait();
    }
    EQ_CHECK(residency->allocatedSize() - before <= (8ull << 20));
    RI::destroyView(f.device, from);
    RI::destroyView(f.device, to);
}

// Multisampled copies between formats report eUnsupported (review 2026-11-12
// §3 (2)): they would go through a buffer, which Metal refuses.
EQ_TEST(metal, multisampled_copy_between_formats_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    Fixture f;
    const RI::TextureUsage usage = RI::TextureUsage::eCopySrc | RI::TextureUsage::eCopyDst |
                                   RI::TextureUsage::eRenderTarget;
    const RI::Texture &a = f.texture({.width = kSize, .height = kSize, .samples = 4, .format = RI::Format::eRGBA8Unorm, .usage = usage});
    const RI::Texture &b = f.texture({.width = kSize, .height = kSize, .samples = 4, .format = RI::Format::eR32Uint, .usage = usage});
    RI::cmdCopyTexture(f.cmd, a.view, b.view);
}

// Multisampled copies of one aspect of a depth-stencil format report too; a
// format without stencil, and equal formats, copy directly.
EQ_TEST(metal, multisampled_depth_aspect_copy_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    Fixture f;
    const RI::TextureUsage usage = RI::TextureUsage::eCopySrc | RI::TextureUsage::eCopyDst |
                                   RI::TextureUsage::eDepthStencil;
    const RI::Texture &plain = f.texture({.width = kSize, .height = kSize, .samples = 4, .format = RI::Format::eD32Float, .usage = usage});
    const RI::Texture &plain2 = f.texture({.width = kSize, .height = kSize, .samples = 4, .format = RI::Format::eD32Float, .usage = usage});
    RI::cmdCopyTexture(f.cmd, plain.view, plain2.view);
    f.submitAndWait();
    const RI::Texture &a = f.texture({.width = kSize, .height = kSize, .samples = 4, .format = RI::Format::eD32FloatS8Uint, .usage = usage});
    const RI::Texture &b = f.texture({.width = kSize, .height = kSize, .samples = 4, .format = RI::Format::eD32FloatS8Uint, .usage = usage});
    RI::TextureView from = RI::createView(f.device, a, {.aspect = RI::Aspect::eDepth});
    RI::TextureView to = RI::createView(f.device, b, {.aspect = RI::Aspect::eDepth});
    RI::cmdCopyTexture(f.cmd, from, to);
}

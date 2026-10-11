// L1 measurement harness (Milestone 3; docs/LayoutEvaluation.md, "Measurement
// harness"). Usage:
//   layout_bench_<backend> [--gpu nvidia|amd|<index>] [--mode <mode>] [--res 1080|2160]
//                          [--frames N] [--warmup N] [--out results.csv]
// Modes: Vulkan: unified, general, transitions, baseline; D3D12: ri, baseline.
// Prints one CSV line per pass and per frame: median, p10 and p90 in ms.

#include "Bench.hpp"

#include <RI/RI.hpp>
#if EQUINE_BACKEND_VULKAN
#include <RI/Vk/Native.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#if EQUINE_BACKEND_D3D12
#include <D3D12AgilityExports.hpp>
EQUINE_D3D12_AGILITY_SDK_EXPORTS()
#endif

namespace {

using bench::Pass;
using bench::PassKind;
using bench::Role;
using bench::Use;
using bench::Workload;

// ── Options ──────────────────────────────────────────────────────────────────

struct Options {
    std::string gpu;
    std::string mode;
    uint32 height = 1080;
    uint32 frames = 300;
    uint32 warmup = 30;
    std::string out;
    bool hostBarrier = true; // Vulkan: --host-barrier off measures P94 (6)
    bool interleave = false; // --interleave on: the mode and the baseline, frame by frame
    std::string only;        // --only <workload>: run one workload
    bool swap = false;       // --swap on: with --interleave, the baseline gets the first texture copy
};

Options parse(int argc, char **argv) {
    Options o;
    if (argc % 2 == 0) {
        std::fprintf(stderr, "usage: layout_bench [--gpu nvidia|amd|<index>] [--mode <mode>] [--res 1080|2160] "
                             "[--frames N] [--warmup N] [--out file.csv]\n");
        std::exit(1);
    }
#if EQUINE_BACKEND_VULKAN
    o.mode = "transitions";
#else
    o.mode = "ri";
#endif
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const std::string value = argv[i + 1];
        if (key == "--gpu") {
            o.gpu = value;
        } else if (key == "--mode") {
            o.mode = value;
        } else if (key == "--res") {
            o.height = static_cast<uint32>(std::stoul(value));
        } else if (key == "--frames") {
            o.frames = static_cast<uint32>(std::stoul(value));
        } else if (key == "--warmup") {
            o.warmup = static_cast<uint32>(std::stoul(value));
        } else if (key == "--out") {
            o.out = value;
        } else if (key == "--interleave") {
            o.interleave = value == "on";
        } else if (key == "--host-barrier") {
            o.hostBarrier = value != "off";
        } else if (key == "--swap") {
            o.swap = value == "on";
        } else if (key == "--only") {
            o.only = value;
        } else if (key == "--baseline-variant") {
            bench::baselineVariant = value;
        } else {
            std::fprintf(stderr, "usage: layout_bench [--gpu nvidia|amd|<index>] [--mode <mode>] [--res 1080|2160] "
                                 "[--frames N] [--warmup N] [--out file.csv]\n");
            std::exit(1);
        }
    }
    return o;
}

RI::Gpu pickGpu(const std::string &want) {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    for (const RI::Gpu &gpu : gpus) {
        if ((want == "nvidia" && gpu.vendor == RI::GpuVendor::eNvidia) ||
            (want == "amd" && gpu.vendor == RI::GpuVendor::eAMD)) {
            return gpu;
        }
    }
    const size_t index = want.empty() ? 0 : std::strtoul(want.c_str(), nullptr, 10);
    return gpus[index < gpus.size() ? index : 0];
}

std::vector<std::byte> loadShader(const char *name) {
#if EQUINE_BACKEND_VULKAN
    const std::string path = std::string(EQUINE_SHADER_DIR) + "/" + name + ".spv";
#else
    const std::string path = std::string(EQUINE_SHADER_DIR) + "/" + name + ".dxil";
#endif
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::vector<std::byte> out(bytes.size());
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

uint64 alignUp(uint64 value, uint64 alignment) { return (value + alignment - 1) / alignment * alignment; }

// ── Root data (mirrors tools/layout_bench/shaders) ──────────────────────────

struct DrawRoot {
    float depth;
    float tilt;
    uint32 seed;
    uint32 pad;
};
// No arrays: HLSL constant buffers pad each array element to 16 bytes.
struct ReadRoot {
    uint32 textureSlot, outputSlot, width, height, multisampled, pad0, pad1, pad2;
};
struct WriteRoot {
    uint32 textureSlot, width, height, seed;
};
struct LightRoot {
    uint32 albedoSlot, normalSlot, depthSlot, litSlot, width, height, pad0, pad1;
};
struct HizRoot {
    uint32 depthSlot, outputSlot, width, height;
};
struct PostRoot {
    uint32 litSlot, pad0, pad1, pad2;
};

// ── Shared objects ───────────────────────────────────────────────────────────

constexpr uint32 kOutputSlot = 0;

struct Shaders {
    std::vector<std::byte> vs, ps, ps1, postVs, postPs, read, write, light, hiz;
};

struct Context {
    Options options;
    RI::Device device;
    RI::Memory gpuMemory;
    uint64 used = 0;
    RI::Memory readbackMemory;
    RI::DescriptorHeap heap;
    RI::Buffer output; // per-tile sums of read passes
    RI::Buffer ticks;
    RI::QueryPool queries;
    Shaders shaders;
    RI::Pipeline read, write, light, hiz;
    uint32 nextSlot = 1;
    bench::Baseline *baseline = nullptr;
    bool useBaseline = false;
};

RI::Texture placeTexture(Context &c, const RI::TextureDesc &desc) {
    const RI::MemoryRequirements req = RI::textureRequirements(c.device, desc);
    const uint64 offset = alignUp(c.used, req.alignment);
    c.used = offset + req.size;
    if (c.used > c.gpuMemory.size) {
        std::fprintf(stderr, "layout_bench: GPU memory block too small\n");
        std::exit(2);
    }
    return RI::createTexture(c.device, c.gpuMemory, offset, desc);
}

bench::Texture makeTexture(Context &c, RI::Format format, uint32 width, uint32 height, uint32 samples,
                           RI::TextureUsage usage) {
    bench::Texture t;
    t.texture = placeTexture(c, {.width = width, .height = height, .samples = samples, .format = format, .usage = usage});
    t.sampledView = RI::isDepthFormat(format) ? RI::createView(c.device, t.texture, {.aspect = RI::Aspect::eDepth})
                                              : t.texture.view;
    t.sampledSlot = c.nextSlot++;
    t.storageSlot = c.nextSlot++;
    return t;
}

void writeDescriptors(Context &c, Workload &w) {
    if (c.useBaseline) {
        bench::baselineWriteDescriptors(*c.baseline, w);
        return;
    }
    for (bench::Texture &t : w.textures) {
        if ((t.texture.usage & RI::TextureUsage::eSampled) != RI::TextureUsage::eNone) {
            RI::writeDescriptors(c.device, c.heap,
                                 {{.slot = t.sampledSlot, .descriptor = RI::ReadView{.texture = t.sampledView}}});
        }
        if ((t.texture.usage & RI::TextureUsage::eStorage) != RI::TextureUsage::eNone) {
            RI::writeDescriptors(c.device, c.heap,
                                 {{.slot = t.storageSlot, .descriptor = RI::ReadWriteView{.texture = t.texture.view}}});
        }
    }
}

RI::Pipeline drawPipeline(Context &c, std::vector<RI::Format> colors, RI::Format depth, uint32 samples) {
    const std::vector<std::byte> &ps = colors.size() > 1 ? c.shaders.ps : c.shaders.ps1;
    RI::GraphicsPipelineDesc desc{.vertex = c.shaders.vs,
                                  .pixel = colors.empty() ? RI::Bytecode{} : RI::Bytecode{ps},
                                  .colorFormats = std::move(colors),
                                  .depthFormat = depth,
                                  .samples = samples};
    desc.depth = {.test = depth != RI::Format::eUndefined, .write = depth != RI::Format::eUndefined,
                  .compare = RI::CompareOp::eLessEqual};
    return RI::createGraphicsPipeline(c.device, desc);
}

// A compute pass reading texture `t` over the whole size.
Pass readPass(Context &c, const char *name, const Workload &w, uint32 t, uint32 width, uint32 height) {
    const bench::Texture &tex = w.textures[t];
    const uint32 slot = tex.sampledSlot;
    const uint32 ms = tex.texture.samples > 1 ? 1u : 0u;
    return {name, PassKind::eCompute, {{t, Role::eSampled}}, [&c, slot, width, height, ms](RI::CommandList &cmd) {
                RI::cmdSetPipeline(cmd, c.read);
                RI::cmdSetRootData(cmd, ReadRoot{slot, kOutputSlot, width, height, ms, 0, 0, 0});
                RI::cmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
            }};
}

// ── Workloads ────────────────────────────────────────────────────────────────

struct Owned {
    std::vector<RI::Pipeline> pipelines;
};

Workload renderThenSample(Context &c, Owned &owned, RI::Format format, uint32 samples, const char *name) {
    const uint32 w = c.options.height * 16 / 9;
    const uint32 h = c.options.height;
    const bool depth = RI::isDepthFormat(format);
    Workload wl{.name = name};
    wl.textures.push_back(makeTexture(
        c, format, w, h, samples,
        RI::TextureUsage::eSampled | (depth ? RI::TextureUsage::eDepthStencil : RI::TextureUsage::eRenderTarget)));
    const RI::Pipeline pipeline = depth ? drawPipeline(c, {}, format, samples) : drawPipeline(c, {format}, RI::Format::eUndefined, samples);
    owned.pipelines.push_back(pipeline);
    wl.passes.push_back({"render", PassKind::eRender, {{0, depth ? Role::eDepthTarget : Role::eColorTarget, true}},
                         [pipeline](RI::CommandList &cmd) {
                             RI::cmdSetPipeline(cmd, pipeline);
                             RI::cmdSetRootData(cmd, DrawRoot{0.1f, 1.0f, 17, 0});
                             RI::cmdDraw(cmd, 3, 8);
                         }});
    wl.passes.push_back(readPass(c, "sample", wl, 0, w, h));
    return wl;
}

Workload storageThenSample(Context &c) {
    const uint32 w = c.options.height * 16 / 9;
    const uint32 h = c.options.height;
    Workload wl{.name = "storage_sample_rgba16f"};
    wl.textures.push_back(
        makeTexture(c, RI::Format::eRGBA16Float, w, h, 1, RI::TextureUsage::eSampled | RI::TextureUsage::eStorage));
    const uint32 slot = wl.textures[0].storageSlot;
    wl.passes.push_back({"write", PassKind::eCompute, {{0, Role::eStorage, true}}, [&c, slot, w, h](RI::CommandList &cmd) {
                             RI::cmdSetPipeline(cmd, c.write);
                             RI::cmdSetRootData(cmd, WriteRoot{slot, w, h, 5});
                             RI::cmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
                         }});
    wl.passes.push_back(readPass(c, "sample", wl, 0, w, h));
    return wl;
}

Workload depthThenHiz(Context &c, Owned &owned) {
    const uint32 w = c.options.height * 16 / 9;
    const uint32 h = c.options.height;
    Workload wl{.name = "depth_hiz_d32f"};
    wl.textures.push_back(makeTexture(c, RI::Format::eD32Float, w, h, 1,
                                      RI::TextureUsage::eSampled | RI::TextureUsage::eDepthStencil));
    wl.textures.push_back(makeTexture(c, RI::Format::eR32Float, w / 2, h / 2, 1,
                                      RI::TextureUsage::eSampled | RI::TextureUsage::eStorage));
    const RI::Pipeline pipeline = drawPipeline(c, {}, RI::Format::eD32Float, 1);
    owned.pipelines.push_back(pipeline);
    wl.passes.push_back({"depth", PassKind::eRender, {{0, Role::eDepthTarget, true}}, [pipeline](RI::CommandList &cmd) {
                             RI::cmdSetPipeline(cmd, pipeline);
                             RI::cmdSetRootData(cmd, DrawRoot{0.1f, 1.0f, 3, 0});
                             RI::cmdDraw(cmd, 3, 8);
                         }});
    const uint32 depthSlot = wl.textures[0].sampledSlot;
    const uint32 outSlot = wl.textures[1].storageSlot;
    wl.passes.push_back({"hiz", PassKind::eCompute, {{0, Role::eSampled}, {1, Role::eStorage, true}},
                         [&c, depthSlot, outSlot, w, h](RI::CommandList &cmd) {
                             RI::cmdSetPipeline(cmd, c.hiz);
                             RI::cmdSetRootData(cmd, HizRoot{depthSlot, outSlot, w / 2, h / 2});
                             RI::cmdDispatch(cmd, (w / 2 + 7) / 8, (h / 2 + 7) / 8, 1);
                         }});
    return wl;
}

Workload realisticFrame(Context &c, Owned &owned) {
    const uint32 w = c.options.height * 16 / 9;
    const uint32 h = c.options.height;
    Workload wl{.name = "frame"};
    using U = RI::TextureUsage;
    wl.textures.push_back(makeTexture(c, RI::Format::eRGBA8Unorm, w, h, 1, U::eSampled | U::eRenderTarget));    // 0 albedo
    wl.textures.push_back(makeTexture(c, RI::Format::eRGBA16Float, w, h, 1, U::eSampled | U::eRenderTarget));   // 1 normal
    wl.textures.push_back(makeTexture(c, RI::Format::eD32Float, w, h, 1, U::eSampled | U::eDepthStencil));      // 2 depth
    wl.textures.push_back(makeTexture(c, RI::Format::eRGBA16Float, w, h, 1, U::eSampled | U::eStorage));        // 3 lit
    wl.textures.push_back(makeTexture(c, RI::Format::eR32Float, w / 2, h / 2, 1, U::eSampled | U::eStorage));   // 4 hiz
    wl.textures.push_back(makeTexture(c, RI::Format::eRGBA8Unorm, w, h, 1, U::eSampled | U::eRenderTarget));    // 5 output

    const RI::Pipeline gbuffer =
        drawPipeline(c, {RI::Format::eRGBA8Unorm, RI::Format::eRGBA16Float}, RI::Format::eD32Float, 1);
    const RI::Pipeline post = RI::createGraphicsPipeline(
        c.device, {.vertex = c.shaders.postVs, .pixel = c.shaders.postPs, .colorFormats = {RI::Format::eRGBA8Unorm}});
    owned.pipelines.push_back(gbuffer);
    owned.pipelines.push_back(post);

    wl.passes.push_back({"gbuffer",
                         PassKind::eRender,
                         {{0, Role::eColorTarget, true}, {1, Role::eColorTarget, true}, {2, Role::eDepthTarget, true}},
                         [gbuffer](RI::CommandList &cmd) {
                             RI::cmdSetPipeline(cmd, gbuffer);
                             RI::cmdSetRootData(cmd, DrawRoot{0.1f, 1.0f, 29, 0});
                             RI::cmdDraw(cmd, 3, 8);
                         }});
    const LightRoot light{wl.textures[0].sampledSlot, wl.textures[1].sampledSlot, wl.textures[2].sampledSlot,
                          wl.textures[3].storageSlot, w, h, 0, 0};
    wl.passes.push_back({"lighting",
                         PassKind::eCompute,
                         {{0, Role::eSampled}, {1, Role::eSampled}, {2, Role::eSampled}, {3, Role::eStorage, true}},
                         [&c, light, w, h](RI::CommandList &cmd) {
                             RI::cmdSetPipeline(cmd, c.light);
                             RI::cmdSetRootData(cmd, light);
                             RI::cmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
                         }});
    const HizRoot hiz{wl.textures[2].sampledSlot, wl.textures[4].storageSlot, w / 2, h / 2};
    wl.passes.push_back({"hiz", PassKind::eCompute, {{2, Role::eSampled}, {4, Role::eStorage, true}},
                         [&c, hiz, w, h](RI::CommandList &cmd) {
                             RI::cmdSetPipeline(cmd, c.hiz);
                             RI::cmdSetRootData(cmd, hiz);
                             RI::cmdDispatch(cmd, (w / 2 + 7) / 8, (h / 2 + 7) / 8, 1);
                         }});
    const uint32 litSlot = wl.textures[3].sampledSlot;
    wl.passes.push_back({"post", PassKind::eRender, {{5, Role::eColorTarget, true}, {3, Role::eSampled}},
                         [post, litSlot](RI::CommandList &cmd) {
                             RI::cmdSetPipeline(cmd, post);
                             RI::cmdSetRootData(cmd, PostRoot{litSlot, 0, 0, 0});
                             RI::cmdDraw(cmd, 3);
                         }});
    wl.passes.push_back(readPass(c, "present", wl, 5, w, h));
    return wl;
}

// ── Recording ────────────────────────────────────────────────────────────────

RI::Stage stageOf(const Pass &pass) {
    return pass.kind == PassKind::eRender
               ? RI::Stage::eVertex | RI::Stage::ePixel | RI::Stage::eRenderTarget | RI::Stage::eDepthStencil
               : RI::Stage::eCompute;
}

// Timing (v2): each pass writes a timestamp right before and right after its
// own work, inside its rendering section. Barriers and layout switches between
// passes (RI's at cmdBeginRendering/cmdEndRendering, the baseline's before each
// pass) fall between passes on both paths, so passes compare like with like,
// and the frame total (and "between" = frame - sum of passes) carries the
// transition cost.
struct Stamp {
    RI::QueryPool *pool;
    uint32 start;
    uint32 end;
};

// The RI path: RI rendering sections; one coarse barrier between passes.
void recordRI(Workload &w, RI::CommandList &cmd, const Pass &pass, const Pass *previous, Stamp stamp) {
    if (previous != nullptr) {
        RI::cmdBarrier(cmd, stageOf(*previous), stageOf(pass));
    }
    // A storage texture the pass overwrites: the baseline starts it from
    // UNDEFINED, and RI says the same with cmdDiscard (P69; harness v4).
    for (const Use &use : pass.uses) {
        if (use.role == Role::eStorage && use.discard) {
            RI::cmdDiscard(cmd, w.textures[use.texture].texture.view);
        }
    }
    if (pass.kind == PassKind::eCompute) {
        RI::cmdWriteTimestamp(cmd, *stamp.pool, stamp.start);
        pass.record(cmd);
        RI::cmdWriteTimestamp(cmd, *stamp.pool, stamp.end);
        return;
    }
    RI::RenderingDesc desc;
    for (const Use &use : pass.uses) {
        const RI::Texture &t = w.textures[use.texture].texture;
        const RI::LoadOp load = use.discard ? RI::LoadOp::eClear : RI::LoadOp::eLoad;
        if (use.role == Role::eColorTarget) {
            desc.colors.push_back({.view = t.view, .load = load});
        } else if (use.role == Role::eDepthTarget) {
            desc.depth = RI::DepthAttachment{.view = t.view, .load = load, .stencilLoad = load, .clear = {.depth = 1.0f}};
        }
    }
    RI::cmdBeginRendering(cmd, desc);
    RI::cmdWriteTimestamp(cmd, *stamp.pool, stamp.start);
    pass.record(cmd);
    RI::cmdWriteTimestamp(cmd, *stamp.pool, stamp.end);
    RI::cmdEndRendering(cmd);
}

void recordBaseline(Context &c, Workload &w, RI::CommandList &cmd, const Pass &pass, Stamp stamp) {
    bench::baselineBeforePass(*c.baseline, cmd, w, pass);
    if (pass.kind == PassKind::eCompute) {
        RI::cmdWriteTimestamp(cmd, *stamp.pool, stamp.start);
        pass.record(cmd);
        RI::cmdWriteTimestamp(cmd, *stamp.pool, stamp.end);
        return;
    }
    bench::baselineBeginRendering(*c.baseline, cmd, w, pass);
    RI::cmdWriteTimestamp(cmd, *stamp.pool, stamp.start);
    pass.record(cmd);
    RI::cmdWriteTimestamp(cmd, *stamp.pool, stamp.end);
    bench::baselineEndRendering(*c.baseline, cmd);
}

struct Stats {
    double median = 0, p10 = 0, p90 = 0;
};

Stats stats(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) { return v[static_cast<size_t>(q * static_cast<double>(v.size() - 1))]; };
    return {at(0.5), at(0.1), at(0.9)};
}

// One measured path: a workload instance recorded the RI way or the baseline way.
struct Path {
    Workload *workload = nullptr;
    bool baseline = false;
    std::string mode;
    std::vector<std::vector<double>> times; // per pass, frame, cpu, between
};

// Runs the paths' frames alternately (frame f records path f % paths.size()), so
// with --interleave the baseline and the RI mode see the same GPU clock and
// power states (integrated GPUs switch between them). Without it, one path.
void run(Context &c, std::vector<Path> &paths, std::ostream &out, const std::string &gpuName) {
    RI::CommandList cmd = RI::createCommandList(c.device);
    // Each workload reuses earlier memory, so its textures are discarded
    // before first use (P69), on both paths.
    RI::cmdBegin(cmd);
    for (Path &path : paths) {
        if (path.baseline) {
            bench::baselineReset(*c.baseline, *path.workload);
            bench::baselineWriteDescriptors(*c.baseline, *path.workload);
        } else {
            const bool saved = c.useBaseline;
            c.useBaseline = false;
            writeDescriptors(c, *path.workload);
            c.useBaseline = saved;
        }
        for (bench::Texture &t : path.workload->textures) {
            RI::cmdDiscard(cmd, t.texture.view);
        }
        path.times.assign(path.workload->passes.size() + 3, {});
    }
    RI::cmdEnd(cmd);
    RI::wait(c.device.graphicsQueue, RI::submit(c.device.graphicsQueue, cmd));
    RI::resetCommandList(cmd);
    const double msPerTick = 1000.0 / static_cast<double>(c.device.limits.timestampFrequency);
    const uint32 pathCount = static_cast<uint32>(paths.size());

    for (uint32 frame = 0; frame < (c.options.warmup + c.options.frames) * pathCount; ++frame) {
        Path &path = paths[frame % pathCount];
        Workload &w = *path.workload;
        const uint32 passCount = static_cast<uint32>(w.passes.size());
        if (frame > 0) {
            RI::resetCommandList(cmd);
        }
        RI::cmdBegin(cmd);
        RI::cmdSetHeap(cmd, c.heap);
        // Timestamps: 0 frame start, 1 + 2i / 2 + 2i around pass i, last frame end.
        const uint32 last = 2 * passCount + 1;
        RI::cmdWriteTimestamp(cmd, c.queries, 0);
        for (uint32 i = 0; i < passCount; ++i) {
            const Stamp stamp{&c.queries, 1 + 2 * i, 2 + 2 * i};
            if (path.baseline) {
                recordBaseline(c, w, cmd, w.passes[i], stamp);
            } else {
                recordRI(w, cmd, w.passes[i], i > 0 ? &w.passes[i - 1] : nullptr, stamp);
            }
        }
        RI::cmdWriteTimestamp(cmd, c.queries, last);
        RI::cmdResolveQueries(cmd, c.queries, 0, last + 1, c.ticks, 0);
        RI::cmdEnd(cmd);
        const auto start = std::chrono::steady_clock::now();
        RI::wait(c.device.graphicsQueue, RI::submit(c.device.graphicsQueue, cmd));
        const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        RI::resetQueries(c.device, c.queries, 0, last + 1);
        if (frame < c.options.warmup * pathCount) {
            continue;
        }
        const auto *t = static_cast<const uint64 *>(c.ticks.mapped);
        double passSum = 0.0;
        for (uint32 i = 0; i < passCount; ++i) {
            const double ms = static_cast<double>(t[2 + 2 * i] - t[1 + 2 * i]) * msPerTick;
            path.times[i].push_back(ms);
            passSum += ms;
        }
        const double frameMs = static_cast<double>(t[last] - t[0]) * msPerTick;
        path.times[passCount].push_back(frameMs);
        path.times[passCount + 1].push_back(cpuMs);
        path.times[passCount + 2].push_back(frameMs - passSum);
    }
    RI::destroyCommandList(c.device, cmd);

    for (Path &path : paths) {
        Workload &w = *path.workload;
        const uint32 passCount = static_cast<uint32>(w.passes.size());
        const auto line = [&](const std::string &pass, const Stats &s) {
#if EQUINE_BACKEND_VULKAN
            const char *backend = "vulkan";
#else
            const char *backend = "d3d12";
#endif
            char text[512];
            std::snprintf(text, sizeof(text), "%s,%s,%s,%u,%s,%s,%.4f,%.4f,%.4f\n", backend, gpuName.c_str(),
                          path.mode.c_str(), c.options.height, w.name.c_str(), pass.c_str(), s.median, s.p10, s.p90);
            out << text << std::flush;
        };
        for (uint32 i = 0; i < passCount; ++i) {
            line(w.passes[i].name, stats(path.times[i]));
        }
        line("frame", stats(path.times[passCount]));
        line("cpu_submit_wait", stats(path.times[passCount + 1]));
        line("between", stats(path.times[passCount + 2]));
    }
}

} // namespace

int main(int argc, char **argv) {
    Context c;
    c.options = parse(argc, argv);
    const RI::Gpu gpu = pickGpu(c.options.gpu);
#if EQUINE_BACKEND_VULKAN
    RI::Vk::LayoutMode mode = RI::Vk::LayoutMode::eAuto;
    if (c.options.mode == "unified") {
        mode = RI::Vk::LayoutMode::eUnified;
    } else if (c.options.mode == "general") {
        mode = RI::Vk::LayoutMode::eGeneral;
    } else if (c.options.mode == "transitions") {
        mode = RI::Vk::LayoutMode::eTransitions;
    }
    c.device = RI::Vk::createDevice({.gpu = gpu}, {.layoutMode = mode, .hostBarrierAtEnd = c.options.hostBarrier});
    if (!c.options.hostBarrier) {
        c.options.mode += "-nohost";
    }
#else
    c.device = RI::createDevice({.gpu = gpu});
#endif
    c.useBaseline = c.options.mode == "baseline";

    c.gpuMemory = RI::allocateMemory(c.device, RI::MemoryKind::eGPU, c.options.height > 1080 ? 2048ull << 20 : 768ull << 20);
    c.readbackMemory = RI::allocateMemory(c.device, RI::MemoryKind::eReadback, 1 << 16);
    c.heap = RI::createDescriptorHeap(c.device, {.resourceCount = 4096});
    const RI::BufferDesc outputDesc{.size = 4 << 20, .usage = RI::BufferUsage::eReadWrite};
    const RI::MemoryRequirements outReq = RI::bufferRequirements(c.device, outputDesc);
    c.output = RI::createBuffer(c.device, c.gpuMemory, 0, outputDesc);
    c.used = alignUp(outReq.size, outReq.alignment);
    c.ticks = RI::createBuffer(c.device, c.readbackMemory, 0, {.size = 256, .usage = RI::BufferUsage::eCopyDst});
    c.queries = RI::createQueryPool(c.device, {.type = RI::QueryType::eTimestamp, .count = 32});
    RI::writeDescriptors(c.device, c.heap,
                         {{.slot = kOutputSlot, .descriptor = RI::ReadWriteView{.buffer = c.output, .stride = 4}}});

    c.shaders = {loadShader("bench_vs"),   loadShader("bench_ps"),    loadShader("bench_ps1"), loadShader("bench_post_vs"),
                 loadShader("bench_post_ps"), loadShader("bench_read"), loadShader("bench_write"),
                 loadShader("bench_light"), loadShader("bench_hiz")};
    c.read = RI::createComputePipeline(c.device, {.shader = c.shaders.read});
    c.write = RI::createComputePipeline(c.device, {.shader = c.shaders.write});
    c.light = RI::createComputePipeline(c.device, {.shader = c.shaders.light});
    c.hiz = RI::createComputePipeline(c.device, {.shader = c.shaders.hiz});
    if (c.useBaseline || c.options.interleave) {
        c.baseline = bench::createBaseline(c.device, c.heap);
    }

    std::ofstream file;
    if (!c.options.out.empty()) {
        file.open(c.options.out, std::ios::app);
    }
    std::ostream &out = file.is_open() ? static_cast<std::ostream &>(file) : std::cout;
    std::fprintf(stderr, "layout_bench: %s, mode %s, %up, %u frames\n", c.device.gpu.name.c_str(), c.options.mode.c_str(),
                 c.options.height, c.options.frames);

    // One workload at a time; its textures are freed (and the memory reused) after it.
    using U = RI::TextureUsage;
    struct Target {
        RI::Format format;
        uint32 samples;
        const char *name;
    };
    std::vector<Target> targets = {{RI::Format::eRGBA8Unorm, 1, "render_sample_rgba8"},
                                   {RI::Format::eRGBA16Float, 1, "render_sample_rgba16f"},
                                   {RI::Format::eR11G11B10Float, 1, "render_sample_r11g11b10f"},
                                   {RI::Format::eRGBA16Float, 4, "render_sample_rgba16f_msaa4"},
                                   {RI::Format::eD32Float, 1, "render_sample_d32f"},
                                   {RI::Format::eD24UnormS8Uint, 1, "render_sample_d24s8"},
                                   {RI::Format::eD32FloatS8Uint, 1, "render_sample_d32fs8"}};
    std::vector<std::function<Workload(Owned &)>> makers;
    for (const Target &t : targets) {
        const U usage = U::eSampled | (RI::isDepthFormat(t.format) ? U::eDepthStencil : U::eRenderTarget);
        if (RI::formatSupported(c.device, t.format, usage, t.samples)) {
            makers.push_back([&c, t](Owned &o) { return renderThenSample(c, o, t.format, t.samples, t.name); });
        }
    }
    makers.push_back([&c](Owned &) { return storageThenSample(c); });
    makers.push_back([&c](Owned &o) { return depthThenHiz(c, o); });
    makers.push_back([&c](Owned &o) { return realisticFrame(c, o); });

    const uint64 baseUsed = c.used;
    for (auto &make : makers) {
        Owned owned;
        c.used = baseUsed;
        // --interleave: the RI mode and the baseline, each on its own textures,
        // alternating frame by frame. Otherwise the one mode given.
        std::vector<Workload> instances;
        instances.push_back(make(owned));
        if (c.options.interleave) {
            instances.push_back(make(owned));
        }
        std::vector<Path> paths;
        if (c.options.interleave) {
            // --swap: each path on the other texture copy (placement check).
            const size_t ri = c.options.swap ? 1 : 0;
            std::string label = "baseline:" + c.options.mode;
            if (!bench::baselineVariant.empty()) {
                label += "[" + bench::baselineVariant + "]";
            }
            paths.push_back({&instances[ri], false, c.options.mode + (c.options.swap ? "[swap]" : ""), {}});
            paths.push_back({&instances[1 - ri], true, label + (c.options.swap ? "[swap]" : ""), {}});
        } else {
            paths.push_back({&instances[0], c.useBaseline, c.options.mode, {}});
        }
        if (c.options.only.empty() || instances[0].name == c.options.only) {
            run(c, paths, out, c.device.gpu.name);
        }
        for (Workload &w : instances) {
            for (bench::Texture &t : w.textures) {
                if (t.sampledView.internal.textureId != 0 && RI::isDepthFormat(t.texture.format)) {
                    RI::destroyView(c.device, t.sampledView);
                }
                RI::destroyTexture(c.device, t.texture);
            }
        }
        for (RI::Pipeline &p : owned.pipelines) {
            RI::destroyPipeline(c.device, p);
        }
    }

    if (c.baseline != nullptr) {
        bench::destroyBaseline(c.baseline);
    }
    for (RI::Pipeline *p : {&c.read, &c.write, &c.light, &c.hiz}) {
        RI::destroyPipeline(c.device, *p);
    }
    RI::destroyQueryPool(c.device, c.queries);
    RI::destroyBuffer(c.device, c.output);
    RI::destroyBuffer(c.device, c.ticks);
    RI::destroyDescriptorHeap(c.device, c.heap);
    RI::freeMemory(c.device, c.gpuMemory);
    RI::freeMemory(c.device, c.readbackMemory);
    RI::destroyDevice(c.device);
    return 0;
}

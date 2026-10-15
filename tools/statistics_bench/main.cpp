// The statistics-cost measurement (P105; docs/FeatureProposal-Sandbox-2026-10-15.md,
// "Cost"). Usage:
//   statistics_bench_<backend> [--gpu nvidia|amd|<index>] [--frames N] [--warmup N]
//                              [--passes N] [--tasks N] [--out results.csv]
//
// A frame is `passes` mesh-shader passes (1920x1080 color and depth) and one
// compute pass, recorded last, with one query around each pass. The variants
// (Queries.hpp, A to H) run interleaved frame by frame (frame f runs variant f % n), so
// they see the same clock and power states (P96). The frame's GPU time comes
// from two timestamps. Prints per variant the median, p10 and p90 in ms, and
// the comparisons that decide the shape of the statistics counters, with P79's
// frame thresholds: "clearly more" is over 2% and over 0.05 ms.
//
// Watch out: every variant is native (Queries.hpp says why RI's own pool cannot
// be used around mesh draws on Vulkan).

#include "Queries.hpp"

#include <RI/RI.hpp>
#include <RI/Helpers/TypedCommands.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if EQUINE_BACKEND_D3D12
#include <D3D12AgilityExports.hpp>
EQUINE_D3D12_AGILITY_SDK_EXPORTS()
#endif

namespace statbench {

const char *variantName(Variant variant) {
    switch (variant) {
    case Variant::eNone: return "A_none";
    case Variant::eStats: return "B_stats";
    case Variant::eStatsMesh: return "C_stats_mesh";
    case Variant::eStatsMeshPrimitives: return "D_stats_mesh_primitives";
    case Variant::eComputeOnly: return "E_compute_only";
    case Variant::eStatsPrimitives: return "F_stats_primitives";
    case Variant::eStatsTask: return "G_stats_task";
    case Variant::eStatsMeshInvocations: return "H_stats_mesh_invocations";
    }
    return "?";
}

} // namespace statbench

namespace {

using statbench::Variant;

struct Options {
    std::string gpu;
    uint32 frames = 600; // measured frames per variant
    uint32 warmup = 120; // frames before measuring, all variants interleaved
    uint32 passes = 6;
    uint32 tasks = 512; // task groups per mesh pass
    std::string out;
};

Options parse(int argc, char **argv) {
    Options o;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const std::string value = argv[i + 1];
        if (key == "--gpu") {
            o.gpu = value;
        } else if (key == "--frames") {
            o.frames = static_cast<uint32>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--warmup") {
            o.warmup = static_cast<uint32>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--passes") {
            o.passes = static_cast<uint32>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--tasks") {
            o.tasks = static_cast<uint32>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "--out") {
            o.out = value;
        } else {
            std::fprintf(stderr, "unknown option %s\n", key.c_str());
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

// Mirrors MeshRoot in shaders/stat_as.hlsl and stat_ms.hlsl.
struct MeshRoot {
    uint32 gridSize;
    uint32 pass;
    uint32 meshletsPerTask;
};

// Mirrors ComputeRoot in shaders/stat_cs.hlsl.
struct ComputeRoot {
    uint32 bufferSlot;
    uint32 count;
};

constexpr uint32 kWidth = 1920;
constexpr uint32 kHeight = 1080;
constexpr uint32 kComputeCount = 1u << 20;
constexpr uint32 kMeshletsPerTask = 32;
constexpr uint32 kGridSize = 64;

struct Stats {
    double median, p10, p90;
};

Stats stats(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) { return v[static_cast<size_t>(q * static_cast<double>(v.size() - 1))]; };
    return {at(0.5), at(0.1), at(0.9)};
}

// One variant's queries: none (A) or native ones (B to E).
struct Run {
    Variant variant = Variant::eNone;
    statbench::NativeQueries *native = nullptr;
    std::vector<double> times;
};

void begin(Run &run, RI::CommandList &cmd, uint32 index, bool mesh) {
    if (run.native != nullptr) {
        statbench::beginNativeQuery(*run.native, cmd, index, mesh);
    }
}

void end(Run &run, RI::CommandList &cmd, uint32 index, bool mesh) {
    if (run.native != nullptr) {
        statbench::endNativeQuery(*run.native, cmd, index, mesh);
    }
}

// Pass/fail of "variant costs clearly more than reference" (P79's frame rule).
const char *verdict(double variant, double reference) {
    const double delta = variant - reference;
    return delta > 0.05 && delta > 0.02 * reference ? "clearly more" : "not clearly more";
}

} // namespace

int main(int argc, char **argv) {
    const Options o = parse(argc, argv);
    RI::Device device = RI::createDevice({.gpu = pickGpu(o.gpu)});
    const std::string gpuName = device.gpu.name;
#if EQUINE_BACKEND_VULKAN
    const char *backend = "vulkan";
#else
    const char *backend = "d3d12";
#endif
    if (!device.limits.meshShaders) {
        std::fprintf(stderr, "%s: no mesh shaders, nothing to measure\n", gpuName.c_str());
        RI::destroyDevice(device);
        return 1;
    }
    const uint32 queryCount = o.passes + 1; // mesh passes, then the compute pass

    // Targets, compute output and readback.
    RI::Memory gpuMemory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 64ull << 20);
    RI::Memory readbackMemory = RI::allocateMemory(device, RI::MemoryKind::eReadback, 1 << 20);
    uint64 used = 0;
    const auto placeTexture = [&](const RI::TextureDesc &desc) {
        const RI::MemoryRequirements req = RI::textureRequirements(device, desc);
        const uint64 offset = alignUp(used, req.alignment);
        used = offset + req.size;
        return RI::createTexture(device, gpuMemory, offset, desc);
    };
    RI::Texture color = placeTexture(
        {.width = kWidth, .height = kHeight, .format = RI::Format::eRGBA8Unorm, .usage = RI::TextureUsage::eRenderTarget});
    RI::Texture depth = placeTexture(
        {.width = kWidth, .height = kHeight, .format = RI::Format::eD32Float, .usage = RI::TextureUsage::eDepthStencil});
    const RI::BufferDesc computeDesc{.size = kComputeCount * 4, .usage = RI::BufferUsage::eReadWrite};
    const RI::MemoryRequirements computeReq = RI::bufferRequirements(device, computeDesc);
    RI::Buffer computeBuffer = RI::createBuffer(device, gpuMemory, alignUp(used, computeReq.alignment), computeDesc);
    const RI::BufferDesc timestampsDesc{.size = 16, .usage = RI::BufferUsage::eCopyDst};
    const RI::BufferDesc resultsDesc{.size = 64 * 1024, .usage = RI::BufferUsage::eCopyDst};
    RI::Buffer timestamps = RI::createBuffer(device, readbackMemory, 0, timestampsDesc);
    RI::Buffer results = RI::createBuffer(
        device, readbackMemory,
        alignUp(RI::bufferRequirements(device, timestampsDesc).size, RI::bufferRequirements(device, resultsDesc).alignment),
        resultsDesc);
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 4});
    RI::writeDescriptors(device, heap,
                         {{.slot = 0, .descriptor = RI::ReadWriteView{.buffer = computeBuffer, .stride = 4}}});

    const std::vector<std::byte> as = loadShader("stat_as"), ms = loadShader("stat_ms"), ps = loadShader("stat_ps"),
                                 cs = loadShader("stat_cs");
    RI::Pipeline meshPipeline =
        RI::createGraphicsPipeline(device, {.pixel = ps,
                                            .task = as,
                                            .mesh = ms,
                                            .colorFormats = {RI::Format::eRGBA8Unorm},
                                            .depthFormat = RI::Format::eD32Float,
                                            .depth = {.test = true, .write = true, .compare = RI::CompareOp::eLessEqual}});
    RI::Pipeline computePipeline = RI::createComputePipeline(device, {.shader = cs});
    RI::QueryPool timestampPool = RI::createQueryPool(device, {.type = RI::QueryType::eTimestamp, .count = 2});

    // The variants this device can run, in order A..E.
    std::vector<Run> runs;
    for (Variant v : {Variant::eNone, Variant::eStats, Variant::eStatsMesh, Variant::eStatsMeshPrimitives,
                      Variant::eComputeOnly, Variant::eStatsPrimitives, Variant::eStatsTask,
                      Variant::eStatsMeshInvocations}) {
        Run run{.variant = v};
        if (v != Variant::eNone) {
            run.native = statbench::createNativeQueries(device, v, queryCount);
            if (run.native == nullptr) {
                std::printf("# %s: not supported on this device, skipped\n", statbench::variantName(v));
                continue;
            }
        }
        runs.push_back(std::move(run));
    }

    RI::CommandList cmd = RI::createCommandList(device);
    const double ticksPerMs = static_cast<double>(device.limits.timestampFrequency) / 1000.0;
    const uint32 total = o.warmup * static_cast<uint32>(runs.size()) + o.frames * static_cast<uint32>(runs.size());
    for (uint32 frame = 0; frame < total; ++frame) {
        Run &run = runs[frame % runs.size()];
        if (frame > 0) {
            RI::resetCommandList(cmd); // the last frame was waited for
        }
        RI::cmdBegin(cmd);
        RI::cmdSetHeap(cmd, heap);
        RI::cmdWriteTimestamp(cmd, timestampPool, 0);
        for (uint32 pass = 0; pass < o.passes; ++pass) {
            begin(run, cmd, pass, true);
            const RI::LoadOp load = pass == 0 ? RI::LoadOp::eClear : RI::LoadOp::eLoad;
            RI::cmdBeginRendering(cmd, {.colors = {{.view = color.view, .load = load}},
                                        .depth = RI::DepthAttachment{.view = depth.view, .load = load}});
            RI::cmdSetPipeline(cmd, meshPipeline);
            RI::Helpers::cmdSetRootData(cmd, MeshRoot{.gridSize = kGridSize, .pass = pass, .meshletsPerTask = kMeshletsPerTask});
            RI::cmdDrawMesh(cmd, o.tasks, 1, 1);
            RI::cmdEndRendering(cmd);
            end(run, cmd, pass, true);
        }
        begin(run, cmd, o.passes, false);
        RI::cmdBarrier(cmd, RI::Stage::eAll, RI::Stage::eCompute);
        RI::cmdSetPipeline(cmd, computePipeline);
        RI::Helpers::cmdSetRootData(cmd, ComputeRoot{.bufferSlot = 0, .count = kComputeCount});
        RI::cmdDispatch(cmd, kComputeCount / 64, 1, 1);
        end(run, cmd, o.passes, false);
        RI::cmdWriteTimestamp(cmd, timestampPool, 1);
        RI::cmdBarrier(cmd, RI::Stage::eAll, RI::Stage::eCopy);
        RI::cmdResolveQueries(cmd, timestampPool, 0, 2, timestamps, 0);
        // Reading the results back is part of using queries, as the sandbox does.
        if (run.native != nullptr) {
            statbench::resolveNativeQueries(*run.native, cmd, results);
        }
        RI::cmdEnd(cmd);
        RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));

        RI::resetQueries(device, timestampPool, 0, 2);
        if (run.native != nullptr) {
            statbench::resetNativeQueries(device, *run.native);
        }
        if (frame >= o.warmup * runs.size()) {
            const auto *t = static_cast<const uint64 *>(timestamps.mapped);
            run.times.push_back(static_cast<double>(t[1] - t[0]) / ticksPerMs);
        }
    }

    std::FILE *csv = o.out.empty() ? nullptr : std::fopen(o.out.c_str(), "w");
    if (csv != nullptr) {
        std::fprintf(csv, "gpu,backend,variant,frame,ms\n");
        for (const Run &run : runs) {
            for (size_t i = 0; i < run.times.size(); ++i) {
                std::fprintf(csv, "%s,%s,%s,%zu,%.5f\n", gpuName.c_str(), backend, statbench::variantName(run.variant),
                             i, run.times[i]);
            }
        }
        std::fclose(csv);
    }

    std::printf("# %s (%s): %u mesh passes x %u task groups, %u frames per variant, interleaved\n", gpuName.c_str(),
                backend, o.passes, o.tasks, o.frames);
    std::printf("%-26s %10s %10s %10s %12s\n", "variant", "median ms", "p10", "p90", "vs A");
    double medians[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    for (const Run &run : runs) {
        const Stats s = stats(run.times);
        medians[static_cast<int>(run.variant)] = s.median;
        std::printf("%-26s %10.4f %10.4f %10.4f %+11.2f%%\n", statbench::variantName(run.variant), s.median, s.p10,
                    s.p90, 100.0 * (s.median - medians[0]) / medians[0]);
    }
    const auto compare = [&](Variant a, Variant b, const char *meaning) {
        const double ma = medians[static_cast<int>(a)], mb = medians[static_cast<int>(b)];
        if (ma < 0 || mb < 0) {
            return;
        }
        std::printf("%s vs %s: %+.4f ms (%+.2f%%), %s -> %s\n", statbench::variantName(a), statbench::variantName(b),
                    ma - mb, 100.0 * (ma - mb) / mb, verdict(ma, mb), meaning);
    };
    compare(Variant::eStats, Variant::eNone, "the cost of a statistics query");
    compare(Variant::eStatsMesh, Variant::eStats, "the cost of adding task and mesh counters");
    compare(Variant::eStatsMeshPrimitives, Variant::eStatsMesh, "clearly more: the primitive count becomes its own query");
    compare(Variant::eStats, Variant::eComputeOnly, "clearly more: a pool should choose its counters");
    compare(Variant::eStatsPrimitives, Variant::eStats, "the cost of the primitive count without task and mesh counters");
    compare(Variant::eStatsTask, Variant::eStats, "the cost of the task counter alone");
    compare(Variant::eStatsMeshInvocations, Variant::eStats, "the cost of the mesh counter alone");

    RI::destroyCommandList(device, cmd);
    for (Run &run : runs) {
        statbench::destroyNativeQueries(device, run.native);
    }
    RI::destroyQueryPool(device, timestampPool);
    RI::destroyPipeline(device, meshPipeline);
    RI::destroyPipeline(device, computePipeline);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroyBuffer(device, computeBuffer);
    RI::destroyBuffer(device, timestamps);
    RI::destroyBuffer(device, results);
    RI::destroyTexture(device, color);
    RI::destroyTexture(device, depth);
    RI::freeMemory(device, gpuMemory);
    RI::freeMemory(device, readbackMemory);
    RI::destroyDevice(device);
    return 0;
}

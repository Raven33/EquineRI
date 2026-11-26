// GPU-written indirect counts on Metal 4 (P18, P48, P116 (3)): RI's path
// against indirect command buffers (ICBs). Native Metal through metal-cpp,
// offscreen; Level 2 records (root data, then MTLDrawPrimitivesIndirectArguments),
// drawn inside one render pass, as cmdExecuteIndirectWithRootData is.
//
// A  RI's path: a vertex function with rasterization off copies the records
//    and zeroes the instance count of those at or beyond the count; then
//    maxCount indirect draws encoded on the CPU, each pointing argument-table
//    indices 2 (root data) and 4 (draw arguments) at its record.
// B  ICB: a vertex function encodes maxCount ICB commands (the buffers an RI
//    command binds: heaps 0 and 1, root data 2, draw arguments 4, index type
//    5 for the vertex stage, 0 to 2 for the fragment stage; draw_primitives;
//    reset past the count); one executeCommandsInBuffer over maxCount.
// B2 ICB with a GPU-written range: the vertex function encodes only the first
//    `count` commands and writes the execution range; executeCommandsInBuffer
//    reads it from GPU memory.
//
// Each draw lights its own pixel (record i at pixel i of a 1920 x 1080
// target): every variant is checked to light exactly `count` pixels. Times:
// the CPU time to encode the pass, and the command buffer's GPU time (commit
// feedback), median of 20 runs after 3.
//
// Run: tools/indirect_count_bench/indirect_count_bench (offscreen, about 30 s).

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

constexpr uint32_t kWidth = 1920, kHeight = 1080, kRuns = 20, kWarmup = 3;

constexpr const char *kSource = R"(
#include <metal_stdlib>
using namespace metal;

struct Root { float2 pos; float2 pad; };
struct Args { uint vertexCount, instanceCount, vertexStart, baseInstance; };
struct Record { Root root; Args args; };

struct Out { float4 position [[position]]; };
vertex Out drawVertex(uint v [[vertex_id]], device const Root &root [[buffer(2)]],
                      device const Args &args [[buffer(4)]]) {
    const float2 corner = float2(v == 1 ? 1.5 : -0.5, v == 2 ? 1.5 : -0.5);
    const float2 pixel = root.pos + 0.5 + corner * 0.6;
    Out o;
    o.position = float4(pixel.x / 1920.0 * 2 - 1, 1 - pixel.y / 1080.0 * 2, args.vertexCount == 3 ? 0.5 : 2.0, 1);
    return o;
}
fragment float4 drawFragment() { return float4(1, 1, 1, 1); }

struct CountParams {
    device const uint *src; device uint *dst; device const uint *count;
    uint total, words, zeroWord, maxCount;
};
vertex void countCopy(constant CountParams &p [[buffer(0)]], uint v [[vertex_id]]) {
    if (v >= p.total) return;
    const uint record = v / p.words;
    const uint count = min(*p.count, p.maxCount);
    p.dst[v] = (record >= count && v % p.words == p.zeroWord) ? 0u : p.src[v];
}

struct IcbArgs { command_buffer commands; };
struct EncodeParams {
    device const IcbArgs *icb; device const Record *records; device const uint *count;
    device uint *range; device const uint *heaps; uint maxCount; uint resetPast;
};
vertex void encodeCommands(constant EncodeParams &p [[buffer(0)]], uint i [[vertex_id]]) {
    if (i >= p.maxCount) return;
    const uint count = min(*p.count, p.maxCount);
    if (i == 0) { p.range[0] = 0; p.range[1] = count; }
    render_command c(p.icb->commands, i);
    if (i < count) {
        device const Record &r = p.records[i];
        // What an RI command binds (DeviceState.hpp's argument-table indices):
        // both heaps, the root data, the draw arguments and the index type,
        // for the vertex and fragment stages (an ICB command cannot inherit
        // some bindings and set others).
        c.set_vertex_buffer(p.heaps, 0);
        c.set_vertex_buffer(p.heaps + 64, 1);
        c.set_vertex_buffer(&r.root, 2);
        c.set_vertex_buffer(&r.args, 4);
        c.set_vertex_buffer(p.heaps + 128, 5);
        c.set_fragment_buffer(p.heaps, 0);
        c.set_fragment_buffer(p.heaps + 64, 1);
        c.set_fragment_buffer(&r.root, 2);
        c.draw_primitives(primitive_type::triangle, r.args.vertexStart, r.args.vertexCount, r.args.instanceCount,
                          r.args.baseInstance);
    } else if (p.resetPast != 0) {
        c.reset();
    }
}
)";

struct Record {
    float pos[2], pad[2];
    uint32_t vertexCount, instanceCount, vertexStart, baseInstance;
};
static_assert(sizeof(Record) == 32);

struct CountParams {
    uint64_t src, dst, count;
    uint32_t total, words, zeroWord, maxCount;
};
struct EncodeParams {
    uint64_t icb, records, count, range, heaps;
    uint32_t maxCount, resetPast;
};

struct Feedback {
    std::mutex mutex;
    std::condition_variable done;
    bool arrived = false;
    double gpuMs = 0;
    bool failed = false;
};

NS::String *str(const char *s) { return NS::String::string(s, NS::UTF8StringEncoding); }

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

struct Bench {
    MTL::Device *device = nullptr;
    MTL4::CommandQueue *queue = nullptr;
    MTL4::CommandAllocator *allocator = nullptr;
    MTL4::CommandBuffer *commands = nullptr;
    MTL4::ArgumentTable *table = nullptr;
    MTL::ResidencySet *residency = nullptr;
    MTL::RenderPipelineState *draw = nullptr, *copy = nullptr, *encode = nullptr;
    MTL::Texture *target = nullptr;
    MTL::Buffer *readback = nullptr;

    MTL::Buffer *buffer(uint64_t size) {
        MTL::Buffer *b = device->newBuffer(size, MTL::ResourceStorageModeShared);
        residency->addAllocation(b);
        return b;
    }

    MTL::RenderPipelineState *pipeline(MTL::Library *library, const char *vertex, const char *fragment, bool icb) {
        MTL::RenderPipelineDescriptor *d = MTL::RenderPipelineDescriptor::alloc()->init();
        d->setVertexFunction(library->newFunction(str(vertex)));
        if (fragment != nullptr) {
            d->setFragmentFunction(library->newFunction(str(fragment)));
            d->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        } else {
            d->setRasterizationEnabled(false);
        }
        d->setSupportIndirectCommandBuffers(icb);
        NS::Error *error = nullptr;
        MTL::RenderPipelineState *p = device->newRenderPipelineState(d, &error);
        if (p == nullptr) {
            std::printf("pipeline %s: %s\n", vertex, error->localizedDescription()->utf8String());
            std::exit(1);
        }
        d->release();
        return p;
    }

    // Commits the recorded command buffer and waits; GPU time in ms.
    double submit(bool *failed) {
        residency->commit();
        Feedback feedback;
        MTL4::CommitOptions *options = MTL4::CommitOptions::alloc()->init();
        options->addFeedbackHandler([&feedback](MTL4::CommitFeedback *result) {
            const std::lock_guard lock(feedback.mutex);
            feedback.gpuMs = (result->GPUEndTime() - result->GPUStartTime()) * 1000.0;
            feedback.failed = result->error() != nullptr;
            if (feedback.failed) {
                std::printf("GPU error: %s\n", result->error()->localizedDescription()->utf8String());
            }
            feedback.arrived = true;
            feedback.done.notify_all();
        });
        const MTL4::CommandBuffer *list[] = {commands};
        queue->commit(list, 1, options);
        options->release();
        std::unique_lock lock(feedback.mutex);
        feedback.done.wait(lock, [&] { return feedback.arrived; });
        *failed = feedback.failed;
        return feedback.gpuMs;
    }

    // Lit pixels of the target.
    uint32_t lit() {
        allocator->reset();
        commands->beginCommandBuffer(allocator);
        MTL4::ComputeCommandEncoder *c = commands->computeCommandEncoder();
        c->copyFromTexture(target, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(kWidth, kHeight, 1), readback, 0, kWidth * 4,
                           kWidth * kHeight * 4);
        c->endEncoding();
        commands->endCommandBuffer();
        bool failed = false;
        submit(&failed);
        const auto *texels = static_cast<const uint32_t *>(readback->contents());
        uint32_t n = 0;
        for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
            n += texels[i] != 0 ? 1 : 0;
        }
        return n;
    }
};

} // namespace

int main() {
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    Bench b;
    b.device = MTL::CreateSystemDefaultDevice();
    NS::Error *error = nullptr;
    b.queue = b.device->newMTL4CommandQueue(MTL4::CommandQueueDescriptor::alloc()->init(), &error);
    b.allocator = b.device->newCommandAllocator();
    b.commands = b.device->newCommandBuffer();
    MTL4::ArgumentTableDescriptor *td = MTL4::ArgumentTableDescriptor::alloc()->init();
    td->setMaxBufferBindCount(8);
    b.table = b.device->newArgumentTable(td, &error);
    b.residency = b.device->newResidencySet(MTL::ResidencySetDescriptor::alloc()->init(), &error);
    b.queue->addResidencySet(b.residency);
    MTL::Library *library = b.device->newLibrary(str(kSource), nullptr, &error);
    if (library == nullptr) {
        std::printf("library: %s\n", error->localizedDescription()->utf8String());
        return 1;
    }
    b.draw = b.pipeline(library, "drawVertex", "drawFragment", true);
    b.copy = b.pipeline(library, "countCopy", nullptr, false);
    b.encode = b.pipeline(library, "encodeCommands", nullptr, false);
    MTL::TextureDescriptor *tex = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, kWidth, kHeight, false);
    tex->setStorageMode(MTL::StorageModePrivate);
    tex->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    b.target = b.device->newTexture(tex);
    b.residency->addAllocation(b.target);
    b.readback = b.buffer(uint64_t{kWidth} * kHeight * 4);

    std::printf("GPU %s; Level 2 records of 32 bytes; target %u x %u\n", b.device->name()->utf8String(), kWidth, kHeight);
    std::printf("%8s %8s | %-28s | %-28s | %-28s\n", "maxCount", "count", "A: RI (copy + CPU draws)",
                "B: ICB, reset past count", "B2: ICB, GPU range");
    for (uint32_t maxCount : {256u, 4096u, 65536u}) {
        MTL::Buffer *records = b.buffer(uint64_t{maxCount} * sizeof(Record));
        MTL::Buffer *copy = b.buffer(uint64_t{maxCount} * sizeof(Record));
        MTL::Buffer *countBuffer = b.buffer(16);
        MTL::Buffer *range = b.buffer(16);
        MTL::Buffer *params = b.buffer(256);
        MTL::Buffer *heaps = b.buffer(1024); // stands in for the heaps and the index-type uniform
        auto *r = static_cast<Record *>(records->contents());
        for (uint32_t i = 0; i < maxCount; ++i) {
            r[i] = {{float(i % kWidth), float(i / kWidth)}, {0, 0}, 3, 1, 0, 0};
        }
        MTL::IndirectCommandBufferDescriptor *id = MTL::IndirectCommandBufferDescriptor::alloc()->init();
        id->setCommandTypes(MTL::IndirectCommandTypeDraw);
        id->setInheritPipelineState(true);
        id->setInheritBuffers(false);
        id->setMaxVertexBufferBindCount(6);
        id->setMaxFragmentBufferBindCount(3);
        MTL::IndirectCommandBuffer *icb = b.device->newIndirectCommandBuffer(id, maxCount, MTL::ResourceStorageModePrivate);
        id->release();
        b.residency->addAllocation(icb);
        MTL::Buffer *icbArgs = b.buffer(16);
        const MTL::ResourceID icbId = icb->gpuResourceID();
        std::memcpy(icbArgs->contents(), &icbId, sizeof(icbId));

        for (uint32_t percent : {1u, 50u, 100u}) {
            const uint32_t count = std::max(1u, maxCount * percent / 100);
            *static_cast<uint32_t *>(countBuffer->contents()) = count;
            char cells[3][64];
            for (int variant = 0; variant < 3; ++variant) {
                std::vector<double> cpu, gpu;
                uint32_t pixels = 0;
                bool anyFailed = false;
                for (uint32_t run = 0; run < kRuns + kWarmup; ++run) {
                    NS::AutoreleasePool *frame = NS::AutoreleasePool::alloc()->init();
                    b.allocator->reset();
                    b.commands->beginCommandBuffer(b.allocator);
                    const auto start = std::chrono::steady_clock::now();
                    MTL4::RenderPassDescriptor *pass = MTL4::RenderPassDescriptor::alloc()->init();
                    auto *a = pass->colorAttachments()->object(0);
                    a->setTexture(b.target);
                    a->setLoadAction(MTL::LoadActionClear);
                    a->setClearColor(MTL::ClearColor(0, 0, 0, 0));
                    a->setStoreAction(MTL::StoreActionStore);
                    MTL4::RenderCommandEncoder *e = b.commands->renderCommandEncoder(pass);
                    pass->release();
                    e->setArgumentTable(b.table, MTL::RenderStageVertex | MTL::RenderStageFragment);
                    if (variant == 0) {
                        const CountParams p{records->gpuAddress(), copy->gpuAddress(), countBuffer->gpuAddress(),
                                            maxCount * 8, 8, 5, maxCount};
                        std::memcpy(params->contents(), &p, sizeof(p));
                        b.table->setAddress(params->gpuAddress(), 0);
                        e->setRenderPipelineState(b.copy);
                        e->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger(0), NS::UInteger(maxCount * 8));
                        e->barrierAfterEncoderStages(MTL::StageVertex, MTL::StageVertex | MTL::StageFragment,
                                                     MTL4::VisibilityOptionDevice);
                        e->setRenderPipelineState(b.draw);
                        for (uint32_t i = 0; i < maxCount; ++i) {
                            const uint64_t record = copy->gpuAddress() + uint64_t{i} * sizeof(Record);
                            b.table->setAddress(record, 2);
                            b.table->setAddress(record + 16, 4);
                            e->drawPrimitives(MTL::PrimitiveTypeTriangle, record + 16);
                        }
                    } else {
                        const EncodeParams p{icbArgs->gpuAddress(), records->gpuAddress(), countBuffer->gpuAddress(),
                                             range->gpuAddress(), heaps->gpuAddress(), maxCount,
                                             variant == 1 ? 1u : 0u};
                        std::memcpy(params->contents(), &p, sizeof(p));
                        b.table->setAddress(params->gpuAddress(), 0);
                        e->setRenderPipelineState(b.encode);
                        e->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger(0), NS::UInteger(maxCount));
                        e->barrierAfterEncoderStages(MTL::StageVertex, MTL::StageVertex | MTL::StageFragment,
                                                     MTL4::VisibilityOptionDevice);
                        e->setRenderPipelineState(b.draw);
                        if (variant == 1) {
                            e->executeCommandsInBuffer(icb, NS::Range(0, maxCount));
                        } else {
                            e->executeCommandsInBuffer(icb, range->gpuAddress());
                        }
                    }
                    e->endEncoding();
                    b.commands->endCommandBuffer();
                    const double cpuMs =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                    bool failed = false;
                    const double gpuMs = b.submit(&failed);
                    anyFailed = anyFailed || failed;
                    if (run >= kWarmup) {
                        cpu.push_back(cpuMs);
                        gpu.push_back(gpuMs);
                    }
                    if (run == 0 || run == kRuns + kWarmup - 1) {
                        const uint32_t n = b.lit();
                        if (run == 0) {
                            pixels = n;
                        } else if (n != pixels) {
                            std::printf("  variant %d: first run lit %u pixels, last run %u\n", variant, pixels, n);
                            pixels = n == count ? pixels : n;
                        }
                    }
                    frame->release();
                }
                std::snprintf(cells[variant], sizeof(cells[variant]), "cpu %7.3f gpu %7.3f ms%s", median(cpu),
                              median(gpu), anyFailed ? " ERR" : pixels == count ? "" : " WRONG");
                if (pixels != count) {
                    std::printf("  variant %d lit %u pixels, expected %u\n", variant, pixels, count);
                }
            }
            std::printf("%8u %8u | %-28s | %-28s | %-28s\n", maxCount, count, cells[0], cells[1], cells[2]);
        }
        icb->release();
    }
    pool->release();
    return 0;
}

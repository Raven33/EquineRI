// What MTLTextureUsagePixelFormatView costs on Apple GPUs (Metal backend,
// P74: RI sets it on textures that list viewFormats, and on sampled
// depth-stencil textures for their stencil views). Native Metal, not RI: the
// same work on textures with and without the flag.
//
// Color (RGBA8Unorm, RGBA16Float; 1x and 4x, 3840 x 2160): eight passes that
// load, blend over and store the target (memory traffic, where lossless
// compression matters), a pass that only resolves (4x), and a compute pass
// reading every sample. Depth (D32Float_S8, 1x and 4x): eight passes that load,
// write and store depth and stencil. Median GPU time of 30 frames, after 3.
//
// Run: tools/metal_view_bench/metal_view_bench (offscreen, about 10 s).

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr uint32_t kWidth = 3840, kHeight = 2160, kPasses = 8, kFrames = 30, kWarmup = 3;

constexpr const char *kSource = R"(
#include <metal_stdlib>
using namespace metal;
struct Out { float4 position [[position]]; };
vertex Out fullscreen(uint v [[vertex_id]]) {
    float2 uv = float2((v << 1) & 2, v & 2);
    Out o; o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1); return o;
}
fragment float4 shade(Out in [[stage_in]], constant uint &pass [[buffer(0)]]) {
    float2 p = in.position.xy;
    return float4(fract(p.x * 0.013 + pass * 0.1), fract(p.y * 0.017), fract((p.x + p.y) * 0.007), 0.05);
}
struct D { float depth [[depth(any)]]; };
fragment D shadeDepth(Out in [[stage_in]]) {
    D d; d.depth = fract(in.position.x * 0.0013 + in.position.y * 0.0007); return d;
}
kernel void readSamples(texture2d_ms<float> t [[texture(0)]], device atomic_uint *sum [[buffer(0)]],
                        uint2 id [[thread_position_in_grid]]) {
    if (id.x >= t.get_width() || id.y >= t.get_height()) return;
    float s = 0;
    for (uint i = 0; i < t.get_num_samples(); ++i) s += t.read(id, i).r;
    if (s > 100.0) atomic_fetch_add_explicit(sum, 1, memory_order_relaxed);
}
kernel void readTexels(texture2d<float> t [[texture(0)]], device atomic_uint *sum [[buffer(0)]],
                       uint2 id [[thread_position_in_grid]]) {
    if (id.x >= t.get_width() || id.y >= t.get_height()) return;
    if (t.read(id).r > 100.0) atomic_fetch_add_explicit(sum, 1, memory_order_relaxed);
}
)";

struct Metal {
    MTL::Device *device = nullptr;
    MTL::Library *library = nullptr;
    MTL::CommandQueue *queue = nullptr;
};

MTL::Function *function(const Metal &m, const char *name) {
    return m.library->newFunction(NS::String::string(name, NS::UTF8StringEncoding));
}

double run(MTL::CommandBuffer *commands) {
    commands->commit();
    commands->waitUntilCompleted();
    return (commands->GPUEndTime() - commands->GPUStartTime()) * 1000.0;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

MTL::Texture *target(const Metal &m, MTL::PixelFormat format, uint32_t samples, bool view) {
    MTL::TextureDescriptor *desc = MTL::TextureDescriptor::texture2DDescriptor(format, kWidth, kHeight, false);
    if (samples > 1) {
        desc->setTextureType(MTL::TextureType2DMultisample);
        desc->setSampleCount(samples);
    }
    desc->setStorageMode(MTL::StorageModePrivate);
    MTL::TextureUsage usage = MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead;
    if (view) {
        usage |= MTL::TextureUsagePixelFormatView;
    }
    desc->setUsage(usage);
    return m.device->newTexture(desc);
}

void color(const Metal &m, MTL::PixelFormat format, const char *name, uint32_t samples) {
    NS::Error *error = nullptr;
    MTL::RenderPipelineDescriptor *rd = MTL::RenderPipelineDescriptor::alloc()->init();
    rd->setVertexFunction(function(m, "fullscreen"));
    rd->setFragmentFunction(function(m, "shade"));
    auto *attachment = rd->colorAttachments()->object(0);
    attachment->setPixelFormat(format);
    attachment->setBlendingEnabled(true);
    attachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
    attachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
    rd->setRasterSampleCount(samples);
    MTL::RenderPipelineState *draw = m.device->newRenderPipelineState(rd, &error);
    MTL::ComputePipelineState *read =
        m.device->newComputePipelineState(function(m, samples > 1 ? "readSamples" : "readTexels"), &error);
    MTL::Buffer *sum = m.device->newBuffer(4, MTL::ResourceStorageModeShared);
    for (bool view : {false, true}) {
        MTL::Texture *texture = target(m, format, samples, view);
        MTL::Texture *resolved = samples > 1 ? target(m, format, 1, false) : nullptr;
        std::vector<double> drawTimes, resolveTimes, readTimes;
        for (uint32_t frame = 0; frame < kFrames + kWarmup; ++frame) {
            NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
            MTL::CommandBuffer *commands = m.queue->commandBuffer();
            for (uint32_t pass = 0; pass < kPasses; ++pass) {
                MTL::RenderPassDescriptor *rp = MTL::RenderPassDescriptor::renderPassDescriptor();
                auto *a = rp->colorAttachments()->object(0);
                a->setTexture(texture);
                a->setLoadAction(pass == 0 ? MTL::LoadActionClear : MTL::LoadActionLoad);
                a->setStoreAction(MTL::StoreActionStore);
                MTL::RenderCommandEncoder *e = commands->renderCommandEncoder(rp);
                e->setRenderPipelineState(draw);
                e->setFragmentBytes(&pass, sizeof(pass), 0);
                e->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
                e->endEncoding();
            }
            const double drawTime = run(commands);
            double resolveTime = 0;
            if (resolved != nullptr) {
                commands = m.queue->commandBuffer();
                MTL::RenderPassDescriptor *rp = MTL::RenderPassDescriptor::renderPassDescriptor();
                auto *a = rp->colorAttachments()->object(0);
                a->setTexture(texture);
                a->setResolveTexture(resolved);
                a->setLoadAction(MTL::LoadActionLoad);
                a->setStoreAction(MTL::StoreActionMultisampleResolve);
                commands->renderCommandEncoder(rp)->endEncoding();
                resolveTime = run(commands);
            }
            commands = m.queue->commandBuffer();
            MTL::ComputeCommandEncoder *c = commands->computeCommandEncoder();
            c->setComputePipelineState(read);
            c->setTexture(texture, 0);
            c->setBuffer(sum, 0, 0);
            c->dispatchThreads(MTL::Size(kWidth, kHeight, 1), MTL::Size(16, 16, 1));
            c->endEncoding();
            const double readTime = run(commands);
            if (frame >= kWarmup) {
                drawTimes.push_back(drawTime);
                resolveTimes.push_back(resolveTime);
                readTimes.push_back(readTime);
            }
            pool->release();
        }
        std::printf("%-12s %ux %-22s: %u load/blend/store passes %7.3f ms, resolve %6.3f ms, read %6.3f ms\n", name,
                    samples, view ? "with PixelFormatView" : "without", kPasses, median(drawTimes),
                    median(resolveTimes), median(readTimes));
        texture->release();
        if (resolved != nullptr) {
            resolved->release();
        }
    }
    sum->release();
    read->release();
    draw->release();
    rd->release();
}

void depth(const Metal &m, uint32_t samples) {
    NS::Error *error = nullptr;
    MTL::RenderPipelineDescriptor *rd = MTL::RenderPipelineDescriptor::alloc()->init();
    rd->setVertexFunction(function(m, "fullscreen"));
    rd->setFragmentFunction(function(m, "shadeDepth"));
    rd->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);
    rd->setStencilAttachmentPixelFormat(MTL::PixelFormatDepth32Float_Stencil8);
    rd->setRasterSampleCount(samples);
    MTL::RenderPipelineState *draw = m.device->newRenderPipelineState(rd, &error);
    MTL::DepthStencilDescriptor *dsd = MTL::DepthStencilDescriptor::alloc()->init();
    dsd->setDepthCompareFunction(MTL::CompareFunctionAlways);
    dsd->setDepthWriteEnabled(true);
    MTL::DepthStencilState *state = m.device->newDepthStencilState(dsd);
    for (bool view : {false, true}) {
        MTL::Texture *texture = target(m, MTL::PixelFormatDepth32Float_Stencil8, samples, view);
        std::vector<double> times;
        for (uint32_t frame = 0; frame < kFrames + kWarmup; ++frame) {
            NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
            MTL::CommandBuffer *commands = m.queue->commandBuffer();
            for (uint32_t pass = 0; pass < kPasses; ++pass) {
                MTL::RenderPassDescriptor *rp = MTL::RenderPassDescriptor::renderPassDescriptor();
                for (MTL::RenderPassAttachmentDescriptor *a :
                     {static_cast<MTL::RenderPassAttachmentDescriptor *>(rp->depthAttachment()),
                      static_cast<MTL::RenderPassAttachmentDescriptor *>(rp->stencilAttachment())}) {
                    a->setTexture(texture);
                    a->setLoadAction(pass == 0 ? MTL::LoadActionClear : MTL::LoadActionLoad);
                    a->setStoreAction(MTL::StoreActionStore);
                }
                MTL::RenderCommandEncoder *e = commands->renderCommandEncoder(rp);
                e->setRenderPipelineState(draw);
                e->setDepthStencilState(state);
                e->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
                e->endEncoding();
            }
            const double time = run(commands);
            if (frame >= kWarmup) {
                times.push_back(time);
            }
            pool->release();
        }
        std::printf("%-12s %ux %-22s: %u load/write/store passes %7.3f ms\n", "D32Float_S8", samples,
                    view ? "with PixelFormatView" : "without", kPasses, median(times));
        texture->release();
    }
    state->release();
    dsd->release();
    draw->release();
    rd->release();
}

} // namespace

int main() {
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    Metal m;
    m.device = MTL::CreateSystemDefaultDevice();
    NS::Error *error = nullptr;
    m.library = m.device->newLibrary(NS::String::string(kSource, NS::UTF8StringEncoding), nullptr, &error);
    if (m.library == nullptr) {
        std::printf("newLibrary failed: %s\n", error->localizedDescription()->utf8String());
        return 1;
    }
    m.queue = m.device->newCommandQueue();
    std::printf("GPU %s, %u x %u\n", m.device->name()->utf8String(), kWidth, kHeight);
    for (uint32_t samples : {4u, 1u}) {
        color(m, MTL::PixelFormatRGBA8Unorm, "RGBA8Unorm", samples);
        color(m, MTL::PixelFormatRGBA16Float, "RGBA16Float", samples);
        depth(m, samples);
    }
    m.queue->release();
    m.library->release();
    m.device->release();
    pool->release();
    return 0;
}

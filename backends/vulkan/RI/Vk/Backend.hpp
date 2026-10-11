#pragma once

// The Vulkan backend's portable surface (P83). No native types here (P84):
// native handles are kept as integers or pointers in `internal`, and native
// access goes through <RI/Vk/Native.hpp>.

#include <RI/Commands.hpp>
#include <RI/Device.hpp>
#include <RI/Resources.hpp>
#include <RI/RayTracing.hpp>
#include <RI/Textures.hpp>
#include <RI/Window.hpp>

#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace RI::Vk {

struct DeviceState; // backend-private: native handles, function table, tracking

// ── Device and queue (P60, P92, P37, P20) ────────────────────────────────────

// The graphics queue (P37: the only queue in v1). Submissions return
// timeline points (P20).
struct Queue {
    struct {
        DeviceState *device = nullptr;
    } internal;
};

// A thin value (P81): copies refer to the same device. The device owns its
// Vulkan instance (P92).
struct Device {
    Gpu gpu;
    DeviceLimits limits;
    Queue graphicsQueue;
    struct {
        DeviceState *state = nullptr;
    } internal;
};

// The GPUs that can run RI (Vulkan 1.3 and a graphics + compute queue), in the
// driver's order. Picking one automatically is a helper (P60).
std::vector<Gpu> listGpus();

Device createDevice(const DeviceDesc &desc);
void destroyDevice(Device &device);

// ── Memory and buffers (P31, P32, P33, P58, P61) ─────────────────────────────

struct Memory {
    MemoryKind kind = MemoryKind::eGPU;
    uint64 size = 0;
    void *mapped = nullptr; // CPU address for every kind except eGPU (P33)
    struct {
        uint64 memory = 0;     // VkDeviceMemory
        uint32 memoryType = 0; // its memory type index
        bool imported = false; // P77: RI does not free it
        uint64 id = 0;         // validation builds: use-after-free tracking (P91)
    } internal;
};

Memory allocateMemory(Device &device, MemoryKind kind, uint64 size);
void freeMemory(Device &device, Memory &memory);

MemoryRequirements bufferRequirements(Device &device, const BufferDesc &desc);

// A buffer placed in user memory at `offset` (P31). It owns no memory.
struct Buffer {
    uint64 size = 0;
    BufferUsage usage = BufferUsage::eNone;
    uint64 gpuAddress = 0;  // always available (P61)
    void *mapped = nullptr; // memory.mapped + offset, or nullptr for eGPU memory
    struct {
        uint64 buffer = 0; // VkBuffer
        bool imported = false; // P77: RI does not destroy it
        uint64 id = 0;
    } internal;
};

Buffer createBuffer(Device &device, const Memory &memory, uint64 offset, const BufferDesc &desc);
void destroyBuffer(Device &device, Buffer &buffer);

// ── Textures and views (P58, P69, P73, P74, P95) ────────────────────────────

// Part of a texture (P73), created explicitly with createView; every texture
// also has a ready-made whole-texture view.
struct TextureView {
    Format format = Format::eUndefined;
    uint32 width = 0;  // of the view's first mip
    uint32 height = 0;
    uint32 samples = 1;
    struct {
        uint64 view = 0;       // VkImageView
        uint64 image = 0;      // VkImage of the texture
        uint32 aspectMask = 0; // VkImageAspectFlags
        uint32 baseMip = 0, mipCount = 1, baseLayer = 0, layerCount = 1;
        uint32 restingLayout = 0; // VkImageLayout the texture rests in (L1)
        TextureUsage textureUsage = TextureUsage::eNone; // checked by copies (P47)
        uint64 textureId = 0;     // validation builds (P91)
    } internal;
};

// A texture placed in user memory at `offset` (P31). It owns no memory. Its
// image layouts are RI's business (P16, L1).
struct Texture {
    uint32 width = 0;
    uint32 height = 0;
    uint32 layers = 1;
    uint32 mips = 1;
    uint32 samples = 1;
    Format format = Format::eUndefined;
    TextureUsage usage = TextureUsage::eNone;
    TextureView view; // the whole texture, in its own format
    struct {
        uint64 image = 0; // VkImage
        uint32 restingLayout = 0;
        uint32 viewFormats = 0; // bit per RI::Format views may use besides `format` (P74)
        bool imported = false;  // P77: RI does not destroy the image
        uint64 id = 0;
    } internal;
};

// Whether the GPU supports `format` for every usage in `usage` at this sample
// count (P58 (2)); e.g. AMD Vulkan has no eD24UnormS8Uint.
bool formatSupported(Device &device, Format format, TextureUsage usage, uint32 samples = 1);

MemoryRequirements textureRequirements(Device &device, const TextureDesc &desc);

// Creates the texture and resets it (contents undefined, ready for use), as a
// discard would (P69, R16). The reset is submitted on the graphics queue at once.
Texture createTexture(Device &device, const Memory &memory, uint64 offset, const TextureDesc &desc);
void destroyTexture(Device &device, Texture &texture);

TextureView createView(Device &device, const Texture &texture, const TextureViewDesc &desc);
void destroyView(Device &device, TextureView &view);

// ── Acceleration structures (P49) ─────────────────────────────────────────────

// A bottom- or top-level acceleration structure placed in user memory (P31).
struct AccelerationStructure {
    uint64 size = 0;
    uint64 gpuAddress = 0; // the address TLAS instances name (AccelerationStructureInstance::blasAddress)
    bool topLevel = false;
    struct {
        uint64 handle = 0; // VkAccelerationStructureKHR
        uint64 buffer = 0; // VkBuffer holding it, placed in the memory
        uint64 id = 0;
    } internal;
};

// Sizes for building `desc` (P49): the structure, and scratch for builds and updates.
AccelerationStructureSizes blasSizes(Device &device, const BlasDesc &desc);
AccelerationStructureSizes tlasSizes(Device &device, const TlasDesc &desc);
// Placement of a structure of `size` bytes: alignment and memory kinds.
MemoryRequirements accelerationStructureRequirements(Device &device, uint64 size);
AccelerationStructure createAccelerationStructure(Device &device, const Memory &memory, uint64 offset, uint64 size,
                                                  bool topLevel);
void destroyAccelerationStructure(Device &device, AccelerationStructure &structure);

// ── Descriptors (P42, P59, P87, P94) ─────────────────────────────────────────

// A resource heap and a sampler heap, written by slot (P42). Writes take effect
// at once; never overwrite a slot that in-flight GPU work may read (P59).
struct DescriptorHeap {
    uint32 resourceCount = 0;
    uint32 samplerCount = 0;
    struct {
        uint64 pool = 0;        // VkDescriptorPool
        uint64 resourceSet = 0; // VkDescriptorSet, set 0
        uint64 samplerSet = 0;  // VkDescriptorSet, set 1
        uint64 id = 0;
    } internal;
};

DescriptorHeap createDescriptorHeap(Device &device, const DescriptorHeapDesc &desc);
void destroyDescriptorHeap(Device &device, DescriptorHeap &heap);

// Views (P87). A read or read-write view names either a texture view or a
// buffer range. Buffer ranges: `stride` 0 is a raw view (ByteAddressBuffer);
// otherwise the element size of a StructuredBuffer. `size` kWholeSize: to the end.
struct ReadView {
    TextureView texture; // a texture view (Texture2D), or
    Buffer buffer;
    uint64 offset = 0;
    uint64 size = kWholeSize;
    uint32 stride = 0;
};
struct ReadWriteView {
    TextureView texture; // a texture view (RWTexture2D), or
    Buffer buffer;
    uint64 offset = 0;
    uint64 size = kWholeSize;
    uint32 stride = 0;
};
struct ConstantBufferView {
    Buffer buffer;
    uint64 offset = 0;
    uint64 size = kWholeSize;
};
// A top-level acceleration structure for RayQuery (RaytracingAccelerationStructure, P49).
struct AccelerationStructureView {
    AccelerationStructure tlas;
};

using Descriptor = std::variant<ReadView, ReadWriteView, ConstantBufferView, AccelerationStructureView>;

struct DescriptorWrite {
    uint32 slot = 0;
    Descriptor descriptor;
};

// Writes descriptors into heap slots: one native call for the whole list (D1, P94).
void writeDescriptors(Device &device, DescriptorHeap &heap, std::span<const DescriptorWrite> writes);
void writeDescriptors(Device &device, DescriptorHeap &heap, std::initializer_list<DescriptorWrite> writes);

// ── Queries (P51, P71) ───────────────────────────────────────────────────────

struct QueryPool {
    QueryType type = QueryType::eTimestamp;
    uint32 count = 0;
    struct {
        DeviceState *device = nullptr;
        uint64 pool = 0; // VkQueryPool
        uint64 id = 0;
    } internal;
};

// Created reset (P71 (1)).
QueryPool createQueryPool(Device &device, const QueryPoolDesc &desc);
void destroyQueryPool(Device &device, QueryPool &pool);
// Resets queries on the CPU, after their results were read (P71 (1)). No GPU cost.
void resetQueries(Device &device, QueryPool &pool, uint32 first, uint32 count);
// Where each counter is in a pipeline-statistics result (P71 (2)).
PipelineStatisticsLayout pipelineStatisticsLayout(Device &device);

// ── Rendering (P40, P52, P75) ────────────────────────────────────────────────

struct ColorAttachment {
    TextureView view;
    LoadOp load = LoadOp::eLoad;
    StoreOp store = StoreOp::eStore;
    ClearValue clear;
    // Resolve into this single-sampled view at cmdEndRendering (P52); a view
    // with format eUndefined (the default): no resolve.
    TextureView resolveTo;
    ResolveMode resolveMode = ResolveMode::eAverage;
};

struct DepthAttachment {
    TextureView view;
    LoadOp load = LoadOp::eLoad;
    StoreOp store = StoreOp::eStore;
    LoadOp stencilLoad = LoadOp::eLoad;
    StoreOp stencilStore = StoreOp::eStore;
    ClearValue clear; // depth and stencil
    bool depthReadOnly = false;   // shaders may sample it during the section if it has eReadOnlyDepthSampling (P75, P97)
    bool stencilReadOnly = false;
    // Resolve into this single-sampled view at cmdEndRendering (P52, P76),
    // depth and stencil with the same mode; format eUndefined: no resolve.
    TextureView resolveTo;
    ResolveMode resolveMode = ResolveMode::eMin;
};

// A rendering section. The viewport and scissor cover the attachments.
struct RenderingDesc {
    std::vector<ColorAttachment> colors;
    std::optional<DepthAttachment> depth;
};

// ── Pipelines (P38, P45, P67) ────────────────────────────────────────────────

struct Pipeline {
    uint32 rootDataSize = 0; // bytes, from the shader (P44, P45)
    struct {
        uint64 pipeline = 0; // VkPipeline
        uint64 layout = 0;   // VkPipelineLayout
        bool graphics = false;
        bool depthWrite = false; // checked against read-only depth sections (P75)
        bool imported = false;   // P77: RI does not destroy it
        uint64 id = 0;
    } internal;
};

Pipeline createComputePipeline(Device &device, const ComputePipelineDesc &desc);
Pipeline createGraphicsPipeline(Device &device, const GraphicsPipelineDesc &desc);
void destroyPipeline(Device &device, Pipeline &pipeline);

// ── Command lists and submission (P34, P56, P20, P36, P47) ───────────────────

// One command list with its own native pool (P56). Record, submit once, wait
// for its timeline point, then reset and reuse.
struct CommandList {
    struct {
        DeviceState *device = nullptr;
        void *commandBuffer = nullptr; // VkCommandBuffer
        uint64 pool = 0;               // VkCommandPool
        uint64 boundLayout[2] = {0, 0}; // pipeline layout per bind point (compute, graphics)
        bool graphics = false;          // bind point of the current pipeline
        uint64 heapResourceSet = 0;     // heap set by cmdSetHeap, 0 if none
        uint64 heapSamplerSet = 0;
        uint32 rootDataSize = 0;        // of the pipeline set by cmdSetPipeline
        uint64 pipeline = 0;            // VkPipeline set by cmdSetPipeline (indirect Level 2, P70)
        uint64 boundPipeline[2] = {0, 0}; // per bind point (compute, graphics), restored by endNative
        uint8 rootData[256] = {};       // the last root data set, restored after native sections (P70)
        uint32 rootDataBytes = 0;
        uint32 renderWidth = 0, renderHeight = 0; // the open section's viewport (P70)
        bool native = false;            // inside beginNative/endNative
        uint64 lastSubmitted = 0;       // timeline value of its last submission
        // The open rendering section: its attachments go back to rest at cmdEndRendering.
        bool rendering = false;
        bool depthReadOnly = false;
        uint32 colorCount = 0;
        TextureView colors[8];
        TextureView depth;
        TextureView resolves[8]; // resolve targets of the open section (P52)
        TextureView depthResolve;
        uint32 depthLayout = 0; // VkImageLayout used during the section
        uint32 markerDepth = 0; // open cmdBeginMarker regions
        void *extras = nullptr; // CommandListExtras: RI-owned per-list objects (P48)
        uint64 id = 0;
    } internal;
};

CommandList createCommandList(Device &device);
void destroyCommandList(Device &device, CommandList &cmd);
void resetCommandList(CommandList &cmd);

void cmdBegin(CommandList &cmd);
void cmdEnd(CommandList &cmd);
void cmdSetHeap(CommandList &cmd, const DescriptorHeap &heap);
void cmdSetPipeline(CommandList &cmd, const Pipeline &pipeline);
void cmdSetRootData(CommandList &cmd, const void *data, uint32 size);
template <typename T>
void cmdSetRootData(CommandList &cmd, const T &data) {
    cmdSetRootData(cmd, &data, static_cast<uint32>(sizeof(T)));
}
void cmdDispatch(CommandList &cmd, uint32 x, uint32 y, uint32 z);
void cmdBarrier(CommandList &cmd, Stage before, Stage after);
void cmdCopyBuffer(CommandList &cmd, const Buffer &src, uint64 srcOffset, const Buffer &dst, uint64 dstOffset,
                   uint64 size);

// Rendering sections (P40). RI switches attachment layouts around the section
// when its backend needs to (A', P68); user code is the same everywhere (P16).
void cmdBeginRendering(CommandList &cmd, const RenderingDesc &desc);
void cmdEndRendering(CommandList &cmd);
void cmdDraw(CommandList &cmd, uint32 vertexCount, uint32 instanceCount = 1, uint32 firstVertex = 0,
             uint32 firstInstance = 0);
// Indexed draws (P39): indices come from an index buffer; vertices are still
// pulled by the shader (SV_VertexID is the index value plus vertexOffset).
void cmdSetIndexBuffer(CommandList &cmd, const Buffer &buffer, uint64 offset, IndexFormat format);
void cmdDrawIndexed(CommandList &cmd, uint32 indexCount, uint32 instanceCount = 1, uint32 firstIndex = 0,
                    int32 vertexOffset = 0, uint32 firstInstance = 0);
// Mesh pipelines (P39): launches x * y * z task shader groups, or mesh shader
// groups when the pipeline has no task shader.
void cmdDrawMesh(CommandList &cmd, uint32 x, uint32 y, uint32 z);
// Indirect execution (P48). Level 1: `maxCount` records of indirectStride(kind)
// bytes at `offset` in `args`, with the count read from `count` at `countOffset`
// when given (at most maxCount). Dispatches take one record and no count.
// Buffers need BufferUsage::eIndirect; the GPU reads them at Stage::eIndirect.
void cmdExecuteIndirect(CommandList &cmd, IndirectKind kind, const Buffer &args, uint64 offset, uint32 maxCount,
                        const Buffer *count = nullptr, uint64 countOffset = 0);
// Level 2 (DeviceLimits::indirectRootData): each record starts with the root
// data of the pipeline set (its rootDataSize, rounded up to 4), followed by the
// arguments; stride indirectStride(kind, rootDataSize). Set root data again
// before later direct draws.
void cmdExecuteIndirectWithRootData(CommandList &cmd, IndirectKind kind, const Buffer &records, uint64 offset,
                                    uint32 maxCount, const Buffer *count = nullptr, uint64 countOffset = 0);




// Copies (P47). A texture view covers one mip (and one aspect of a depth
// format); the buffer side follows copyLayout(). Copies count as Stage::eCopy.
CopyLayout copyLayout(Device &device, const TextureView &view);
void cmdCopyBufferToTexture(CommandList &cmd, const Buffer &src, uint64 srcOffset, const TextureView &dst);
void cmdCopyTextureToBuffer(CommandList &cmd, const TextureView &src, const Buffer &dst, uint64 dstOffset);
// Same size, layer count, sample count and texel size.
void cmdCopyTexture(CommandList &cmd, const TextureView &src, const TextureView &dst);

// Resolves a multisampled view into a single-sampled one (P52, P76), outside
// rendering sections. Counts as Stage::eRenderTarget (color) or
// Stage::eDepthStencil (depth) for barriers.
void cmdResolve(CommandList &cmd, const TextureView &src, const TextureView &dst, ResolveMode mode);

// Acceleration structure builds (P49): `update` refits a structure built with
// eAllowUpdate from the same geometry. Scratch at `scratchOffset` (a multiple of
// DeviceLimits::accelerationStructureScratchAlignment) of a buffer with
// eReadWrite. Builds count as Stage::eAccelerationStructure.
void cmdBuildBlas(CommandList &cmd, const AccelerationStructure &dst, const BlasDesc &desc, const Buffer &scratch,
                  uint64 scratchOffset, bool update = false);
void cmdBuildTlas(CommandList &cmd, const AccelerationStructure &dst, const TlasDesc &desc, const Buffer &scratch,
                  uint64 scratchOffset, bool update = false);

// Required before first use of a texture whose memory held another resource
// (P69). A full-scope barrier.
void cmdDiscard(CommandList &cmd, const TextureView &view);

// Clears outside rendering sections (P65).
void cmdClearTexture(CommandList &cmd, const TextureView &view, const ClearValue &value);
void cmdClearBuffer(CommandList &cmd, const Buffer &buffer, uint32 value);

// Timestamps (P51, P71).
void cmdWriteTimestamp(CommandList &cmd, const QueryPool &pool, uint32 index);
void cmdResetQueries(CommandList &cmd, const QueryPool &pool, uint32 first, uint32 count);
// Occlusion and pipeline-statistics queries (P71): begin and end around the
// commands to measure, in the same command list.
void cmdBeginQuery(CommandList &cmd, const QueryPool &pool, uint32 index);
void cmdEndQuery(CommandList &cmd, const QueryPool &pool, uint32 index);
// Copies `count` results to `buffer` at `offset` (a multiple of 8): one
// 64-bit value per timestamp or occlusion query, pipelineStatisticsLayout().size
// bytes per statistics query.
void cmdResolveQueries(CommandList &cmd, const QueryPool &pool, uint32 first, uint32 count, const Buffer &buffer,
                       uint64 offset);

// ── Debugging (P53) ──────────────────────────────────────────────────────────

// Names shown by debuggers, captures (RenderDoc, PIX, Nsight) and validation
// messages. Always on, in every build.
void setName(Device &device, const Memory &memory, std::string_view name);
void setName(Device &device, const Buffer &buffer, std::string_view name);
void setName(Device &device, const Texture &texture, std::string_view name);
void setName(Device &device, const TextureView &view, std::string_view name);
void setName(Device &device, const DescriptorHeap &heap, std::string_view name);
void setName(Device &device, const QueryPool &pool, std::string_view name);
void setName(Device &device, const Pipeline &pipeline, std::string_view name);
void setName(Device &device, const CommandList &cmd, std::string_view name);

// A named region of commands in captures; regions nest and close in the same
// command list.
void cmdBeginMarker(CommandList &cmd, std::string_view name);
void cmdEndMarker(CommandList &cmd);

TimelinePoint submit(Queue &queue, CommandList &cmd);
void wait(Queue &queue, TimelinePoint point);
bool isReached(Queue &queue, TimelinePoint point);

// ── Swapchains (P43, P63, P64) ───────────────────────────────────────────────

struct SwapchainState; // backend-private: surface, images, semaphores

// A window's swapchain. The screen texture of each frame comes from acquire();
// swapchain image cycling is not frames in flight (P43).
struct Swapchain {
    uint32 width = 0;
    uint32 height = 0;
    SurfaceFormat format;
    PresentMode presentMode = PresentMode::eFifo;
    uint32 imageCount = 0;
    struct {
        SwapchainState *state = nullptr;
        uint64 id = 0;
    } internal;
};

// The (format, color space) pairs the window can show (P64 (1)).
std::vector<SurfaceFormat> surfaceFormats(Device &device, const Window &window);
bool presentModeSupported(Device &device, const Window &window, PresentMode mode);
// Whether compute may write the screen texture in this format (P64 (5)).
bool screenStorageSupported(Device &device, const Window &window, Format format);

Swapchain createSwapchain(Device &device, const SwapchainDesc &desc);
// Waits for the GPU, then recreates the images at the new size (P43: the user
// decides when, e.g. after a window resize or an out-of-date report).
void resizeSwapchain(Device &device, Swapchain &swapchain, uint32 width, uint32 height);
void destroySwapchain(Device &device, Swapchain &swapchain);

// The screen texture to draw into this frame (P63 (2)): usage eRenderTarget |
// eCopyDst (and eStorage if asked). Blocks while no image is free. Its
// contents are undefined after acquire.
Texture acquire(Device &device, Swapchain &swapchain);
// Shows the acquired texture once the work submitted before it has finished.
void present(Queue &queue, Swapchain &swapchain);

} // namespace RI::Vk

#pragma once

// The hidden per-device state behind RI::Metal::Device (P82), heap-allocated
// once per device so Device stays a small value, the per-command-list state,
// and the declarations the backend's sources share (P113-P116).
//
// Keeps: the device and queue, the residency set (every Memory heap, heap
// buffer and command memory chunk: Metal 4 makes nothing resident on its own),
// the timeline event, a small fixed buffer for MSC's index-type uniform, the
// sampler cache (one MTLSamplerState per SamplerDesc, P99, P114 (3)), the fence
// that orders counter-heap resolves, and in validation builds the object and
// placement trackers and the device thread (P106). Everything is released with
// the device.
//
// Ownership: metal-cpp objects created with new*/alloc are released with
// release(); RI holds one reference to each, never autoreleased objects
// beyond the call that made them (an NS::AutoreleasePool wraps calls that
// create autoreleased objects).
//
// D3D12 counterpart: backends/d3d12/DeviceState.hpp.

#include <RI/Error.hpp>
#include <RI/Metal/Backend.hpp>
#if EQUINE_VALIDATION
#include <RI/Validation/Checks.hpp>
#include <RI/Validation/ObjectTracker.hpp>
#include <RI/Validation/Placement.hpp>

#include <thread>
#endif

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace RI::Metal {

// MSC's binding model (P114): argument-table buffer indices.
inline constexpr uint32 kResourceHeapIndex = 0; // kIRDescriptorHeapBindPoint
inline constexpr uint32 kSamplerHeapIndex = 1;  // kIRSamplerHeapBindPoint
inline constexpr uint32 kRootDataIndex = 2;     // kIRArgumentBufferBindPoint: the 256 bytes of root constants
inline constexpr uint32 kDrawArgumentsIndex = 4; // kIRArgumentBufferDrawArgumentsBindPoint
inline constexpr uint32 kUniformsIndex = 5;      // kIRArgumentBufferUniformsBindPoint: the index type
inline constexpr uint32 kArgumentTableBuffers = 6;
// Bytes between the index types of DeviceState::uniforms: argument-table
// addresses stay 16-byte aligned, as Metal wants buffer addresses.
inline constexpr uint64 kIndexTypeStride = 16;
// MSC's index types (IRRuntime index types): the slots of DeviceState::uniforms.
inline constexpr uint64 kNonIndexed = 0;
inline constexpr uint64 kIndexUint16 = 1;
inline constexpr uint64 kIndexUint32 = 2;

// Occlusion queries (P115): a rendering section's visibility results go to a
// page of this many 8-byte slots in command memory.
inline constexpr uint32 kOcclusionSlotsPerSection = 4096;
inline constexpr uint64 kVisibilityPageSize = uint64{kOcclusionSlotsPerSection} * 8;
// Metal writes a visibility result at an offset of 256 KiB or more to offset
// 0 instead, silently, also under the debug layer (probed on the M4): pages
// end within the first 256 KiB of their chunk.
inline constexpr uint64 kVisibilityOffsetLimit = 256 * 1024;

// The resolve modes Metal has (P76): depth resolves take sample 0, the
// minimum or the maximum (MTLMultisampleDepthResolveFilter), stencil resolves
// sample 0 only (the other filter follows the depth resolve, no RI mode).
inline constexpr uint32 kDepthResolveModes = resolveModeBit(ResolveMode::eSampleZero) |
                                             resolveModeBit(ResolveMode::eMin) | resolveModeBit(ResolveMode::eMax);
inline constexpr uint32 kStencilResolveModes = resolveModeBit(ResolveMode::eSampleZero);

// One heap slot: MSC's IRDescriptorTableEntry (P114 (3)), as MSC's runtime
// header writes it. Buffers: gpuVA = address, metadata low 32 bits = length.
// Textures: textureViewID = gpuResourceID, metadata = min LOD clamp (float
// bits). Samplers: gpuVA = the sampler's gpuResourceID (not textureViewID),
// metadata = LOD bias (float bits).
struct DescriptorEntry {
    uint64 gpuVA = 0;
    uint64 textureViewID = 0;
    uint64 metadata = 0;
};
static_assert(sizeof(DescriptorEntry) == 24);

// Commit feedback (MTL4CommitFeedback) runs on a Metal thread, and after the
// timeline event has reached the submission's value (probed on the M4: always
// later), so a GPU error is known only once the submission's feedback ran:
// wait and isReached wait for both. Handlers hold this by shared_ptr, so one
// that runs after destroyDevice writes into memory still alive.
struct GpuFeedback {
    std::mutex mutex;
    std::condition_variable arrived;
    uint64 through = 0;         // every submission up to this value has its feedback
    std::set<uint64> early;     // values above `through` whose feedback came first
    std::string error;          // the first GPU error, raised as eDeviceLost
    std::atomic<bool> hasError{false};
};

struct DeviceState {
    MTL::Device *device = nullptr;
    MTL4::CommandQueue *queue = nullptr;
    MTL::ResidencySet *residency = nullptr;
    bool residencyDirty = false; // committed by submit before the next commit to the queue
    MTL::SharedEvent *timeline = nullptr; // the graphics queue's timeline (P20)
    uint64 lastSignaled = 0;

    // Commit feedback of every submission (GpuFeedback): shared with the
    // handlers, which may run after destroyDevice.
    std::shared_ptr<GpuFeedback> feedback = std::make_shared<GpuFeedback>();

    // MSC's uniform at kUniformsIndex, one 16-bit index type per slot of
    // kIndexTypeStride bytes: 0 non-indexed, 1 uint16, 2 uint32 (IRRuntime
    // index types). Fixed, shared by every command list.
    MTL::Buffer *uniforms = nullptr;

    // One sampler per distinct SamplerDesc (P99), created on first write and
    // released with the device: Metal returns the same object for the same
    // description anyway, and RI keeps its reference. Key: samplerKey.
    std::map<std::array<uint32, 13>, MTL::SamplerState *> samplers;

    // Orders counter-heap resolves, which run outside encoders, after the
    // queue's earlier work and before later encoders (Queries.cpp).
    MTL::Fence *resolveFence = nullptr;

    // Occlusion query pools alive: while there are any, each rendering
    // section gets a visibility page (P115), since Metal takes the visibility
    // buffer when the section begins, before RI knows whether it holds queries.
    std::atomic<uint32> occlusionPools{0};

#if EQUINE_VALIDATION
    std::thread::id deviceThread; // the thread that called createDevice (P106)
    Validation::ObjectTracker objects;
    Validation::PlacementTracker placements;
#endif
};

// Command memory (P115): shared-storage chunks owned by one command list,
// growing (64 KiB, then doubling) and recycled at reset.
struct CommandMemoryChunk {
    MTL::Buffer *buffer = nullptr;
    uint64 size = 0;
};

struct CommandMemoryAllocation {
    std::byte *cpu = nullptr;
    uint64 gpuAddress = 0;
    MTL::Buffer *buffer = nullptr; // the chunk, for copies
    uint64 offset = 0;             // in the chunk
};

// One occlusion query of the open section (P115): its slot in the section's
// visibility page, copied to `pool` at `index` by cmdEndRendering.
struct OcclusionSlot {
    MTL::Buffer *pool = nullptr;
    uint32 index = 0;
    uint32 slot = 0;
};

// Recording state of one command list. Metal records through encoders: RI
// keeps one compute encoder open for dispatches, copies and clears, opens a
// render encoder per rendering section, and carries barriers across encoder
// boundaries (P116, proposal §6.2). At most one encoder is open.
struct CommandListState {
    MTL4::CommandAllocator *allocator = nullptr;
    MTL4::CommandBuffer *commandBuffer = nullptr;
    MTL4::ArgumentTable *argumentTable = nullptr; // kArgumentTableBuffers buffer slots
    MTL4::ComputeCommandEncoder *compute = nullptr; // the open compute encoder, or null
    MTL4::RenderCommandEncoder *render = nullptr;   // the open section's encoder, or null

    // Barriers (P36) as MTLStages masks. `pendingBefore/After` were recorded
    // while no encoder was open: the next encoder starts with a queue barrier.
    // `encoderBefore/After` accumulate the barriers recorded in the open
    // encoder: when it closes, they carry over to the next one.
    uint64 pendingBefore = 0, pendingAfter = 0;
    uint64 encoderBefore = 0, encoderAfter = 0;

    std::vector<CommandMemoryChunk> chunks;
    size_t chunk = 0;   // the chunk being filled
    uint64 used = 0;    // bytes used in it

    // Lifecycle (P34, P56): Metal asserts or faults on misuse, so RI checks it
    // in every build. `recorded`: ended by cmdEnd and not submitted since.
    bool recording = false;
    bool recorded = false;
    bool everBegun = false; // the command buffer was begun once (renewCommandBuffer)

    // Bound state.
    bool heapSet = false;
    uint64 resourceHeapAddress = 0, samplerHeapAddress = 0;
    MTL::ComputePipelineState *pipeline = nullptr;
    uint32 threadGroupSize[3] = {1, 1, 1};
    uint32 rootDataSize = 0;
    // The graphics pipeline set last (a copy of the value: its state is
    // applied to each render encoder), kept across sections as on D3D12.
    Pipeline graphics;
    bool graphicsSet = false;
    // The index buffer (cmdSetIndexBuffer), kept until set again.
    uint64 indexAddress = 0, indexLength = 0;
    IndexFormat indexFormat = IndexFormat::eUint16;
    bool indexSet = false;

    // The open rendering section (P40, P103, P104, P75).
    bool rendering = false;
    uint32 renderWidth = 0, renderHeight = 0;
    Viewport viewport;
    Rect scissor;
    uint8 stencilReference = 0;
    bool stencilOverride = false; // cmdSetStencilReference was called in this section
    bool depthReadOnly = false;
    bool sectionDrawn = false;    // a draw was encoded in the section (cmdWriteTimestamp)
    // The section's visibility page (P115): null when no occlusion pool
    // existed as the section began. The query open, if any (Metal counts one
    // at a time), and the queries the section took slots for.
    MTL::Buffer *visibilityBuffer = nullptr;
    uint64 visibilityOffset = 0;
    std::vector<OcclusionSlot> occlusionSlots;
    bool occlusionOpen = false;
    OcclusionSlot openOcclusion;
    // Markers inside a section are the render encoder's debug groups (a
    // command-buffer group must not straddle an encoder): the names of those
    // still open, and how many markers begun before the section ended in it.
    std::vector<std::string> sectionMarkers;
    uint32 deferredPops = 0;
    // A counter-heap resolve ran since the last encoder: the next encoder waits
    // for resolveFence (DeviceState's, borrowed) before its work.
    bool waitResolve = false;
    MTL::Fence *resolveFence = nullptr;
    uint32 markerDepth = 0;
};

// ── Shared helpers (defined in the file named) ───────────────────────────────

// Device.cpp: NS::String from UTF-8 (autoreleased: call inside a pool).
NS::String *nsString(std::string_view text);
// Device.cpp: adds an allocation to the device's residency set (committed by
// the next submit) and removes one.
void makeResident(DeviceState &state, MTL::Allocation *allocation);
void makeNonResident(DeviceState &state, MTL::Allocation *allocation);
// Device.cpp: reports a GPU error recorded by commit feedback, if any.
void checkGpuError(DeviceState &state);
// Commands.cpp: the last timeline value whose submissions completed and
// delivered their commit feedback.
uint64 completedValue(DeviceState &state);
// Device.cpp: reports an NS::Error from a Metal call as `category`.
[[noreturn]] void reportNsError(ErrorCategory category, std::string_view call, NS::Error *error);

// Commands.cpp: the open compute encoder, opened if needed (with the argument
// table set and carried barriers issued). Inside a rendering section it
// reports the call as misplaced: the section's render encoder stays open.
MTL4::ComputeCommandEncoder *computeEncoder(CommandListState &cmd);
// Commands.cpp: opens a render encoder for `pass` (ending the open encoder),
// with the argument table set for every render stage and the carried
// barriers issued. Its fragment work (attachment loads and stores) waits for
// all earlier work, and endEncoder makes every later encoder wait for it: an
// attachment is ordered as D3D12's layout switches order it (P68).
MTL4::RenderCommandEncoder *beginRenderEncoder(CommandListState &cmd, MTL4::RenderPassDescriptor *pass);
// Commands.cpp: ends the open encoder, if any, carrying its barriers.
void endEncoder(CommandListState &cmd);
// Commands.cpp: `size` bytes of command memory aligned to `alignment` (a power
// of two, at least 16 recommended for root data), valid until the list is
// reset, ending at most `limit` bytes into their chunk (visibility pages).
CommandMemoryAllocation allocateCommandMemory(DeviceState &state, CommandListState &cmd, uint64 size,
                                              uint64 alignment, uint64 limit = ~uint64{0});
// Commands.cpp: RI's Stage masks as MTLStages (proposal §6.2).
uint64 stagesOf(Stage stage);
// The stages render encoders run, and that their barriers accept.
inline constexpr uint64 kRenderEncoderStages =
    MTL::StageVertex | MTL::StageObject | MTL::StageMesh | MTL::StageFragment;
// Commands.cpp: encodes a render pass that only loads and stores (a clear
// outside rendering sections): beginRenderEncoder then endEncoder, so it
// orders like D3D12's clear inside its layout switches (P65).
void runClearPass(CommandListState &cmd, MTL4::RenderPassDescriptor *pass);
// Rendering.cpp: cmdSetPipeline's half for graphics pipelines: keeps the
// pipeline and, inside a section, sets its state on the render encoder.
void bindGraphicsPipeline(CommandListState &cmd, const Pipeline &pipeline);
// Rendering.cpp: every build, a draw needs an open section and a pipeline of
// its kind (mesh or vertex): Metal faults otherwise.
void checkDraw(CommandListState &cmd, const char *call, bool mesh);
// Rendering.cpp: points kUniformsIndex at MSC's index type for the next draw.
void bindIndexType(DeviceState &state, CommandListState &cmd, uint64 indexType);
// Queries.cpp: the occlusion part of cmdEndRendering, once the section's
// encoder ended: copies the section's results into their pools (P115).
void copyOcclusionResults(CommandListState &cmd);

// Textures.cpp: RI's formats as Metal's (Invalid for formats Metal lacks).
MTL::PixelFormat pixelFormatOf(Format format);
const char *formatName(Format format);

// Debug.cpp: the native names of an object (setLabel), shared by setName and
// names given at creation (P106).
void applyName(DeviceState &state, const Memory &memory, std::string_view name);
void applyName(DeviceState &state, const Buffer &buffer, std::string_view name);
void applyName(DeviceState &state, const Texture &texture, std::string_view name);
void applyName(DeviceState &state, const TextureView &view, std::string_view name);
void applyName(DeviceState &state, const DescriptorHeap &heap, std::string_view name);
void applyName(DeviceState &state, const QueryPool &pool, std::string_view name);
void applyName(DeviceState &state, const Pipeline &pipeline, std::string_view name);
void applyName(DeviceState &state, const CommandList &cmd, std::string_view name);

// Unsupported.cpp: reports `call` as not built yet on the Metal backend.
[[noreturn]] void notYet(std::string_view call);

// Releases a metal-cpp object and clears the pointer.
template <typename T>
void release(T *&object) {
    if (object != nullptr) {
        object->release();
        object = nullptr;
    }
}

// Validation builds: the id of a new object, and the checks on its use (as
// EQ_DX_* on D3D12).
#if EQUINE_VALIDATION
#define EQ_MTL_TRACK(state) ::RI::Validation::trackObject((state)->objects)
#define EQ_MTL_UNTRACK(state, id) ::RI::Validation::untrackObject((state)->objects, id)
#define EQ_MTL_TRACK_NAME(state, id, name) ::RI::Validation::setTrackedName((state)->objects, id, name)
#define EQ_MTL_CHECK_ALIVE(state, id, kind) EQ_VALIDATE(::RI::Validation::checkAlive((state)->objects, id, kind))
#define EQ_MTL_CHECK_THREAD(state)                                                                       \
    EQ_VALIDATE((state) == nullptr ? ::RI::Validation::CheckResult{}                                     \
                                   : ::RI::Validation::checkDeviceThread((state)->deviceThread, __func__))
#else
#define EQ_MTL_TRACK(state) uint64{0}
#define EQ_MTL_UNTRACK(state, id) ((void)0)
#define EQ_MTL_TRACK_NAME(state, id, name) ((void)0)
#define EQ_MTL_CHECK_ALIVE(state, id, kind) ((void)0)
#define EQ_MTL_CHECK_THREAD(state) ((void)0)
#endif

} // namespace RI::Metal

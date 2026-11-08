#pragma once

// The hidden per-device state behind RI::Metal::Device (P82), heap-allocated
// once per device so Device stays a small value, the per-command-list state,
// and the declarations the backend's sources share (P113-P116).
//
// Keeps: the device and queue, the residency set (every Memory heap, heap
// buffer and command memory chunk: Metal 4 makes nothing resident on its own),
// the timeline event, a small fixed buffer for MSC's index-type uniform, and in
// validation builds the object and placement trackers and the device thread
// (P106). Everything is released with the device.
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

#include <atomic>
#include <condition_variable>
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

// One heap slot: MSC's IRDescriptorTableEntry (P114 (3)). Buffers: gpuVA =
// address, metadata low 32 bits = length. Textures: textureViewID =
// gpuResourceID, metadata = min LOD clamp (float bits). Samplers:
// textureViewID = the sampler's gpuResourceID, metadata = LOD bias.
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

    // MSC's uniform at kUniformsIndex, one 16-bit index type per slot: 0
    // non-indexed, 1 uint16, 2 uint32 (IRRuntime index types). Fixed, shared.
    MTL::Buffer *uniforms = nullptr;

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

// Recording state of one command list. Metal records through encoders: RI
// keeps one compute encoder open for dispatches, copies and clears, and
// carries barriers across encoder boundaries (P116, proposal §6.2).
struct CommandListState {
    MTL4::CommandAllocator *allocator = nullptr;
    MTL4::CommandBuffer *commandBuffer = nullptr;
    MTL4::ArgumentTable *argumentTable = nullptr; // kArgumentTableBuffers buffer slots
    MTL4::ComputeCommandEncoder *compute = nullptr; // the open encoder, or null

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

    // Bound state.
    bool heapSet = false;
    uint64 resourceHeapAddress = 0, samplerHeapAddress = 0;
    MTL::ComputePipelineState *pipeline = nullptr;
    uint32 threadGroupSize[3] = {1, 1, 1};
    uint32 rootDataSize = 0;
    bool rendering = false; // rendering sections: not built yet on Metal
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
// table set and carried barriers issued). Ends an open encoder of another kind.
MTL4::ComputeCommandEncoder *computeEncoder(CommandListState &cmd);
// Commands.cpp: ends the open encoder, if any, carrying its barriers.
void endEncoder(CommandListState &cmd);
// Commands.cpp: `size` bytes of command memory aligned to `alignment` (a power
// of two, at least 16 recommended for root data), valid until the list is reset.
CommandMemoryAllocation allocateCommandMemory(DeviceState &state, CommandListState &cmd, uint64 size,
                                              uint64 alignment);
// Commands.cpp: RI's Stage masks as MTLStages (proposal §6.2).
uint64 stagesOf(Stage stage);

// Debug.cpp: the native names of an object (setLabel), shared by setName and
// names given at creation (P106).
void applyName(DeviceState &state, const Memory &memory, std::string_view name);
void applyName(DeviceState &state, const Buffer &buffer, std::string_view name);
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

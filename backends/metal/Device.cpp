// Device creation on Metal (P60, P92, P113).
//
// Owns: listGpus (Metal 4 and Apple7 GPUs; the id is the registry ID),
// createDevice / destroyDevice, the DeviceLimits, the MTL4 queue, the device's
// residency set (makeResident / makeNonResident), the timeline event, MSC's
// index-type uniforms, and the shared NS helpers (nsString, reportNsError,
// checkGpuError).
//
// Watch out: Metal 4 makes nothing resident on its own; every heap, heap
// buffer and command memory chunk goes through makeResident, and submit
// commits the set when it is dirty. Commit feedback handlers run on a Metal
// thread after the timeline event is signaled: they write into a GpuFeedback
// they share with the device, and destroyDevice waits for all of them, so a
// submission's GPU error is never lost or written after the device is gone.
//
// D3D12 counterpart: backends/d3d12/Device.cpp and Adapter.cpp.

#include "DeviceState.hpp"

#include <RI/Metal/Native.hpp>

#include <algorithm>
#include <cstring>
#include <format>

namespace RI::Metal {

namespace {

// Metal getters and NS::String::string return autoreleased objects: each
// public call that meets them drains its own pool, as RI keeps none (DeviceState.hpp).
struct ScopedPool {
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    ~ScopedPool() { pool->release(); }
};

// "Can run RI" (P113 (2)): Metal 4 on Apple silicon, M1 or later.
bool meetsRequirements(MTL::Device *device) {
    return device->supportsFamily(MTL::GPUFamilyMetal4) && device->supportsFamily(MTL::GPUFamilyApple7);
}

Gpu describeGpu(MTL::Device *device) {
    Gpu gpu{
        .name = device->name() != nullptr ? device->name()->utf8String() : "",
        .vendor = GpuVendor::eOther,
        // Apple silicon shares system memory with the CPU.
        .kind = GpuKind::eIntegrated,
        .vendorId = 0x106B, // Apple
        .deviceId = 0,      // Metal reports no PCI device ID
        // The memory Metal advises the GPU to use without hurting the system,
        // the counterpart of D3D12's shared system memory on UMA.
        .memory = device->recommendedMaxWorkingSetSize(),
    };
    const uint64 registryId = device->registryID();
    static_assert(sizeof(registryId) <= sizeof(gpu.id));
    std::memcpy(gpu.id.data(), &registryId, sizeof(registryId));
    return gpu;
}

// The listed GPU with `gpu.id`, retained, and its description in `found`.
MTL::Device *findGpu(const Gpu &gpu, Gpu &found) {
    NS::Array *devices = MTL::CopyAllDevices();
    MTL::Device *match = nullptr;
    for (NS::UInteger i = 0; devices != nullptr && i < devices->count(); ++i) {
        auto *device = devices->object<MTL::Device>(i);
        if (meetsRequirements(device)) {
            const Gpu described = describeGpu(device);
            if (described.id == gpu.id) {
                found = described;
                match = device->retain();
                break;
            }
        }
    }
    if (devices != nullptr) {
        devices->release();
    }
    if (match == nullptr) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("the GPU '{}' is not available; pass a GPU returned by listGpus()", gpu.name));
    }
    return match;
}

} // namespace

NS::String *nsString(std::string_view text) {
    // NS::String::string reads up to a terminator, which a string_view lacks.
    const std::string copy(text);
    return NS::String::string(copy.c_str(), NS::UTF8StringEncoding);
}

void makeResident(DeviceState &state, MTL::Allocation *allocation) {
    state.residency->addAllocation(allocation);
    state.residencyDirty = true;
}

void makeNonResident(DeviceState &state, MTL::Allocation *allocation) {
    // Committed with the next submission; the GPU no longer uses the
    // allocation (the caller destroys it only after its last use, P20).
    state.residency->removeAllocation(allocation);
    state.residencyDirty = true;
}

void checkGpuError(DeviceState &state) {
    GpuFeedback &feedback = *state.feedback;
    if (!feedback.hasError.load(std::memory_order_acquire)) {
        return;
    }
    std::string message;
    {
        const std::lock_guard lock(feedback.mutex);
        message = feedback.error;
    }
    reportError(ErrorCategory::eDeviceLost, std::format("a Metal command buffer failed on the GPU: {}", message));
}

void reportNsError(ErrorCategory category, std::string_view call, NS::Error *error) {
    ScopedPool pool;
    if (error == nullptr) {
        reportError(category, std::format("{} failed (no error description)", call));
    }
    const NS::String *description = error->localizedDescription();
    const NS::String *domain = error->domain();
    reportError(category, std::format("{} failed: {} ({} {})", call,
                                      description != nullptr ? description->utf8String() : "(no description)",
                                      domain != nullptr ? domain->utf8String() : "", static_cast<int64>(error->code())));
}

std::vector<Gpu> listGpus() {
    ScopedPool pool;
    std::vector<Gpu> gpus;
    NS::Array *devices = MTL::CopyAllDevices();
    for (NS::UInteger i = 0; devices != nullptr && i < devices->count(); ++i) {
        auto *device = devices->object<MTL::Device>(i);
        if (meetsRequirements(device)) {
            gpus.push_back(describeGpu(device));
        }
    }
    if (devices != nullptr) {
        devices->release();
    }
    return gpus;
}

Device createDevice(const DeviceDesc &desc) {
    ScopedPool pool;
    auto *state = new DeviceState{};
#if EQUINE_VALIDATION
    state->deviceThread = std::this_thread::get_id();
#endif
    Device device;
    state->device = findGpu(desc.gpu, device.gpu);

    NS::Error *error = nullptr;
    MTL4::CommandQueueDescriptor *queueDesc = MTL4::CommandQueueDescriptor::alloc()->init();
    queueDesc->setLabel(nsString("EquineRI graphics queue"));
    state->queue = state->device->newMTL4CommandQueue(queueDesc, &error);
    queueDesc->release();
    if (state->queue == nullptr) {
        reportNsError(ErrorCategory::eUnsupported, "newMTL4CommandQueue", error);
    }

    // One set for the device's lifetime, attached to the queue once: RI adds
    // and removes heaps from it (DeviceState.hpp).
    MTL::ResidencySetDescriptor *residencyDesc = MTL::ResidencySetDescriptor::alloc()->init();
    residencyDesc->setLabel(nsString("EquineRI residency"));
    error = nullptr;
    state->residency = state->device->newResidencySet(residencyDesc, &error);
    residencyDesc->release();
    if (state->residency == nullptr) {
        reportNsError(ErrorCategory::eOutOfMemory, "newResidencySet", error);
    }
    state->queue->addResidencySet(state->residency);

    state->timeline = state->device->newSharedEvent();
    if (state->timeline == nullptr) {
        reportError(ErrorCategory::eOutOfMemory, "newSharedEvent failed (the timeline)");
    }
    state->timeline->setLabel(nsString("EquineRI timeline"));
    state->timeline->setSignaledValue(0);

    // MSC's index-type uniform (kUniformsIndex): bound per draw at the slot of
    // the draw's index type, so one fixed buffer serves every command list.
    const uint16 indexTypes[3] = {0, 1, 2};
    state->uniforms = state->device->newBuffer(indexTypes, sizeof(indexTypes),
                                               MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked);
    if (state->uniforms == nullptr) {
        reportError(ErrorCategory::eOutOfMemory, "newBuffer failed (MSC index-type uniforms)");
    }
    state->uniforms->setLabel(nsString("EquineRI index types"));
    makeResident(*state, state->uniforms);

    // Stage 2 of the backend (P116): compute only. Features not built yet are
    // reported false, even where the GPU has them (mesh, ray queries, indirect).
    device.limits = {
        // Heaps are plain buffers of 24-byte entries: no Metal limit binds
        // before memory does; RI keeps D3D12's tier-2 count as the portable size.
        .maxResourceDescriptors = 1000000,
        .maxSamplerDescriptors = 2048, // D3D12's sampler heap
        .maxRootDataSize = kMaxRootDataSize, // the fixed root signature (P114 (1))
        // A heap entry holds the view's length in 32 bits (P114 (3)), so views
        // stop below 4 GiB, and within the device's largest buffer; D3D12's
        // element limit (P106) binds before that for small strides.
        .maxBufferViewSize = std::min<uint64>({kMaxBufferViewElements * 2048, 0xFFFF'FFFFu & ~uint64{15},
                                               state->device->maxBufferLength()}),
        .maxConstantViewSize = 65536,
        .gpuUploadMemory = true, // unified memory: every heap is GPU memory
        .timestampFrequency = state->device->queryTimestampFrequency(),
        .meshShaders = false,
        .geometryShaders = false, // P116 (1): MSC's emulation later
        .tessellation = false,
        .indirectRootData = false,
        .rayTracing = false,
        .workGraphs = false,
        .samplerMinMax = false,
        .int64BufferAtomics = false, // P116 (2)
        .int64SharedAtomics = true,
        .accelerationStructureScratchAlignment = 0,
        .depthResolveModes = 0,
        .stencilResolveModes = 0,
    };

    device.graphicsQueue.internal.device = state;
    device.internal.state = state;
    return device;
}

void destroyDevice(Device &device) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState *state = device.internal.state;
    if (state == nullptr) {
        reportError(ErrorCategory::eProgrammerError, "destroyDevice: this device was never created or already destroyed");
    }
    // Every commit feedback runs before the queue goes: a submission still in
    // flight finishes, and its handler sees the device's GpuFeedback alive.
    {
        GpuFeedback &feedback = *state->feedback;
        std::unique_lock lock(feedback.mutex);
        feedback.arrived.wait(lock, [&] { return feedback.through >= state->lastSignaled; });
    }
    if (state->uniforms != nullptr) {
        state->residency->removeAllocation(state->uniforms);
    }
    release(state->uniforms);
    release(state->timeline);
    if (state->queue != nullptr && state->residency != nullptr) {
        state->queue->removeResidencySet(state->residency);
    }
    release(state->residency);
    release(state->queue);
    release(state->device);
    delete state;
    device.internal.state = nullptr;
    device.graphicsQueue.internal.device = nullptr;
}

NativeDevice native(const Device &device) {
    const DeviceState &state = *device.internal.state;
    return {
        .device = state.device,
        .graphicsQueue = state.queue,
        .residencySet = state.residency,
        .timeline = state.timeline,
    };
}

} // namespace RI::Metal

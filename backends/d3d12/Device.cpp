// Device creation (P60, P92). D3D12 has no feature enable step: every feature
// the GPU supports is available once the device exists. Native additions are a
// hook that runs just before D3D12CreateDevice.

#include "Adapter.hpp"
#include "D3D12Check.hpp"
#include "DeviceState.hpp"

#include <RI/Error.hpp>
#include <RI/D3D12/Native.hpp>

#include <cstdio>
#include <format>

namespace RI::D3D12 {

namespace {

// Validation builds (P21, P53): the debug layer and DRED. Both must be on
// before a device is created on the adapter, and stay on for the process, so
// they are turned on once.
void enableValidationOnce() {
    static bool enabled = false;
    if (enabled || !EQUINE_VALIDATION) {
        return;
    }
    ID3D12Debug *debug = nullptr;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        reportError(ErrorCategory::eUnsupported,
                    "this is a validation build (EQUINE_VALIDATION) but the D3D12 debug layer is unavailable; "
                    "check that the Agility SDK runtime (D3D12\\d3d12SDKLayers.dll) is next to the executable");
    }
    debug->EnableDebugLayer();
    // GPU-based validation (P53 (4)): bindless indices and resource states,
    // checked by instrumented shaders.
    ID3D12Debug1 *debug1 = nullptr;
    if (SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&debug1)))) {
        debug1->SetEnableGPUBasedValidation(TRUE);
        release(debug1);
    }
    release(debug);

    // GPU crash diagnostics (P53 (2)).
    ID3D12DeviceRemovedExtendedDataSettings1 *dred = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        release(dred);
    }
    enabled = true;
}

// Debug-layer messages (P21): errors and corruption go to the error channel as
// programmer errors; warnings are printed.
void CALLBACK onDebugMessage(D3D12_MESSAGE_CATEGORY /*category*/, D3D12_MESSAGE_SEVERITY severity,
                             D3D12_MESSAGE_ID /*id*/, LPCSTR description, void * /*context*/) {
    const char *message = description != nullptr ? description : "(no message)";
    if (severity == D3D12_MESSAGE_SEVERITY_CORRUPTION || severity == D3D12_MESSAGE_SEVERITY_ERROR) {
        reportError(ErrorCategory::eProgrammerError, std::format("D3D12 debug layer: {}", message));
    }
    if (severity == D3D12_MESSAGE_SEVERITY_WARNING) {
        std::fprintf(stderr, "[EquineRI] D3D12 warning: %s\n", message);
    }
}

DeviceState::CpuHeap createCpuHeap(ID3D12Device10 *device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32 count) {
    DeviceState::CpuHeap heap;
    const D3D12_DESCRIPTOR_HEAP_DESC desc{.Type = type, .NumDescriptors = count};
    checkHr(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap.heap)), "CreateDescriptorHeap (RI CPU heap)");
    heap.increment = device->GetDescriptorHandleIncrementSize(type);
    heap.free.reserve(count);
    for (uint32 i = count; i > 0; --i) {
        heap.free.push_back(i - 1);
    }
    return heap;
}

} // namespace

uint32 allocateSlot(DeviceState::CpuHeap &heap, const char *what) {
    if (heap.free.empty()) {
        reportError(ErrorCategory::eOutOfMemory, std::format("RI's {} descriptor heap is full: destroy unused views", what));
    }
    const uint32 slot = heap.free.back();
    heap.free.pop_back();
    return slot;
}

D3D12_CPU_DESCRIPTOR_HANDLE slotHandle(const DeviceState::CpuHeap &heap, uint32 slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap.heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * heap.increment;
    return handle;
}

void beginInternal(DeviceState &state) {
    checkHr(state.internalAllocator->Reset(), "ID3D12CommandAllocator::Reset (internal)");
    checkHr(state.internalList->Reset(state.internalAllocator, nullptr), "ID3D12GraphicsCommandList::Reset (internal)");
}

void endAndWaitInternal(DeviceState &state) {
    checkHr(state.internalList->Close(), "ID3D12GraphicsCommandList::Close (internal)");
    ID3D12CommandList *lists[] = {state.internalList};
    state.graphicsQueue->ExecuteCommandLists(1, lists);
    const uint64 value = ++state.lastSignaled;
    checkHr(state.graphicsQueue->Signal(state.fence, value), "ID3D12CommandQueue::Signal (internal)");
    if (state.fence->GetCompletedValue() < value) {
        checkHr(state.fence->SetEventOnCompletion(value, nullptr), "ID3D12Fence::SetEventOnCompletion (internal)");
    }
}

Device createDevice(const DeviceDesc &desc) { return createDevice(desc, {}); }

Device createDevice(const DeviceDesc &desc, const NativeDeviceAdditions &additions) {
    enableValidationOnce();

    auto *state = new DeviceState{};
    state->factory = createFactory();
    Device device;
    state->adapter = findGpu(state->factory, desc.gpu, device.gpu);

    if (additions.beforeDeviceCreation != nullptr) {
        additions.beforeDeviceCreation(state->adapter, additions.user);
    }
    checkHr(D3D12CreateDevice(state->adapter, kMinFeatureLevel, IID_PPV_ARGS(&state->device)),
            "D3D12CreateDevice (ID3D12Device10: Agility SDK runtime)");

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels{.NumFeatureLevels = 3, .pFeatureLevelsRequested = levels};
    checkHr(state->device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevels, sizeof(featureLevels)),
            "CheckFeatureSupport(FEATURE_LEVELS)");
    state->featureLevel = featureLevels.MaxSupportedFeatureLevel;

    if (EQUINE_VALIDATION) {
        // ID3D12InfoQueue1 comes with the Agility SDK debug layer. Without it,
        // messages still reach the debugger output.
        if (SUCCEEDED(state->device->QueryInterface(IID_PPV_ARGS(&state->infoQueue)))) {
            checkHr(state->infoQueue->RegisterMessageCallback(&onDebugMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr,
                                                              &state->messageCallbackCookie),
                    "ID3D12InfoQueue1::RegisterMessageCallback");
        } else {
            std::fprintf(stderr, "[EquineRI] D3D12 debug messages go to the debugger output only "
                                 "(ID3D12InfoQueue1 unavailable)\n");
        }
    }

    const D3D12_COMMAND_QUEUE_DESC queueDesc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    checkHr(state->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&state->graphicsQueue)), "CreateCommandQueue");
    checkHr(state->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state->fence)), "CreateFence");

    state->resourceDescriptorSize =
        state->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    state->samplerDescriptorSize = state->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    // GPU memory the CPU can write (P32): all-or-nothing on D3D12.
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16{};
    state->gpuUploadHeap =
        SUCCEEDED(state->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options16, sizeof(options16))) &&
        options16.GPUUploadHeapSupported;

    state->rtvHeap = createCpuHeap(state->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvHeapSize);
    state->dsvHeap = createCpuHeap(state->device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvHeapSize);
    const D3D12_DESCRIPTOR_HEAP_DESC clearDesc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                               .NumDescriptors = kClearHeapSize,
                                               .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    const D3D12_DESCRIPTOR_HEAP_DESC clearCpuDesc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                  .NumDescriptors = kClearHeapSize};
    checkHr(state->device->CreateDescriptorHeap(&clearDesc, IID_PPV_ARGS(&state->clearHeap)), "CreateDescriptorHeap");
    checkHr(state->device->CreateDescriptorHeap(&clearCpuDesc, IID_PPV_ARGS(&state->clearCpuHeap)),
            "CreateDescriptorHeap");
    checkHr(state->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&state->internalAllocator)),
            "CreateCommandAllocator (internal)");
    checkHr(state->device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                                              IID_PPV_ARGS(&state->internalList)),
            "CreateCommandList1 (internal)");

    UINT64 timestampFrequency = 0;
    checkHr(state->graphicsQueue->GetTimestampFrequency(&timestampFrequency), "GetTimestampFrequency");

    // Resource binding tier 3 and the root signature's 64-DWORD budget (P44).
    device.limits = {
        .maxResourceDescriptors = D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2,
        .maxSamplerDescriptors = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE,
        .maxRootDataSize = 64 * 4,
        .gpuUploadMemory = state->gpuUploadHeap,
        .timestampFrequency = timestampFrequency,
    };

    device.graphicsQueue.internal.device = state;
    device.internal.state = state;
    return device;
}

void destroyDevice(Device &device) {
    DeviceState *state = device.internal.state;
    if (state == nullptr) {
        reportError(ErrorCategory::eProgrammerError, "destroyDevice: this device was never created or already destroyed");
    }
    release(state->internalList);
    release(state->internalAllocator);
    release(state->clearHeap);
    release(state->clearCpuHeap);
    release(state->rtvHeap.heap);
    release(state->dsvHeap.heap);
    release(state->fence);
    release(state->graphicsQueue);
    if (state->infoQueue != nullptr) {
        state->infoQueue->UnregisterMessageCallback(state->messageCallbackCookie);
        release(state->infoQueue);
    }
    release(state->device);
    release(state->adapter);
    release(state->factory);
    delete state;
    device.internal.state = nullptr;
}

NativeDevice native(const Device &device) {
    const DeviceState &state = *device.internal.state;
    return {
        .factory = state.factory,
        .adapter = state.adapter,
        .device = state.device,
        .graphicsQueue = state.graphicsQueue,
        .featureLevel = state.featureLevel,
    };
}

} // namespace RI::D3D12

// Debugging on D3D12 (P53).
//
// Owns: setName for every object kind, applyName (the names given at
// creation, P106, and setName's), cmdBeginMarker / cmdEndMarker (PIX
// events without the PIX runtime: BeginEvent with a UTF-16 string, the format
// PIX and other tools read), and the device-removed path: reportDeviceRemoved
// prints DRED's breadcrumbs and page-fault details in validation builds.
//
// Vulkan counterpart: backends/vulkan/Debug.cpp.

#include "D3D12Check.hpp"
#include "DeviceState.hpp"

#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>

#include <format>
#include <string>

namespace RI::D3D12 {

namespace {

// UTF-8 to UTF-16, for SetName and PIX events.
std::wstring wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

ID3D12GraphicsCommandList7 *listOf(const CommandList &cmd) {
    return static_cast<ID3D12GraphicsCommandList7 *>(cmd.internal.list);
}

// PIX event metadata for a UTF-16 string (WINPIX_EVENT_UNICODE_VERSION).
constexpr UINT kPixUnicode = 0;

} // namespace

void nameObject(void *object, std::string_view name) {
    if (object != nullptr) {
        static_cast<ID3D12Object *>(object)->SetName(wide(name).c_str());
    }
}

void applyName(const DeviceState &, const Memory &memory, std::string_view name) {
    nameObject(memory.internal.heap, name);
}
void applyName(const DeviceState &, const Buffer &buffer, std::string_view name) {
    nameObject(buffer.internal.resource, name);
}
void applyName(const DeviceState &, const Texture &texture, std::string_view name) {
    nameObject(texture.internal.resource, name);
}
// Views are descriptors on D3D12, not objects: nothing to name.
void applyName(const DeviceState &, const TextureView &, std::string_view) {}
void applyName(const DeviceState &, const DescriptorHeap &heap, std::string_view name) {
    nameObject(heap.internal.resourceHeap, name);
    nameObject(heap.internal.samplerHeap, name);
}
void applyName(const DeviceState &, const QueryPool &pool, std::string_view name) {
    nameObject(pool.internal.heap, name);
}
void applyName(const DeviceState &, const Pipeline &pipeline, std::string_view name) {
    nameObject(pipeline.internal.pipelineState, name);
    nameObject(pipeline.internal.rootSignature, name);
}
void applyName(const DeviceState &, const CommandList &cmd, std::string_view name) {
    nameObject(cmd.internal.list, name);
}
void applyName(const DeviceState &, const AccelerationStructure &structure, std::string_view name) {
    nameObject(structure.internal.resource, name);
}
void applyName(const DeviceState &, const WorkGraph &graph, std::string_view name) {
    nameObject(graph.internal.stateObject, name);
    nameObject(graph.internal.rootSignature, name);
}

void setName(Device &device, const Memory &memory, std::string_view name) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    applyName(*device.internal.state, memory, name);
}
void setName(Device &device, const Buffer &buffer, std::string_view name) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    applyName(*device.internal.state, buffer, name);
}
void setName(Device &device, const Texture &texture, std::string_view name) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    applyName(*device.internal.state, texture, name);
}
void setName(Device &device, const TextureView &view, std::string_view name) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    applyName(*device.internal.state, view, name);
}
void setName(Device &device, const DescriptorHeap &heap, std::string_view name) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    applyName(*device.internal.state, heap, name);
}
void setName(Device &device, const QueryPool &pool, std::string_view name) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    applyName(*device.internal.state, pool, name);
}
void setName(Device &device, const Pipeline &pipeline, std::string_view name) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    applyName(*device.internal.state, pipeline, name);
}
void setName(Device &device, const CommandList &cmd, std::string_view name) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    applyName(*device.internal.state, cmd, name);
}

void cmdBeginMarker(CommandList &cmd, std::string_view name) {
    EQ_DX_CHECK_THREAD(cmd.internal.device);
    ++cmd.internal.markerDepth;
    const std::wstring text = wide(name);
    listOf(cmd)->BeginEvent(kPixUnicode, text.c_str(), static_cast<UINT>((text.size() + 1) * sizeof(wchar_t)));
}

void cmdEndMarker(CommandList &cmd) {
    EQ_DX_CHECK_THREAD(cmd.internal.device);
    EQ_VALIDATE(Validation::checkMarkerEnd(cmd.internal.markerDepth));
    --cmd.internal.markerDepth;
    listOf(cmd)->EndEvent();
}

namespace {

// P53 (2): DRED's breadcrumbs and page-fault report, or "" if DRED is off.
std::string dredDetails(DeviceState &state) {
    ID3D12DeviceRemovedExtendedData1 *dred = nullptr;
    if (FAILED(state.device->QueryInterface(IID_PPV_ARGS(&dred)))) {
        return {};
    }
    std::string text;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs))) {
        for (const D3D12_AUTO_BREADCRUMB_NODE1 *node = breadcrumbs.pHeadAutoBreadcrumbNode; node != nullptr;
             node = node->pNext) {
            const UINT done = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
            if (done >= node->BreadcrumbCount) {
                continue; // this command list finished
            }
            text += std::format("; command list '{}': {} of {} operations done",
                                node->pCommandListDebugNameA != nullptr ? node->pCommandListDebugNameA : "(unnamed)",
                                done, node->BreadcrumbCount);
            if (node->pCommandHistory != nullptr) {
                text += std::format(", next operation D3D12_AUTO_BREADCRUMB_OP {}",
                                    static_cast<int>(node->pCommandHistory[done]));
            }
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pageFault)) && pageFault.PageFaultVA != 0) {
        text += std::format("; page fault at GPU address 0x{:x}", pageFault.PageFaultVA);
        for (const D3D12_DRED_ALLOCATION_NODE1 *node = pageFault.pHeadExistingAllocationNode; node != nullptr;
             node = node->pNext) {
            text += std::format(" (allocation '{}')", node->ObjectNameA != nullptr ? node->ObjectNameA : "(unnamed)");
        }
        for (const D3D12_DRED_ALLOCATION_NODE1 *node = pageFault.pHeadRecentFreedAllocationNode; node != nullptr;
             node = node->pNext) {
            text += std::format(" (recently freed '{}')", node->ObjectNameA != nullptr ? node->ObjectNameA : "(unnamed)");
        }
    }
    release(dred);
    return text;
}

} // namespace

void reportDeviceRemoved(DeviceState &state) {
    const HRESULT reason = state.device->GetDeviceRemovedReason();
    reportError(ErrorCategory::eDeviceLost,
                std::format("the D3D12 device was removed (reason 0x{:08X}){}", static_cast<uint32>(reason),
                            dredDetails(state)));
}

void checkQueueHr(DeviceState &state, HRESULT result, std::string_view call) {
    if (categoryOf(result) == ErrorCategory::eDeviceLost) {
        reportDeviceRemoved(state);
    }
    checkHr(result, call);
}

} // namespace RI::D3D12

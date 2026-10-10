#include "D3D12Check.hpp"

#include <dxgi1_6.h>

#include <format>

namespace RI::D3D12 {

namespace {

std::string_view nameOf(HRESULT result) {
    switch (result) {
    case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
    case E_INVALIDARG: return "E_INVALIDARG";
    case E_NOINTERFACE: return "E_NOINTERFACE";
    case E_NOTIMPL: return "E_NOTIMPL";
    case E_FAIL: return "E_FAIL";
    case DXGI_ERROR_UNSUPPORTED: return "DXGI_ERROR_UNSUPPORTED";
    case DXGI_ERROR_NOT_FOUND: return "DXGI_ERROR_NOT_FOUND";
    case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
    case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_SDK_COMPONENT_MISSING: return "DXGI_ERROR_SDK_COMPONENT_MISSING";
    default: return "HRESULT";
    }
}

} // namespace

ErrorCategory categoryOf(HRESULT result) {
    switch (result) {
    case E_OUTOFMEMORY:
        return ErrorCategory::eOutOfMemory;
    case DXGI_ERROR_DEVICE_REMOVED:
    case DXGI_ERROR_DEVICE_HUNG:
    case DXGI_ERROR_DEVICE_RESET:
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
        return ErrorCategory::eDeviceLost;
    case DXGI_ERROR_UNSUPPORTED:
    case DXGI_ERROR_SDK_COMPONENT_MISSING:
    case E_NOTIMPL:
        return ErrorCategory::eUnsupported;
    default:
        return ErrorCategory::eProgrammerError;
    }
}

void checkHr(HRESULT result, std::string_view call, std::source_location where) {
    if (SUCCEEDED(result)) {
        return;
    }
    reportError(categoryOf(result),
                std::format("{} failed: {} (0x{:08X})", call, nameOf(result), static_cast<uint32>(result)), where);
}

} // namespace RI::D3D12

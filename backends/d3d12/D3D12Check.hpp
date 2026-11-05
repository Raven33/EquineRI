#pragma once

// Turns a failed HRESULT into a report on the error channel (P57), with the
// category that matches the result.

#include <RI/Abi.hpp>
#include <RI/Error.hpp>

#include <d3d12.h>

#include <source_location>
#include <string_view>

namespace RI::inline EQUINE_RI_ABI::D3D12 {

ErrorCategory categoryOf(HRESULT result);

// Reports `call` as failed unless SUCCEEDED(result).
void checkHr(HRESULT result, std::string_view call, std::source_location where = std::source_location::current());

// Releases a COM object and sets the pointer to nullptr.
template <typename T>
void release(T *&object) {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

} // namespace RI::D3D12

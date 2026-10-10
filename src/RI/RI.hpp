#pragma once

// The one header user code includes. It brings in the portable part and the
// backend chosen by the build, then makes `RI::` name that backend (P83): each
// backend lives in its own namespace (`RI::Vk::`, `RI::D3D12::`), and the
// using-directive below lets `RI::createBuffer` find `RI::Vk::createBuffer`.
// Portable types (descriptions, enums, flags) live in `RI::` itself.
//
// A target linking a backend library gets its EQUINE_BACKEND_* macro. Code for
// both backends is compiled once per backend, in separate targets.

#include <RI/Error.hpp>

#if EQUINE_BACKEND_VULKAN && EQUINE_BACKEND_D3D12
#error "Both EQUINE_BACKEND_VULKAN and EQUINE_BACKEND_D3D12 are set: compile RI code once per backend, in separate targets."
#elif EQUINE_BACKEND_VULKAN
#include <RI/Vk/Backend.hpp>
namespace RI {
using namespace Vk;
}
#elif EQUINE_BACKEND_D3D12
#include <RI/D3D12/Backend.hpp>
namespace RI {
using namespace D3D12;
}
#else
#error "No RI backend: link EquineVulkanBackend or EquineD3D12Backend."
#endif

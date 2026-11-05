#pragma once

// D3D12 only loads the Agility SDK runtime when the *executable* exports these
// two symbols; exporting them from a library has no effect. Invoke the macro
// once, at namespace scope, in exactly one .cpp file of each D3D12 executable,
// and call equine_deploy_agility_sdk(<target>) in its CMakeLists.txt.
//
// The exported version comes from the Agility d3d12.h, so it always matches the
// tree's package (cmake/EquineAgilitySDKVersion.cmake, P110): D3D12_SDK_VERSION
// for a retail package, D3D12_PREVIEW_SDK_VERSION for a preview. Every Agility
// header defines both, so only the package name tells a preview apart.

#include <d3d12.h>

#ifndef EQUINE_AGILITY_SDK_VERSION_MINOR
#error "Link EquineAgilitySDK so the tree's Agility SDK headers and version are used."
#endif

#if EQUINE_AGILITY_SDK_PREVIEW
#define EQUINE_D3D12_EXPORTED_SDK_VERSION D3D12_PREVIEW_SDK_VERSION
#else
#define EQUINE_D3D12_EXPORTED_SDK_VERSION D3D12_SDK_VERSION
#endif
static_assert(EQUINE_D3D12_EXPORTED_SDK_VERSION == EQUINE_AGILITY_SDK_VERSION_MINOR,
              "A Windows SDK d3d12.h shadows the tree's Agility SDK header.");

#define EQUINE_D3D12_AGILITY_SDK_EXPORTS()                                                           \
    extern "C" {                                                                                     \
    __declspec(dllexport) extern const UINT D3D12SDKVersion = EQUINE_D3D12_EXPORTED_SDK_VERSION;     \
    __declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";                            \
    }

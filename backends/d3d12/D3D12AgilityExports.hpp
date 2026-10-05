#pragma once

// D3D12 only loads the Agility SDK runtime when the *executable* exports these
// two symbols; exporting them from a library has no effect. Invoke the macro
// once, at namespace scope, in exactly one .cpp file of each D3D12 executable,
// and call equine_deploy_agility_sdk(<target>) in its CMakeLists.txt.
//
// D3D12_SDK_VERSION comes from the Agility d3d12.h, so the exported version
// always matches the pinned package (cmake/EquineAgilitySDK.cmake).

#include <d3d12.h>

#ifndef EQUINE_AGILITY_SDK_VERSION_MINOR
#error "Link EquineAgilitySDK so the pinned Agility SDK headers and version are used."
#endif
static_assert(D3D12_SDK_VERSION == EQUINE_AGILITY_SDK_VERSION_MINOR,
              "A Windows SDK d3d12.h shadows the pinned Agility SDK header.");

#define EQUINE_D3D12_AGILITY_SDK_EXPORTS()                                                  \
    extern "C" {                                                                            \
    __declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION;            \
    __declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";                   \
    }

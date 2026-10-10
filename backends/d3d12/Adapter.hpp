#pragma once

// DXGI factory and adapter description, shared by listGpus and createDevice.
// Each device owns its own factory (P92).

#include <RI/Device.hpp>

#include <d3d12.h>
#include <dxgi1_6.h>

namespace RI::D3D12 {

// RI needs at least feature level 12_0.
inline constexpr D3D_FEATURE_LEVEL kMinFeatureLevel = D3D_FEATURE_LEVEL_12_0;

IDXGIFactory6 *createFactory();

// Fills `gpu` and returns true if the adapter can run RI: feature level 12_0,
// shader model 6.6, resource binding tier 3, enhanced barriers. Creates a
// short-lived device to check (D3D12 returns the existing device if one is
// alive on this adapter).
bool describeGpu(IDXGIAdapter1 *adapter, Gpu &gpu);

// Finds a listed GPU again by its LUID (P92). Reports a programmer error if absent.
// The returned adapter holds a reference.
IDXGIAdapter1 *findGpu(IDXGIFactory6 *factory, const Gpu &gpu, Gpu &found);

} // namespace RI::D3D12

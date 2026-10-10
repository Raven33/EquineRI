#pragma once

// RI formats as DXGI formats. Depth textures that are sampled are created
// typeless, with separate DSV and SRV formats (D3D12 cannot read a depth
// format through an SRV of the same format).

#include <RI/Textures.hpp>

#include <dxgiformat.h>

namespace RI::D3D12 {

DXGI_FORMAT dxgiFormat(Format format);           // the format itself (RTV, UAV, color SRV, PSO formats)
DXGI_FORMAT typelessDepthFormat(Format format);  // resource format of a sampled depth texture
DXGI_FORMAT depthSrvFormat(Format format, bool stencil); // SRV of the depth (or stencil) plane

} // namespace RI::D3D12

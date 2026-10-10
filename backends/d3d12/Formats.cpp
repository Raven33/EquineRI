#include "Formats.hpp"

namespace RI::D3D12 {

DXGI_FORMAT dxgiFormat(Format format) {
    switch (format) {
    case Format::eUndefined: return DXGI_FORMAT_UNKNOWN;
    case Format::eR8Unorm: return DXGI_FORMAT_R8_UNORM;
    case Format::eRG8Unorm: return DXGI_FORMAT_R8G8_UNORM;
    case Format::eRGBA8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::eRGBA8Srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::eBGRA8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case Format::eBGRA8Srgb: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case Format::eRGB10A2Unorm: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case Format::eR11G11B10Float: return DXGI_FORMAT_R11G11B10_FLOAT;
    case Format::eR16Float: return DXGI_FORMAT_R16_FLOAT;
    case Format::eRG16Float: return DXGI_FORMAT_R16G16_FLOAT;
    case Format::eRGBA16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::eR32Float: return DXGI_FORMAT_R32_FLOAT;
    case Format::eRG32Float: return DXGI_FORMAT_R32G32_FLOAT;
    case Format::eRGBA32Float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case Format::eR32Uint: return DXGI_FORMAT_R32_UINT;
    case Format::eRG32Uint: return DXGI_FORMAT_R32G32_UINT;
    case Format::eRGBA32Uint: return DXGI_FORMAT_R32G32B32A32_UINT;
    case Format::eD16Unorm: return DXGI_FORMAT_D16_UNORM;
    case Format::eD32Float: return DXGI_FORMAT_D32_FLOAT;
    case Format::eD24UnormS8Uint: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case Format::eD32FloatS8Uint: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT typelessDepthFormat(Format format) {
    switch (format) {
    case Format::eD16Unorm: return DXGI_FORMAT_R16_TYPELESS;
    case Format::eD32Float: return DXGI_FORMAT_R32_TYPELESS;
    case Format::eD24UnormS8Uint: return DXGI_FORMAT_R24G8_TYPELESS;
    case Format::eD32FloatS8Uint: return DXGI_FORMAT_R32G8X24_TYPELESS;
    default: return dxgiFormat(format);
    }
}

DXGI_FORMAT depthSrvFormat(Format format, bool stencil) {
    switch (format) {
    case Format::eD16Unorm: return DXGI_FORMAT_R16_UNORM;
    case Format::eD32Float: return DXGI_FORMAT_R32_FLOAT;
    case Format::eD24UnormS8Uint: return stencil ? DXGI_FORMAT_X24_TYPELESS_G8_UINT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case Format::eD32FloatS8Uint:
        return stencil ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return dxgiFormat(format);
    }
}

} // namespace RI::D3D12

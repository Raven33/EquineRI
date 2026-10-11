// Portable texture helpers declared in Textures.hpp: format properties
// (depth, stencil, texel size) shared by both backends.

#include <RI/Textures.hpp>

namespace RI {

bool isDepthFormat(Format format) {
    switch (format) {
    case Format::eD16Unorm:
    case Format::eD32Float:
    case Format::eD24UnormS8Uint:
    case Format::eD32FloatS8Uint:
        return true;
    default:
        return false;
    }
}

bool hasStencil(Format format) { return format == Format::eD24UnormS8Uint || format == Format::eD32FloatS8Uint; }

uint32 bytesPerTexel(Format format) {
    switch (format) {
    case Format::eR8Unorm: return 1;
    case Format::eRG8Unorm:
    case Format::eR16Float: return 2;
    case Format::eRGBA8Unorm:
    case Format::eRGBA8Srgb:
    case Format::eBGRA8Unorm:
    case Format::eBGRA8Srgb:
    case Format::eRGB10A2Unorm:
    case Format::eR11G11B10Float:
    case Format::eRG16Float:
    case Format::eR32Float:
    case Format::eR32Uint: return 4;
    case Format::eRGBA16Float:
    case Format::eRG32Float:
    case Format::eRG32Uint: return 8;
    case Format::eRGBA32Float:
    case Format::eRGBA32Uint: return 16;
    default: return 0;
    }
}

uint32 copyBytesPerTexel(Format format, Aspect aspect) {
    if (!isDepthFormat(format)) {
        return bytesPerTexel(format);
    }
    if (aspect == Aspect::eStencil) {
        return hasStencil(format) ? 1 : 0;
    }
    return format == Format::eD16Unorm ? 2 : 4;
}

} // namespace RI

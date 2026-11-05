#pragma once

// One-call upload and readback (P47 helper, P41): each call makes its own
// staging memory and command list, submits, waits, and frees them. Simple and
// slow: for loading and tools, not per-frame streaming.

#include <RI/Abi.hpp>
#include <RI/RI.hpp>

#include <vector>

namespace RI::inline EQUINE_RI_ABI::Helpers {

void uploadBuffer(Device &device, const Buffer &dst, uint64 offset, const void *data, uint64 size);
std::vector<std::byte> readbackBuffer(Device &device, const Buffer &src, uint64 offset, uint64 size);

// Texels of the view (one mip, all its layers; copyLayout's rules), rows
// tightly packed in `texels`: width * bytesPerTexel bytes per row, layers one
// after the other.
void uploadTexture(Device &device, const TextureView &dst, const void *texels);
std::vector<std::byte> readbackTexture(Device &device, const TextureView &src);

} // namespace RI::Helpers

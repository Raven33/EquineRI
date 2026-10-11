#pragma once

// RI's own rules for memory, descriptors, pipelines and command lists
// (P88: only what native layers cannot see). Plain checks returning a
// CheckResult (P91); backends call them through EQ_VALIDATE.

#include <RI/Textures.hpp>
#include <RI/Validation/Validation.hpp>
#include <types.hpp>

#include <string_view>

namespace RI::Validation {

// A descriptor slot inside its heap (P59). RI computes native addresses from
// slots, so an out-of-range slot would silently write past the heap on D3D12.
CheckResult checkSlot(uint32 slot, uint32 count, std::string_view heap);

// Root data passed to cmdSetRootData matches the size the pipeline's shader
// declares (P24, P45).
CheckResult checkRootDataSize(uint64 given, uint32 declared);

// A command list is reset, destroyed or re-recorded only after the GPU has
// finished its last submission (P56).
CheckResult checkCommandListIdle(uint64 lastSubmitted, uint64 completed, std::string_view action);

// A range [offset, offset + size) fits in `total` bytes and `offset` is a
// multiple of `alignment` (resource placement P31, buffer views P87).
CheckResult checkRange(uint64 offset, uint64 size, uint64 total, uint64 alignment, std::string_view what);

// A descriptor heap is set before a pipeline (P94): D3D12 requires the heaps
// bound before a root signature that indexes them, so RI makes it the rule on
// every backend.
CheckResult checkHeapSetBeforePipeline(bool heapSet);

// Buffer views (P87), one rule for both backends (D3D12 addresses views in
// elements and 256-byte constant blocks): read and read-write views start at a
// multiple of 16 bytes and of `stride`, and cover whole elements; constant
// buffer views start at a multiple of 256 and their size, rounded up to 256,
// fits in the buffer. `size` is already resolved (no kWholeSize).
CheckResult checkBufferView(uint64 offset, uint64 size, uint64 bufferSize, uint32 stride, bool constant);

// Rounds a constant buffer view's size up to the 256-byte block (both backends).
inline constexpr uint64 constantViewSize(uint64 size) { return (size + 255) & ~uint64{255}; }

// A command that must be recorded inside (`inside` true) or outside a
// rendering section (P40, P65, P71): `call` names it.
CheckResult checkRenderingSection(bool rendering, bool inside, std::string_view call);

// A view's format reinterprets the texture's (P74): same bytes per texel.
CheckResult checkViewFormatCompatible(Format texture, Format view);

// A view's mip and layer range (counts already resolved) lies inside the texture (P73).
CheckResult checkViewRange(uint32 baseMip, uint32 mipCount, uint32 mips, uint32 baseLayer, uint32 layerCount,
                           uint32 layers);

// eReadOnlyDepthSampling (P97) goes with eSampled, eDepthStencil and a depth format.
CheckResult checkTextureUsage(Format format, TextureUsage usage);

} // namespace RI::Validation

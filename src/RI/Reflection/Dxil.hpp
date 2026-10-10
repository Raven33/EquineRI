#pragma once

// RI's own small DXIL reader (P67). It reads what the D3D12 backend needs to
// build a root signature from compiled bytecode, without dxcompiler or LLVM:
//
//   - SFI0 part: whether the shader indexes the resource or sampler heap (P30);
//   - PSV0 part: the declared resource ranges and the compute thread-group size;
//   - DXIL part: the size of the root data constant buffer (P45), read from the
//     module's `!dx.resources` metadata in the LLVM 3.7 bitcode. That metadata
//     survives `-Qstrip_reflect`, so no reflection part is needed.
//
// The reader only parses bytes, so it is portable and tested on every platform.
// Malformed input yields an error message, never a crash or an out-of-bounds read.

#include <types.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace RI::Reflection {

enum class DxilResourceClass : uint8 {
    eSRV,
    eUAV,
    eCBV,
    eSampler,
};

// One declared (non-heap-indexed) resource range.
struct DxilBinding {
    DxilResourceClass resourceClass = DxilResourceClass::eSRV;
    uint32 space = 0;
    uint32 lowerBound = 0;
    uint32 count = 0; // UINT32_MAX for unbounded arrays
};

struct DxilInfo {
    bool resourceHeapIndexing = false;     // SFI0: shader uses ResourceDescriptorHeap[]
    bool samplerHeapIndexing = false;      // SFI0: shader uses SamplerDescriptorHeap[]
    bool hasRootData = false;              // a CBV at register(b0, space999) (EQ_ROOT_DATA, P45)
    uint32 rootDataSize = 0;               // its size in bytes, from the DXIL part's dx.resources metadata
    std::vector<DxilBinding> bindings;     // every other declared resource (from PSV0), in PSV0 order
    uint32 threadGroupSize[3] = {0, 0, 0}; // compute/mesh/amplification numthreads (PSV0, else metadata); 0 if not
};

struct DxilReadResult {
    DxilInfo info;
    std::string error; // empty on success; otherwise a readable message
};

// SFI0 feature bits (D3D_SHADER_FEATURE_*), verified with DXC 1.9.2609.
inline constexpr uint64 kDxilFeatureResourceDescriptorHeapIndexing = 0x2000000;
inline constexpr uint64 kDxilFeatureSamplerDescriptorHeapIndexing = 0x4000000;

// The register space reserved for EQ_ROOT_DATA (P45).
inline constexpr uint32 kRootDataSpace = 999;

// Reads a DXBC container holding a DXIL shader. Root data rules (P45): the only
// resource allowed in space 999 is one CBV at b0; anything else is an error.
DxilReadResult readDxil(std::span<const std::byte> bytecode);

} // namespace RI::Reflection

#pragma once

// RI's own small DXIL reader (P67). It reads what the D3D12 backend needs to
// build a root signature from compiled bytecode, without dxcompiler or LLVM:
//
//   - SFI0 part: whether the shader indexes the resource or sampler heap (P30),
//     and uses 64-bit atomics on heap resources or groupshared memory (P105);
//   - PSV0 part: the declared resource ranges and the compute thread-group size;
//   - DXIL part: the sizes of the root data constant buffer (P45) and of the
//     bindful constant buffers (P112), read from the module's `!dx.resources`
//     metadata in the LLVM 3.7 bitcode. That metadata survives
//     `-Qstrip_reflect`, so no reflection part is needed;
//   - ISG1, OSG1, PSG1 parts: the input, output and per-primitive output
//     signatures, to check the values passed to the pixel shader (P101).
//
// The reader only parses bytes, so it is portable and tested on every platform.
// Malformed input yields an error message, never a crash or an out-of-bounds read.

#include <RI/Abi.hpp>
#include <RI/Commands.hpp>
#include <types.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace RI::inline EQUINE_RI_ABI::Reflection {

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

// One element of a signature (ISG1, OSG1, PSG1 parts): a value passed between stages.
struct DxilSignatureElement {
    std::string semanticName; // as written, without its index
    uint32 semanticIndex = 0;
    uint32 systemValue = 0; // D3D_NAME: 0 for none, kDxilSystemValuePosition for SV_Position, ...
    uint32 registerIndex = 0;
    uint8 mask = 0; // the components it uses, bit 0 = x
};

// D3D_NAME values RI looks at.
inline constexpr uint32 kDxilSystemValueNone = 0;
inline constexpr uint32 kDxilSystemValuePosition = 1;

struct DxilInfo {
    bool resourceHeapIndexing = false;     // SFI0: shader uses ResourceDescriptorHeap[]
    bool samplerHeapIndexing = false;      // SFI0: shader uses SamplerDescriptorHeap[]
    bool int64HeapAtomics = false;         // SFI0: 64-bit atomics on heap-indexed resources (P105)
    bool int64SharedAtomics = false;       // SFI0: 64-bit atomics on groupshared memory (P105)
    bool hasRootData = false;              // a CBV at register(b0, space999) (EQ_ROOT_DATA, P45)
    uint32 rootDataSize = 0;               // its size in bytes, from the DXIL part's dx.resources metadata
    uint32 constantBuffers = 0;            // EQ_CONSTANT_BUFFER slots, register(b<slot>, space998), as a bitmask (P112)
    uint32 constantBufferSizes[kMaxConstantBuffers] = {}; // their sizes in bytes, from dx.resources
    std::vector<DxilBinding> bindings;     // every other declared resource (from PSV0), in PSV0 order
    uint32 threadGroupSize[3] = {0, 0, 0}; // compute/mesh/amplification numthreads (PSV0, else metadata); 0 if not
    std::vector<DxilSignatureElement> inputs;           // ISG1, in signature order; empty if absent
    std::vector<DxilSignatureElement> outputs;          // OSG1 (a mesh shader's per-vertex outputs)
    std::vector<DxilSignatureElement> primitiveOutputs; // PSG1 (a mesh shader's per-primitive outputs)
};

struct DxilReadResult {
    DxilInfo info;
    std::string error; // empty on success; otherwise a readable message
};

// SFI0 feature bits (D3D_SHADER_FEATURE_*), verified with DXC 1.9.2609.
inline constexpr uint64 kDxilFeatureResourceDescriptorHeapIndexing = 0x2000000;
inline constexpr uint64 kDxilFeatureSamplerDescriptorHeapIndexing = 0x4000000;
inline constexpr uint64 kDxilFeatureAtomicInt64OnGroupShared = 0x800000;
inline constexpr uint64 kDxilFeatureAtomicInt64OnDescriptorHeapResource = 0x10000000;

// The register space reserved for EQ_ROOT_DATA (P45).
inline constexpr uint32 kRootDataSpace = 999;
// The register space reserved for EQ_CONSTANT_BUFFER (P112): CBVs b0 to b7 only.
inline constexpr uint32 kConstantBufferSpace = 998;

// Reads a DXBC container holding a DXIL shader. Root data rules (P45): the only
// resource allowed in space 999 is one CBV at b0; in space 998, single CBVs at
// b0 to b7 (P112); anything else there is an error.
DxilReadResult readDxil(std::span<const std::byte> bytecode);

} // namespace RI::Reflection

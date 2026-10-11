#pragma once

// The L1 measurement harness (docs/LayoutEvaluation.md, "Measurement harness").
// Workloads are lists of passes; each pass names the textures it touches and
// how. The RI path records them with RI calls and coarse barriers, as an RI user
// writes them. The baseline records the same passes with hand-written optimal
// per-texture layouts and exact stages, natively (P79's speed reference).

#include <RI/RI.hpp>

#include <functional>
#include <string>
#include <vector>

namespace bench {

enum class Role : uint8 {
    eColorTarget,   // color attachment
    eDepthTarget,   // depth attachment, written
    eSampled,       // read by shaders (Load)
    eStorage,       // written by a compute shader (all texels)
};

struct Use {
    uint32 texture = 0;    // index into Workload::textures
    Role role = Role::eSampled;
    bool discard = false;  // old contents not needed (cleared or fully overwritten)
};

enum class PassKind : uint8 { eRender, eCompute };

struct Pass {
    std::string name;
    PassKind kind = PassKind::eCompute;
    std::vector<Use> uses;
    // Records the pass's work: pipelines, root data, draws or dispatches. For
    // render passes, attachments are set up by the recorder from `uses`
    // (colors in order, then the depth target), with a clear for discarded ones.
    std::function<void(RI::CommandList &)> record;
};

struct Texture {
    RI::Texture texture;
    RI::TextureView sampledView; // single aspect (depth for depth formats)
    uint32 sampledSlot = 0;
    uint32 storageSlot = 0;
};

struct Workload {
    std::string name;
    std::vector<Texture> textures;
    std::vector<Pass> passes;
};

// Diagnosis (--baseline-variant, Vulkan): the baseline copies one of RI's
// choices, to find which one costs. Comma-separated: "ro" (sampled color in
// READ_ONLY_OPTIMAL), "general" (storage textures sampled in GENERAL), "full"
// (full-scope barriers). Empty: the optimal baseline.
inline std::string baselineVariant;
inline bool hasVariant(const char *name) {
    return ("," + baselineVariant + ",").find("," + std::string(name) + ",") != std::string::npos;
}

// The baseline's state and calls, one implementation per backend.
struct Baseline;
Baseline *createBaseline(RI::Device &device, RI::DescriptorHeap &heap);
void destroyBaseline(Baseline *baseline);
// Writes the descriptors of every texture with the layouts the baseline uses.
void baselineWriteDescriptors(Baseline &baseline, Workload &workload);
// Before a pass: the exact per-texture transitions it needs.
void baselineBeforePass(Baseline &baseline, RI::CommandList &cmd, Workload &workload, const Pass &pass);
// Native rendering section from the pass's uses; ended by baselineEndRendering.
void baselineBeginRendering(Baseline &baseline, RI::CommandList &cmd, Workload &workload, const Pass &pass);
void baselineEndRendering(Baseline &baseline, RI::CommandList &cmd);
// Forgets tracked layouts (new workload).
void baselineReset(Baseline &baseline, Workload &workload);

} // namespace bench

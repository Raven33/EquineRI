#pragma once

// Frame graph helper (P35, P41), first shape. Driven only by declarations
// (P35): you declare each step's resource uses, compile once, then record
// commands yourself between beginStep calls. No pass objects and no
// callbacks: the helper only plans.
//
// The plan: transient textures placed in one memory block, aliasing those
// whose lifetimes do not overlap; a discard before each transient texture's
// first use in a frame (P69); and one coarse barrier per step covering every
// hazard with the steps before it, the previous frame included (P36).

#include <RI/RI.hpp>

#include <vector>

namespace RI::Helpers {

enum class Access : uint8 { eRead, eWrite }; // eWrite also covers read-modify-write

struct ResourceUse {
    uint32 resource = 0; // from addTransientTexture or addExternal
    Stage stage = Stage::eNone;
    Access access = Access::eRead;
};

struct FrameGraph {
    struct Resource {
        bool transient = false;
        TextureDesc desc;
        Texture texture;
        uint64 offset = 0;
        uint64 size = 0;
        uint64 alignment = 0;
        uint32 firstStep = UINT32_MAX;
        uint32 lastStep = 0;
    };
    struct StepPlan {
        Stage before = Stage::eNone;
        Stage after = Stage::eNone;
        std::vector<uint32> discards; // transient resources first used in this step
    };
    Device *device = nullptr;
    std::vector<Resource> resources;
    std::vector<std::vector<ResourceUse>> steps;
    std::vector<StepPlan> plan;
    Memory memory;
    uint64 memorySize = 0;
    bool compiled = false;
};

FrameGraph createFrameGraph(Device &device);
// Waits for nothing: destroy it once the GPU no longer uses its textures.
void destroyFrameGraph(FrameGraph &graph);

// A texture the graph creates and places; its contents last one frame.
uint32 addTransientTexture(FrameGraph &graph, const TextureDesc &desc);
// A resource you own (a buffer, a persistent texture, the screen texture),
// tracked for barriers only.
uint32 addExternal(FrameGraph &graph);
// The next step, in recording order.
uint32 addStep(FrameGraph &graph, std::vector<ResourceUse> uses);

// Places the transient textures (aliasing where lifetimes allow), creates
// them, and plans the barriers. A transient texture read before it is
// written is a programmer error.
void compileFrameGraph(FrameGraph &graph);
const Texture &graphTexture(const FrameGraph &graph, uint32 resource);
// Records the step's planned discards and barrier; then record its commands.
void beginStep(const FrameGraph &graph, CommandList &cmd, uint32 step);

} // namespace RI::Helpers

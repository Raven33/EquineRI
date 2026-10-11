#include <RI/Helpers/FrameGraph.hpp>

#include <algorithm>
#include <format>
#include <numeric>

namespace RI::Helpers {

namespace {

uint64 alignUp(uint64 value, uint64 alignment) { return (value + alignment - 1) / alignment * alignment; }

// Place transient textures: largest first, at the lowest offset that does not
// overlap, in memory, a texture alive in an overlapping range of steps.
void place(FrameGraph &graph) {
    std::vector<uint32> order;
    for (uint32 i = 0; i < graph.resources.size(); ++i) {
        if (graph.resources[i].transient) {
            order.push_back(i);
        }
    }
    std::ranges::sort(order, [&](uint32 a, uint32 b) { return graph.resources[a].size > graph.resources[b].size; });
    std::vector<uint32> placed;
    for (uint32 index : order) {
        FrameGraph::Resource &r = graph.resources[index];
        std::vector<std::pair<uint64, uint64>> taken; // memory ranges of time-overlapping textures
        for (uint32 other : placed) {
            const FrameGraph::Resource &o = graph.resources[other];
            if (r.firstStep <= o.lastStep && o.firstStep <= r.lastStep) {
                taken.emplace_back(o.offset, o.offset + o.size);
            }
        }
        std::ranges::sort(taken);
        uint64 offset = 0;
        for (const auto &[begin, end] : taken) {
            if (alignUp(offset, r.alignment) + r.size <= begin) {
                break;
            }
            offset = std::max(offset, end);
        }
        r.offset = alignUp(offset, r.alignment);
        graph.memorySize = std::max(graph.memorySize, r.offset + r.size);
        placed.push_back(index);
    }
}

// Hazards per step, frames repeating: two passes over the steps, the first
// only to carry the previous frame's last accesses into the second.
void planBarriers(FrameGraph &graph) {
    struct Track {
        Stage lastWrite = Stage::eNone;
        Stage readers = Stage::eNone;
    };
    std::vector<Track> tracks(graph.resources.size());
    graph.plan.assign(graph.steps.size(), {});
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32 s = 0; s < graph.steps.size(); ++s) {
            FrameGraph::StepPlan step;
            for (const ResourceUse &use : graph.steps[s]) {
                Track &t = tracks[use.resource];
                const FrameGraph::Resource &r = graph.resources[use.resource];
                if (r.transient && r.firstStep == s) {
                    // A discard starts the texture's frame: a full barrier of its own.
                    step.discards.push_back(use.resource);
                    t = {};
                }
                if (t.lastWrite != Stage::eNone) { // read or write after write
                    step.before |= t.lastWrite;
                    step.after |= use.stage;
                }
                if (use.access == Access::eWrite && t.readers != Stage::eNone) { // write after read
                    step.before |= t.readers;
                    step.after |= use.stage;
                }
            }
            for (const ResourceUse &use : graph.steps[s]) {
                Track &t = tracks[use.resource];
                if (use.access == Access::eWrite) {
                    t.lastWrite = use.stage;
                    t.readers = Stage::eNone;
                } else {
                    t.readers |= use.stage;
                }
            }
            if (pass == 1) {
                graph.plan[s] = std::move(step);
            }
        }
    }
}

} // namespace

FrameGraph createFrameGraph(Device &device) {
    FrameGraph graph;
    graph.device = &device;
    return graph;
}

void destroyFrameGraph(FrameGraph &graph) {
    for (FrameGraph::Resource &r : graph.resources) {
        if (r.transient && graph.compiled) {
            destroyTexture(*graph.device, r.texture);
        }
    }
    if (graph.compiled && graph.memorySize > 0) {
        freeMemory(*graph.device, graph.memory);
    }
    graph = {};
}

uint32 addTransientTexture(FrameGraph &graph, const TextureDesc &desc) {
    FrameGraph::Resource r;
    r.transient = true;
    r.desc = desc;
    graph.resources.push_back(r);
    return static_cast<uint32>(graph.resources.size() - 1);
}

uint32 addExternal(FrameGraph &graph) {
    graph.resources.push_back({});
    return static_cast<uint32>(graph.resources.size() - 1);
}

uint32 addStep(FrameGraph &graph, std::vector<ResourceUse> uses) {
    if (graph.compiled) {
        reportError(ErrorCategory::eProgrammerError, "Helpers::addStep: the frame graph is already compiled");
    }
    const uint32 step = static_cast<uint32>(graph.steps.size());
    for (const ResourceUse &use : uses) {
        if (use.resource >= graph.resources.size()) {
            reportError(ErrorCategory::eProgrammerError, "Helpers::addStep: unknown resource");
        }
        FrameGraph::Resource &r = graph.resources[use.resource];
        if (r.transient && r.firstStep == UINT32_MAX && use.access == Access::eRead) {
            reportError(ErrorCategory::eProgrammerError,
                        std::format("Helpers::addStep: step {} reads transient texture {} before any step writes it",
                                    step, use.resource));
        }
        r.firstStep = std::min(r.firstStep, step);
        r.lastStep = std::max(r.lastStep, step);
    }
    graph.steps.push_back(std::move(uses));
    return step;
}

void compileFrameGraph(FrameGraph &graph) {
    for (FrameGraph::Resource &r : graph.resources) {
        if (r.transient) {
            const MemoryRequirements req = textureRequirements(*graph.device, r.desc);
            r.size = req.size;
            r.alignment = req.alignment;
            if (r.firstStep == UINT32_MAX) {
                r.firstStep = r.lastStep = 0; // unused: placed alone
            }
        }
    }
    place(graph);
    if (graph.memorySize > 0) {
        graph.memory = allocateMemory(*graph.device, MemoryKind::eGPU, graph.memorySize);
    }
    for (FrameGraph::Resource &r : graph.resources) {
        if (r.transient) {
            r.texture = createTexture(*graph.device, graph.memory, r.offset, r.desc);
        }
    }
    planBarriers(graph);
    graph.compiled = true;
}

const Texture &graphTexture(const FrameGraph &graph, uint32 resource) {
    if (!graph.compiled || resource >= graph.resources.size() || !graph.resources[resource].transient) {
        reportError(ErrorCategory::eProgrammerError,
                    "Helpers::graphTexture: not a transient texture of a compiled graph");
    }
    return graph.resources[resource].texture;
}

void beginStep(const FrameGraph &graph, CommandList &cmd, uint32 step) {
    if (!graph.compiled || step >= graph.plan.size()) {
        reportError(ErrorCategory::eProgrammerError, "Helpers::beginStep: unknown step or graph not compiled");
    }
    const FrameGraph::StepPlan &plan = graph.plan[step];
    for (uint32 resource : plan.discards) {
        cmdDiscard(cmd, graph.resources[resource].texture.view);
    }
    if (plan.before != Stage::eNone) {
        cmdBarrier(cmd, plan.before, plan.after);
    }
}

} // namespace RI::Helpers

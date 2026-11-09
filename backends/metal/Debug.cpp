// Debugging on Metal (P53).
//
// Owns: setName for every object kind, applyName (the names given at
// creation, P106, and setName's; textures' in Textures.cpp), cmdBeginMarker /
// cmdEndMarker (debug groups on the MTL4CommandBuffer, which Xcode's capture
// and Instruments show).
//
// Watch out: a debug group must not straddle an open encoder, so markers end
// the open compute encoder first (the next command opens a new one, carrying
// the barriers); inside a rendering section they are debug groups of its
// render encoder instead (cmdEndRendering carries the open ones over). Compute pipeline labels are fixed at creation
// (MTLComputePipelineDescriptor), so setName on a pipeline names it in
// validation messages only. Texture and view names are applied in Textures.cpp,
// next to the views they label.
//
// D3D12 counterpart: backends/d3d12/Debug.cpp.

#include "DeviceState.hpp"
#include <RI/Validation/Checks.hpp>

namespace RI::Metal {

namespace {

// nsString returns an autoreleased string: drained before the call returns.
struct ScopedPool {
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    ~ScopedPool() { pool->release(); }
};

template <typename T>
void label(void *object, std::string_view name) {
    if (object != nullptr) {
        ScopedPool scope;
        static_cast<T *>(object)->setLabel(nsString(name));
    }
}

} // namespace

void applyName([[maybe_unused]] DeviceState &state, const Memory &memory, std::string_view name) {
    label<MTL::Heap>(memory.internal.heap, name);
    label<MTL::Buffer>(memory.internal.mapBuffer, name);
    EQ_MTL_TRACK_NAME(&state, memory.internal.id, name);
}
void applyName([[maybe_unused]] DeviceState &state, const Buffer &buffer, std::string_view name) {
    label<MTL::Buffer>(buffer.internal.buffer, name);
    EQ_MTL_TRACK_NAME(&state, buffer.internal.id, name);
}
void applyName([[maybe_unused]] DeviceState &state, const DescriptorHeap &heap, std::string_view name) {
    label<MTL::Buffer>(heap.internal.resourceBuffer, name);
    label<MTL::Buffer>(heap.internal.samplerBuffer, name);
    EQ_MTL_TRACK_NAME(&state, heap.internal.id, name);
}
void applyName([[maybe_unused]] DeviceState &state, const QueryPool &pool, std::string_view name) {
    if (pool.type == QueryType::eOcclusion) {
        label<MTL::Buffer>(pool.internal.heap, name);
    } else {
        label<MTL4::CounterHeap>(pool.internal.heap, name);
    }
    EQ_MTL_TRACK_NAME(&state, pool.internal.id, name);
}
// MTLComputePipelineState has no label setter: the name reaches validation messages only.
void applyName([[maybe_unused]] DeviceState &state, [[maybe_unused]] const Pipeline &pipeline,
               [[maybe_unused]] std::string_view name) {
    EQ_MTL_TRACK_NAME(&state, pipeline.internal.id, name);
}
void applyName([[maybe_unused]] DeviceState &state, const CommandList &cmd, std::string_view name) {
    if (cmd.internal.state != nullptr) {
        label<MTL4::CommandBuffer>(cmd.internal.state->commandBuffer, name);
    }
    EQ_MTL_TRACK_NAME(&state, cmd.internal.id, name);
}

void setName(Device &device, const Memory &memory, std::string_view name) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    applyName(*device.internal.state, memory, name);
}
void setName(Device &device, const Buffer &buffer, std::string_view name) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    applyName(*device.internal.state, buffer, name);
}
void setName(Device &device, const Texture &texture, std::string_view name) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    applyName(*device.internal.state, texture, name);
}
void setName(Device &device, const TextureView &view, std::string_view name) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    applyName(*device.internal.state, view, name);
}
void setName(Device &device, const DescriptorHeap &heap, std::string_view name) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    applyName(*device.internal.state, heap, name);
}
void setName(Device &device, const QueryPool &pool, std::string_view name) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    applyName(*device.internal.state, pool, name);
}
void setName(Device &device, const Pipeline &pipeline, std::string_view name) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    applyName(*device.internal.state, pipeline, name);
}
void setName(Device &device, const CommandList &cmd, std::string_view name) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    applyName(*device.internal.state, cmd, name);
}

void cmdBeginMarker(CommandList &cmd, std::string_view name) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    CommandListState &state = *cmd.internal.state;
    ++state.markerDepth;
    ScopedPool scope;
    // Inside a section, the section's encoder holds the group (Rendering.cpp
    // moves it to the command buffer if it is still open at the section's end).
    if (state.render != nullptr) {
        state.render->pushDebugGroup(nsString(name));
        state.sectionMarkers.emplace_back(name);
        return;
    }
    endEncoder(state);
    state.commandBuffer->pushDebugGroup(nsString(name));
}

void cmdEndMarker(CommandList &cmd) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    CommandListState &state = *cmd.internal.state;
    EQ_VALIDATE(Validation::checkMarkerEnd(state.markerDepth));
    if (state.markerDepth == 0) {
        return; // validation off: nothing to close
    }
    --state.markerDepth;
    if (state.render != nullptr) {
        if (!state.sectionMarkers.empty()) {
            state.render->popDebugGroup();
            state.sectionMarkers.pop_back();
        } else {
            ++state.deferredPops; // begun before the section: closed once its encoder ends
        }
        return;
    }
    endEncoder(state);
    state.commandBuffer->popDebugGroup();
}

} // namespace RI::Metal

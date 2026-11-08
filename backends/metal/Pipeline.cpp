// Pipelines on Metal (P38, P44, P45, P114).
//
// Owns: compute pipelines from a .mtl.dxil (the DXIL container plus an "MTLB"
// part holding Metal Shader Converter's metallib): RI's DXIL reader gives the
// facts (numthreads, root data, 64-bit atomics, declared resources), the
// metallib gives the MTL::ComputePipelineState. Graphics pipelines are not
// built yet (Unsupported.cpp).
//
// Watch out: Metal dispatches name threadsPerThreadgroup, so the DXIL's
// numthreads is kept with the pipeline (P114 (2)). A pipeline's label can only
// be set through its descriptor, so the creation name goes there; setName
// after creation only renames the tracked object. 64-bit atomics are checked
// against DeviceLimits in validation builds (P105), as on D3D12.
//
// D3D12 counterpart: backends/d3d12/Pipeline.cpp.

#include "DeviceState.hpp"

#include <RI/Reflection/Dxil.hpp>
#include <RI/Validation/Checks.hpp>

#include <dispatch/dispatch.h>

#include <format>

namespace RI::Metal {

namespace {

// Reads the shader's DXIL and rejects what RI does not set up yet. In
// validation builds, also what the device lacks (P105).
Reflection::DxilInfo reflectStage([[maybe_unused]] const Device &device, Bytecode bytecode, const char *call,
                                  [[maybe_unused]] const char *stage) {
    const Reflection::DxilReadResult read = Reflection::readDxil(bytecode);
    if (!read.error.empty()) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: {} (the Metal backend takes .mtl.dxil: DXIL with the metallib in an MTLB part)",
                                call, read.error));
    }
    EQ_VALIDATE(Validation::checkInt64Atomics(read.info.int64HeapAtomics, read.info.int64SharedAtomics,
                                              device.limits.int64BufferAtomics, device.limits.int64SharedAtomics,
                                              call, stage));
    if (!read.info.bindings.empty()) {
        const Reflection::DxilBinding &b = read.info.bindings.front();
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: the shader declares a resource at register {}{}, space {}: declared resources are "
                                "not supported yet; reach resources through ResourceDescriptorHeap[] (P30)",
                                call, "tucs"[static_cast<int>(b.resourceClass)], b.lowerBound, b.space));
    }
    return read.info;
}

void checkRootDataSize(const Device &device, uint32 size, const char *call) {
    if (size > device.limits.maxRootDataSize) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("{}: the shader's root data is {} bytes; Metal allows {} (P44)", call, size,
                                device.limits.maxRootDataSize));
    }
}

// The metallib of a .mtl.dxil as an MTL::Library (owned).
MTL::Library *newLibrary(DeviceState &state, Bytecode bytecode, const char *call) {
    const std::span<const std::byte> metallib = Reflection::findDxilPart(bytecode, "MTLB");
    if (metallib.empty()) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: the shader has no MTLB part (the Metal backend takes .mtl.dxil: DXIL with the "
                                "metallib from Metal Shader Converter in an MTLB part)",
                                call));
    }
    // The default destructor copies the bytes: the caller's bytecode may go
    // away while Metal still holds the dispatch data.
    dispatch_data_t data = dispatch_data_create(metallib.data(), metallib.size(), nullptr,
                                                DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NS::Error *error = nullptr;
    MTL::Library *library = state.device->newLibrary(data, &error);
    dispatch_release(data);
    if (library == nullptr) {
        reportNsError(ErrorCategory::eProgrammerError, std::format("{}: newLibrary (the MTLB part)", call), error);
    }
    return library;
}

} // namespace

Pipeline createComputePipeline(Device &device, const ComputePipelineDesc &desc) {
    DeviceState &state = *device.internal.state;
    const char *call = "createComputePipeline";
    const Reflection::DxilInfo info = reflectStage(device, desc.shader, call, "compute");
    checkRootDataSize(device, info.rootDataSize, call);

    // Error objects and strings below are autoreleased.
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    MTL::Library *library = newLibrary(state, desc.shader, call);
    MTL::Function *function = library->newFunction(nsString("main"));
    if (function == nullptr) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: the metallib has no function \"main\" (compile the shader with entry point main)",
                                call));
    }
    MTL::ComputePipelineDescriptor *pipelineDesc = MTL::ComputePipelineDescriptor::alloc()->init();
    pipelineDesc->setComputeFunction(function);
    if (!desc.name.empty()) {
        pipelineDesc->setLabel(nsString(desc.name));
    }
    NS::Error *error = nullptr;
    MTL::ComputePipelineState *pipelineState =
        state.device->newComputePipelineState(pipelineDesc, MTL::PipelineOptionNone, nullptr, &error);
    release(pipelineDesc);
    release(function);
    release(library);
    if (pipelineState == nullptr) {
        // reportNsError aborts (P57), so nothing is cleaned up first.
        reportNsError(ErrorCategory::eProgrammerError, std::format("{}: newComputePipelineState", call), error);
    }
    pool->release();

    Pipeline out;
    out.rootDataSize = info.hasRootData ? info.rootDataSize : 0;
    out.internal.pipelineState = pipelineState;
    for (int i = 0; i < 3; ++i) {
        out.internal.threadGroupSize[i] = info.threadGroupSize[i] != 0 ? info.threadGroupSize[i] : 1;
    }
    out.internal.id = EQ_MTL_TRACK(&state);
    if (!desc.name.empty()) {
        applyName(state, out, desc.name);
    }
    return out;
}

void destroyPipeline(Device &device, Pipeline &pipeline) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, pipeline.internal.id, "pipeline");
    EQ_MTL_UNTRACK(&state, pipeline.internal.id);
    if (!pipeline.internal.imported) { // P77: an imported pipeline stays the user's
        auto *object = static_cast<NS::Object *>(pipeline.internal.pipelineState);
        release(object);
    }
    pipeline = {};
}

} // namespace RI::Metal

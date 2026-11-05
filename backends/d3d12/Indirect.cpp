// Indirect execution on D3D12 (P48).
//
// Owns: cmdExecuteIndirect and cmdExecuteIndirectWithRootData through
// ExecuteIndirect, and indirectScratchRequirements (always size 0: D3D12
// needs no scratch).
//
// Keeps: RI's command signatures in DeviceState, per root signature (null for
// Level 1), kind and root data size. Level 2 signatures set the root
// constants (root parameter 0) first. Released when their pipeline is
// destroyed (forgetCommandSignatures) or with the device.
//
// Vulkan counterpart: backends/vulkan/Indirect.cpp.

#include "D3D12Check.hpp"
#include "DeviceState.hpp"

#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>

#include <format>

namespace RI::inline EQUINE_RI_ABI::D3D12 {

namespace {

ID3D12GraphicsCommandList7 *listOf(const CommandList &cmd) {
    return static_cast<ID3D12GraphicsCommandList7 *>(cmd.internal.list);
}

ID3D12CommandSignature *signatureFor(DeviceState &state, IndirectKind kind, ID3D12RootSignature *root,
                                     uint32 rootDataSize) {
    const DeviceState::SignatureKey key{root, static_cast<uint32>(kind), rootDataSize};
    auto found = state.commandSignatures.find(key);
    if (found != state.commandSignatures.end()) {
        return found->second;
    }
    D3D12_INDIRECT_ARGUMENT_DESC arguments[2]{};
    UINT count = 0;
    if (root != nullptr && rootDataSize > 0) {
        arguments[count].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        arguments[count].Constant = {.RootParameterIndex = 0,
                                     .DestOffsetIn32BitValues = 0,
                                     .Num32BitValuesToSet = (rootDataSize + 3) / 4};
        ++count;
    }
    switch (kind) {
    case IndirectKind::eDraw: arguments[count].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW; break;
    case IndirectKind::eDrawIndexed: arguments[count].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED; break;
    case IndirectKind::eDrawMesh: arguments[count].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH; break;
    case IndirectKind::eDispatch: arguments[count].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH; break;
    }
    ++count;
    const D3D12_COMMAND_SIGNATURE_DESC desc{.ByteStride = indirectStride(kind, root != nullptr ? rootDataSize : 0),
                                            .NumArgumentDescs = count,
                                            .pArgumentDescs = arguments};
    ID3D12CommandSignature *signature = nullptr;
    // A root signature only when the signature changes root arguments.
    checkHr(state.device->CreateCommandSignature(&desc, count > 1 ? root : nullptr, IID_PPV_ARGS(&signature)),
            "CreateCommandSignature");
    state.commandSignatures.emplace(key, signature);
    return signature;
}

void execute(CommandList &cmd, IndirectKind kind, const Buffer &args, uint64 offset, uint32 maxCount,
             const Buffer *count, uint64 countOffset, bool level2, [[maybe_unused]] const char *call) {
    DeviceState &state = *cmd.internal.device;
    EQ_DX_CHECK_ALIVE(&state, args.internal.id, "buffer");
    EQ_VALIDATE(Validation::checkConstantBuffersSet(cmd.internal.constantBuffersDeclared, cmd.internal.constantBuffersSet,
                                                    call));
    if (count != nullptr) {
        EQ_DX_CHECK_ALIVE(&state, count->internal.id, "buffer");
    }
    const bool graphics = kind != IndirectKind::eDispatch;
    auto *root = static_cast<ID3D12RootSignature *>(cmd.internal.rootSignature[graphics ? 1 : 0]);
    const uint32 rootDataSize = level2 ? cmd.internal.rootDataSize : 0;
    [[maybe_unused]] const uint32 stride = indirectStride(kind, rootDataSize);
    EQ_VALIDATE(Validation::checkIndirect(
        {.dispatch = !graphics,
         .argsUsage = (args.usage & BufferUsage::eIndirect) != BufferUsage::eNone,
         .offset = offset,
         .stride = stride,
         .argsSize = args.size,
         .maxCount = maxCount,
         .hasCount = count != nullptr,
         .countUsage = count != nullptr && (count->usage & BufferUsage::eIndirect) != BufferUsage::eNone,
         .countOffset = countOffset,
         .countSize = count != nullptr ? count->size : 0,
         .countForDispatchAllowed = level2},
        call));
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, graphics, call));
    ID3D12CommandSignature *signature = signatureFor(state, kind, level2 ? root : nullptr, rootDataSize);
    listOf(cmd)->ExecuteIndirect(signature, maxCount, static_cast<ID3D12Resource *>(args.internal.resource), offset,
                                 count != nullptr ? static_cast<ID3D12Resource *>(count->internal.resource) : nullptr,
                                 countOffset);
}

} // namespace

void cmdExecuteIndirect(CommandList &cmd, IndirectKind kind, const Buffer &args, uint64 offset, uint32 maxCount,
                        const Buffer *count, uint64 countOffset) {
    EQ_DX_CHECK_THREAD(cmd.internal.device);
    execute(cmd, kind, args, offset, maxCount, count, countOffset, false, "cmdExecuteIndirect");
}

// ExecuteIndirect needs no scratch: size 0, so `scratch` may be null.
MemoryRequirements indirectScratchRequirements(Device &device, [[maybe_unused]] const Pipeline &pipeline,
                                               IndirectKind, uint32) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    EQ_DX_CHECK_ALIVE(device.internal.state, pipeline.internal.id, "pipeline");
    return {.size = 0, .alignment = 1, .kinds = bufferRequirements(device, {.size = 256}).kinds};
}

void cmdExecuteIndirectWithRootData(CommandList &cmd, IndirectKind kind, const Buffer &records, uint64 offset,
                                    uint32 maxCount, const Buffer *scratch, uint64, const Buffer *count,
                                    uint64 countOffset) {
    EQ_DX_CHECK_THREAD(cmd.internal.device);
    if (scratch != nullptr) {
        EQ_DX_CHECK_ALIVE(cmd.internal.device, scratch->internal.id, "buffer");
    }
    execute(cmd, kind, records, offset, maxCount, count, countOffset, true, "cmdExecuteIndirectWithRootData");
}

void forgetCommandSignatures(DeviceState &state, void *rootSignature) {
    for (auto it = state.commandSignatures.begin(); it != state.commandSignatures.end();) {
        if (rootSignature == nullptr || it->first.root == rootSignature) {
            it->second->Release();
            it = state.commandSignatures.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace RI::D3D12

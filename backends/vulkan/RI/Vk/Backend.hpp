#pragma once

// The Vulkan backend's portable surface (P83). No native types here (P84):
// native handles are kept as integers or pointers in `internal`, and native
// access goes through <RI/Vk/Native.hpp>.

#include <RI/Commands.hpp>
#include <RI/Device.hpp>
#include <RI/Resources.hpp>

#include <initializer_list>
#include <span>
#include <variant>
#include <vector>

namespace RI::Vk {

struct DeviceState; // backend-private: native handles, function table, tracking

// ── Device and queue (P60, P92, P37, P20) ────────────────────────────────────

// The graphics queue (P37: the only queue in v1). Submissions return
// timeline points (P20).
struct Queue {
    struct {
        DeviceState *device = nullptr;
    } internal;
};

// A thin value (P81): copies refer to the same device. The device owns its
// Vulkan instance (P92).
struct Device {
    Gpu gpu;
    DeviceLimits limits;
    Queue graphicsQueue;
    struct {
        DeviceState *state = nullptr;
    } internal;
};

// The GPUs that can run RI (Vulkan 1.3 and a graphics + compute queue), in the
// driver's order. Picking one automatically is a helper (P60).
std::vector<Gpu> listGpus();

Device createDevice(const DeviceDesc &desc);
void destroyDevice(Device &device);

// ── Memory and buffers (P31, P32, P33, P58, P61) ─────────────────────────────

struct Memory {
    MemoryKind kind = MemoryKind::eGPU;
    uint64 size = 0;
    void *mapped = nullptr; // CPU address for every kind except eGPU (P33)
    struct {
        uint64 memory = 0; // VkDeviceMemory
        uint64 id = 0;     // validation builds: use-after-free tracking (P91)
    } internal;
};

Memory allocateMemory(Device &device, MemoryKind kind, uint64 size);
void freeMemory(Device &device, Memory &memory);

MemoryRequirements bufferRequirements(Device &device, const BufferDesc &desc);

// A buffer placed in user memory at `offset` (P31). It owns no memory.
struct Buffer {
    uint64 size = 0;
    BufferUsage usage = BufferUsage::eNone;
    uint64 gpuAddress = 0;  // always available (P61)
    void *mapped = nullptr; // memory.mapped + offset, or nullptr for eGPU memory
    struct {
        uint64 buffer = 0; // VkBuffer
        uint64 id = 0;
    } internal;
};

Buffer createBuffer(Device &device, const Memory &memory, uint64 offset, const BufferDesc &desc);
void destroyBuffer(Device &device, Buffer &buffer);

// ── Descriptors (P42, P59, P87, P94) ─────────────────────────────────────────

// A resource heap and a sampler heap, written by slot (P42). Writes take effect
// at once; never overwrite a slot that in-flight GPU work may read (P59).
struct DescriptorHeap {
    uint32 resourceCount = 0;
    uint32 samplerCount = 0;
    struct {
        uint64 pool = 0;        // VkDescriptorPool
        uint64 resourceSet = 0; // VkDescriptorSet, set 0
        uint64 samplerSet = 0;  // VkDescriptorSet, set 1
        uint64 id = 0;
    } internal;
};

DescriptorHeap createDescriptorHeap(Device &device, const DescriptorHeapDesc &desc);
void destroyDescriptorHeap(Device &device, DescriptorHeap &heap);

// Buffer views (P87). `stride` 0 is a raw view (ByteAddressBuffer); otherwise
// the element size of a StructuredBuffer. `size` kWholeSize: to the end.
struct ReadView {
    Buffer buffer;
    uint64 offset = 0;
    uint64 size = kWholeSize;
    uint32 stride = 0;
};
struct ReadWriteView {
    Buffer buffer;
    uint64 offset = 0;
    uint64 size = kWholeSize;
    uint32 stride = 0;
};
struct ConstantBufferView {
    Buffer buffer;
    uint64 offset = 0;
    uint64 size = kWholeSize;
};

using Descriptor = std::variant<ReadView, ReadWriteView, ConstantBufferView>;

struct DescriptorWrite {
    uint32 slot = 0;
    Descriptor descriptor;
};

// Writes descriptors into heap slots: one native call for the whole list (D1, P94).
void writeDescriptors(Device &device, DescriptorHeap &heap, std::span<const DescriptorWrite> writes);
void writeDescriptors(Device &device, DescriptorHeap &heap, std::initializer_list<DescriptorWrite> writes);

// ── Pipelines (P38, P45, P67) ────────────────────────────────────────────────

struct Pipeline {
    uint32 rootDataSize = 0; // bytes, from the shader (P44, P45)
    struct {
        uint64 pipeline = 0; // VkPipeline
        uint64 layout = 0;   // VkPipelineLayout
        uint64 id = 0;
    } internal;
};

Pipeline createComputePipeline(Device &device, const ComputePipelineDesc &desc);
void destroyPipeline(Device &device, Pipeline &pipeline);

// ── Command lists and submission (P34, P56, P20, P36, P47) ───────────────────

// One command list with its own native pool (P56). Record, submit once, wait
// for its timeline point, then reset and reuse.
struct CommandList {
    struct {
        DeviceState *device = nullptr;
        void *commandBuffer = nullptr; // VkCommandBuffer
        uint64 pool = 0;               // VkCommandPool
        uint64 boundLayout = 0;        // pipeline layout the heap sets were bound with
        uint64 heapResourceSet = 0;    // heap set by cmdSetHeap, 0 if none
        uint64 heapSamplerSet = 0;
        uint32 rootDataSize = 0;       // of the pipeline set by cmdSetPipeline
        uint64 lastSubmitted = 0;      // timeline value of its last submission
        uint64 id = 0;
    } internal;
};

CommandList createCommandList(Device &device);
void destroyCommandList(Device &device, CommandList &cmd);
void resetCommandList(CommandList &cmd);

void cmdBegin(CommandList &cmd);
void cmdEnd(CommandList &cmd);
void cmdSetHeap(CommandList &cmd, const DescriptorHeap &heap);
void cmdSetPipeline(CommandList &cmd, const Pipeline &pipeline);
void cmdSetRootData(CommandList &cmd, const void *data, uint32 size);
template <typename T>
void cmdSetRootData(CommandList &cmd, const T &data) {
    cmdSetRootData(cmd, &data, static_cast<uint32>(sizeof(T)));
}
void cmdDispatch(CommandList &cmd, uint32 x, uint32 y, uint32 z);
void cmdBarrier(CommandList &cmd, Stage before, Stage after);
void cmdCopyBuffer(CommandList &cmd, const Buffer &src, uint64 srcOffset, const Buffer &dst, uint64 dstOffset,
                   uint64 size);

TimelinePoint submit(Queue &queue, CommandList &cmd);
void wait(Queue &queue, TimelinePoint point);
bool isReached(Queue &queue, TimelinePoint point);

} // namespace RI::Vk

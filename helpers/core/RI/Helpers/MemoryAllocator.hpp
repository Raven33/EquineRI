#pragma once

// Memory allocator helper (P41, P31): places buffers and textures in blocks
// of each memory kind, first fit, so you never compute offsets. Frees are
// deferred to a timeline point (P20). Built only on RI's public functions.

#include <RI/RI.hpp>

#include <vector>

namespace RI::Helpers {

struct Allocation {
    Memory memory; // the block
    uint64 offset = 0;
    uint64 size = 0;
    MemoryKind kind = MemoryKind::eGPU;
    uint32 block = 0;
    // The range held another resource before: a texture placed here needs
    // cmdDiscard before its first use (P69).
    bool needsDiscard = false;
};

struct MemoryAllocator {
    struct Range {
        uint64 offset = 0;
        uint64 size = 0;
    };
    struct Block {
        Memory memory;
        std::vector<Range> free;  // sorted by offset
        uint64 highWater = 0;     // bytes below this were used at some point
    };
    struct Pending {
        Allocation allocation;
        TimelinePoint point;
    };
    Device *device = nullptr;
    uint64 blockSize = 0;
    std::vector<Block> blocks[4]; // per MemoryKind
    std::vector<Pending> pending; // released, waiting for their timeline point
};

// Blocks of `blockSize` bytes; larger requests get a block of their own.
MemoryAllocator createMemoryAllocator(Device &device, uint64 blockSize = 64ull << 20);
void destroyMemoryAllocator(MemoryAllocator &allocator);

Allocation allocate(MemoryAllocator &allocator, MemoryKind kind, const MemoryRequirements &requirements);
// The range becomes free once `lastUse` is reached on the graphics queue
// (a default point: at once).
void release(MemoryAllocator &allocator, const Allocation &allocation, TimelinePoint lastUse = {});

// Conveniences: requirements, allocation and creation in one call.
Buffer createBuffer(MemoryAllocator &allocator, MemoryKind kind, const BufferDesc &desc, Allocation &allocation);
Texture createTexture(MemoryAllocator &allocator, const TextureDesc &desc, Allocation &allocation);

} // namespace RI::Helpers

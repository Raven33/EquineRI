// Implements MemoryAllocator.hpp (P41, P31): first-fit placement in blocks per
// memory kind, frees deferred to timeline points, on RI's public functions.

#include <RI/Helpers/MemoryAllocator.hpp>

#include <algorithm>
#include <format>

namespace RI::Helpers {

namespace {

uint64 alignUp(uint64 value, uint64 alignment) { return (value + alignment - 1) / alignment * alignment; }

void freeRange(MemoryAllocator::Block &block, uint64 offset, uint64 size) {
    auto &free = block.free;
    auto it = std::ranges::lower_bound(free, offset, {}, &MemoryAllocator::Range::offset);
    it = free.insert(it, {offset, size});
    // Merge with the next and the previous range.
    if (it + 1 != free.end() && it->offset + it->size == (it + 1)->offset) {
        it->size += (it + 1)->size;
        free.erase(it + 1);
    }
    if (it != free.begin() && (it - 1)->offset + (it - 1)->size == it->offset) {
        (it - 1)->size += it->size;
        free.erase(it);
    }
}

// Ranges whose timeline point is reached become free.
void collect(MemoryAllocator &allocator) {
    auto &pending = allocator.pending;
    for (size_t i = 0; i < pending.size();) {
        if (isReached(allocator.device->graphicsQueue, pending[i].point)) {
            const Allocation &a = pending[i].allocation;
            freeRange(allocator.blocks[static_cast<uint32>(a.kind)][a.block], a.offset, a.size);
            pending[i] = pending.back();
            pending.pop_back();
        } else {
            ++i;
        }
    }
}

bool tryPlace(MemoryAllocator::Block &block, const MemoryRequirements &req, Allocation &out) {
    for (size_t i = 0; i < block.free.size(); ++i) {
        MemoryAllocator::Range &range = block.free[i];
        const uint64 offset = alignUp(range.offset, req.alignment);
        if (offset + req.size > range.offset + range.size) {
            continue;
        }
        out.offset = offset;
        out.size = req.size;
        out.memory = block.memory;
        out.needsDiscard = offset < block.highWater;
        block.highWater = std::max(block.highWater, offset + req.size);
        // Split the range: before (alignment padding) and after.
        const MemoryAllocator::Range before{range.offset, offset - range.offset};
        const MemoryAllocator::Range after{offset + req.size, range.offset + range.size - offset - req.size};
        block.free.erase(block.free.begin() + static_cast<std::ptrdiff_t>(i));
        if (after.size > 0) {
            block.free.insert(block.free.begin() + static_cast<std::ptrdiff_t>(i), after);
        }
        if (before.size > 0) {
            block.free.insert(block.free.begin() + static_cast<std::ptrdiff_t>(i), before);
        }
        return true;
    }
    return false;
}

} // namespace

MemoryAllocator createMemoryAllocator(Device &device, uint64 blockSize) {
    MemoryAllocator allocator;
    allocator.device = &device;
    allocator.blockSize = blockSize;
    return allocator;
}

void destroyMemoryAllocator(MemoryAllocator &allocator) {
    for (auto &blocks : allocator.blocks) {
        for (MemoryAllocator::Block &block : blocks) {
            freeMemory(*allocator.device, block.memory);
        }
    }
    allocator = {};
}

Allocation allocate(MemoryAllocator &allocator, MemoryKind kind, const MemoryRequirements &requirements) {
    if (!fits(requirements, kind)) {
        reportError(ErrorCategory::eProgrammerError,
                    "Helpers::allocate: the resource cannot be placed in this memory kind (MemoryRequirements::kinds)");
    }
    collect(allocator);
    auto &blocks = allocator.blocks[static_cast<uint32>(kind)];
    Allocation out;
    out.kind = kind;
    for (uint32 i = 0; i < blocks.size(); ++i) {
        if (tryPlace(blocks[i], requirements, out)) {
            out.block = i;
            return out;
        }
    }
    MemoryAllocator::Block block;
    const uint64 size = std::max(allocator.blockSize, alignUp(requirements.size, requirements.alignment));
    block.memory = allocateMemory(*allocator.device, kind, size);
    block.free.push_back({0, size});
    blocks.push_back(block);
    out.block = static_cast<uint32>(blocks.size() - 1);
    tryPlace(blocks.back(), requirements, out);
    return out;
}

void release(MemoryAllocator &allocator, const Allocation &allocation, TimelinePoint lastUse) {
    allocator.pending.push_back({allocation, lastUse});
    collect(allocator);
}

Buffer createBuffer(MemoryAllocator &allocator, MemoryKind kind, const BufferDesc &desc, Allocation &allocation) {
    allocation = allocate(allocator, kind, bufferRequirements(*allocator.device, desc));
    return RI::createBuffer(*allocator.device, allocation.memory, allocation.offset, desc);
}

Texture createTexture(MemoryAllocator &allocator, const TextureDesc &desc, Allocation &allocation) {
    allocation = allocate(allocator, MemoryKind::eGPU, textureRequirements(*allocator.device, desc));
    return RI::createTexture(*allocator.device, allocation.memory, allocation.offset, desc);
}

} // namespace RI::Helpers

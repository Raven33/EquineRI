#pragma once

// Descriptor slot helper (P41, P42, P59): hands out heap slots and takes them
// back only after the GPU work that may read them is done.

#include <RI/RI.hpp>

#include <vector>

namespace RI::Helpers {

struct SlotAllocator {
    struct Pending {
        uint32 slot = 0;
        TimelinePoint point;
    };
    Queue *queue = nullptr;
    uint32 first = 0;
    uint32 count = 0;
    uint32 next = 0;            // never handed out yet: next .. first + count
    std::vector<uint32> free;   // released and safe to reuse
    std::vector<Pending> pending;
};

// Slots `first` .. `first + count - 1` of a heap, reused after their last use
// on `queue` is reached.
SlotAllocator createSlotAllocator(Queue &queue, uint32 first, uint32 count);
// A free slot; a programmer error when every slot is in use.
uint32 allocateSlot(SlotAllocator &slots);
// The slot is reused once `lastUse` is reached (P59: never overwrite a slot
// in-flight work may read).
void releaseSlot(SlotAllocator &slots, uint32 slot, TimelinePoint lastUse);

} // namespace RI::Helpers

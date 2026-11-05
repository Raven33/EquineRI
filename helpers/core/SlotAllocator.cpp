// Implements SlotAllocator.hpp (P41, P59): heap slots reused only after their
// last use's timeline point.

#include <RI/Helpers/SlotAllocator.hpp>

#include <format>

namespace RI::inline EQUINE_RI_ABI::Helpers {

SlotAllocator createSlotAllocator(Queue &queue, uint32 first, uint32 count) {
    SlotAllocator slots;
    slots.queue = &queue;
    slots.first = first;
    slots.count = count;
    slots.next = first;
    return slots;
}

uint32 allocateSlot(SlotAllocator &slots) {
    for (size_t i = 0; i < slots.pending.size();) {
        if (isReached(*slots.queue, slots.pending[i].point)) {
            slots.free.push_back(slots.pending[i].slot);
            slots.pending[i] = slots.pending.back();
            slots.pending.pop_back();
        } else {
            ++i;
        }
    }
    if (!slots.free.empty()) {
        const uint32 slot = slots.free.back();
        slots.free.pop_back();
        return slot;
    }
    if (slots.next < slots.first + slots.count) {
        return slots.next++;
    }
    reportError(ErrorCategory::eProgrammerError,
                std::format("Helpers::allocateSlot: all {} slots are in use or waiting for the GPU", slots.count));
}

void releaseSlot(SlotAllocator &slots, uint32 slot, TimelinePoint lastUse) {
    slots.pending.push_back({slot, lastUse});
}

} // namespace RI::Helpers

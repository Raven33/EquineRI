#pragma once

// Portable recording and submission types (CONTEXT.md P20, P34, P36, P56).

#include <RI/Flags.hpp>
#include <types.hpp>

namespace RI {

// Pipeline stages for coarse barriers (P36): a barrier from `before` to
// `after` makes every write of the `before` stages visible to every access of
// the `after` stages. Graphics stages come with graphics pipelines.
enum class Stage : uint32 {
    eNone = 0,
    eCopy = rvl::eBit(0),
    eCompute = rvl::eBit(1),
    eIndirect = rvl::eBit(2), // reading indirect arguments (P48)
    eAll = rvl::eBit(31),
};
makeBitmask(Stage)

// A point on a queue's GPU timeline (P20): reached when the GPU has finished
// the submission that returned it. Values only grow; 0 is always reached.
struct TimelinePoint {
    uint64 value = 0;
};

} // namespace RI

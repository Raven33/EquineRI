// Implements Grid.hpp: folding group counts into rows (P106).

#include <RI/Helpers/Grid.hpp>

#include <format>

namespace RI::inline EQUINE_RI_ABI::Helpers {

DispatchIndirectArgs groupGrid(uint64 count, uint32 groupSize) {
    if (groupSize == 0) {
        reportError(ErrorCategory::eProgrammerError, "groupGrid: the group size is 0");
    }
    const uint64 groups = (count + groupSize - 1) / groupSize;
    const uint64 width = kMaxGroupsPerDimension;
    if (groups <= width) {
        return {.x = static_cast<uint32>(groups), .y = 1, .z = 1};
    }
    const uint64 rows = (groups + width - 1) / width;
    if (rows > width) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("groupGrid: {} groups do not fit in {} x {} groups", groups, width, width));
    }
    return {.x = static_cast<uint32>(width), .y = static_cast<uint32>(rows), .z = 1};
}

} // namespace RI::Helpers

#pragma once

// Group grids (P106): a dispatch or mesh draw has at most
// kMaxGroupsPerDimension groups per dimension on every backend, so a large
// count is folded into rows. Shaders recover the linear group index with
// eqLinearGroup (<equine/grid.hlsli>).

#include <RI/Abi.hpp>
#include <RI/RI.hpp>

namespace RI::inline EQUINE_RI_ABI::Helpers {

// The groups for `count` items of `groupSize` each: {groups, 1, 1} up to
// kMaxGroupsPerDimension groups, then rows of kMaxGroupsPerDimension. The last
// row may launch groups past the end; shaders skip them by comparing the
// linear index with the group count.
DispatchIndirectArgs groupGrid(uint64 count, uint32 groupSize);

} // namespace RI::Helpers

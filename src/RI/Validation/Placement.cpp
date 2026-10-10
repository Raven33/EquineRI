#include <RI/Validation/Placement.hpp>

#include <algorithm>
#include <format>

namespace RI::Validation {

void placeResource(PlacementTracker &tracker, uint64 memoryId, uint64 offset, uint64 size, uint64 resourceId,
                   bool isTexture) {
    std::vector<PlacementTracker::Range> &ranges = tracker.used[memoryId];
    const bool overlaps = std::ranges::any_of(ranges, [&](const PlacementTracker::Range &r) {
        return offset < r.offset + r.size && r.offset < offset + size;
    });
    if (isTexture && overlaps) {
        tracker.needsDiscard.insert(resourceId);
    }
    ranges.push_back({offset, size});
}

void markDiscarded(PlacementTracker &tracker, uint64 textureId) { tracker.needsDiscard.erase(textureId); }

void forgetMemory(PlacementTracker &tracker, uint64 memoryId) { tracker.used.erase(memoryId); }

CheckResult checkDiscarded(const PlacementTracker &tracker, uint64 textureId, std::string_view use) {
    if (tracker.needsDiscard.contains(textureId)) {
        return std::format("{}: the texture was placed over memory that held another resource; record cmdDiscard "
                           "before its first use (P69; load operations do not count)",
                           use);
    }
    return {};
}

CheckResult checkDepthWriteAllowed(bool sectionDepthReadOnly, bool pipelineWritesDepth) {
    if (sectionDepthReadOnly && pipelineWritesDepth) {
        return std::string("cmdSetPipeline: the pipeline writes depth, but the rendering section's depth is "
                           "read-only (P75)");
    }
    return {};
}

} // namespace RI::Validation

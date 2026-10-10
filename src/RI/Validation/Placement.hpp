#pragma once

// Discard tracking for placed resources (P69), validation builds only. A
// texture placed over memory that another resource (alive or destroyed) has
// occupied must be discarded before its first use; RI checks this at the uses
// it sees (rendering attachments, clears). Shader uses are invisible under
// bindless and are not checked.

#include <RI/Validation/Validation.hpp>
#include <types.hpp>

#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RI::Validation {

struct PlacementTracker {
    struct Range {
        uint64 offset = 0;
        uint64 size = 0;
    };
    std::unordered_map<uint64, std::vector<Range>> used; // memory id -> every range ever placed
    std::unordered_set<uint64> needsDiscard;             // texture ids
};

// Records a resource placed in memory. A texture overlapping an earlier
// placement must be discarded first.
void placeResource(PlacementTracker &tracker, uint64 memoryId, uint64 offset, uint64 size, uint64 resourceId,
                   bool isTexture);
void markDiscarded(PlacementTracker &tracker, uint64 textureId);
void forgetMemory(PlacementTracker &tracker, uint64 memoryId);

// `use` names the RI call ("cmdBeginRendering", "cmdClearTexture").
CheckResult checkDiscarded(const PlacementTracker &tracker, uint64 textureId, std::string_view use);

// A pipeline that writes depth is not used in a read-only depth section (P75).
CheckResult checkDepthWriteAllowed(bool sectionDepthReadOnly, bool pipelineWritesDepth);

} // namespace RI::Validation

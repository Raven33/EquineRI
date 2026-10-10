#include <RI/Validation/ObjectTracker.hpp>

#include <atomic>
#include <format>

namespace RI::Validation {

namespace {
std::atomic<uint64> nextObjectId{1};
} // namespace

uint64 trackObject(ObjectTracker &tracker) {
    const uint64 id = nextObjectId.fetch_add(1, std::memory_order_relaxed);
    tracker.live.insert(id);
    return id;
}

void untrackObject(ObjectTracker &tracker, uint64 id) { tracker.live.erase(id); }

CheckResult checkAlive(const ObjectTracker &tracker, uint64 id, std::string_view kind) {
    if (id == 0) {
        return std::format("this {} was never created (default-constructed or zeroed)", kind);
    }
    if (!tracker.live.contains(id)) {
        return std::format("this {} was destroyed or belongs to another device (object #{})", kind, id);
    }
    return {};
}

} // namespace RI::Validation

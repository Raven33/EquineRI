// Implements ObjectTracker.hpp: the ids and names of live objects, per device
// (P81, P91, P106).

#include <RI/Validation/ObjectTracker.hpp>

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>

namespace RI::Validation {

namespace {
std::atomic<uint64> nextObjectId{1};
} // namespace

uint64 trackObject(ObjectTracker &tracker) {
    const uint64 id = nextObjectId.fetch_add(1, std::memory_order_relaxed);
    const std::unique_lock lock(tracker.mutex);
    tracker.live.emplace(id, std::string{});
    return id;
}

void setTrackedName(ObjectTracker &tracker, uint64 id, std::string_view name) {
    const std::unique_lock lock(tracker.mutex);
    if (const auto found = tracker.live.find(id); found != tracker.live.end()) {
        found->second = name;
    }
}

void untrackObject(ObjectTracker &tracker, uint64 id) {
    const std::unique_lock lock(tracker.mutex);
    const auto found = tracker.live.find(id);
    if (found == tracker.live.end()) {
        return;
    }
    if (!found->second.empty()) {
        if (tracker.destroyed.size() == kDestroyedNames) {
            tracker.destroyed.pop_front();
        }
        tracker.destroyed.emplace_back(id, std::move(found->second));
    }
    tracker.live.erase(found);
}

CheckResult checkAlive(const ObjectTracker &tracker, uint64 id, std::string_view kind) {
    if (id == 0) {
        return std::format("this {} was never created (default-constructed or zeroed)", kind);
    }
    const std::shared_lock lock(tracker.mutex);
    if (tracker.live.contains(id)) {
        return {};
    }
    const auto named = std::ranges::find(tracker.destroyed, id, &std::pair<uint64, std::string>::first);
    if (named != tracker.destroyed.end()) {
        return std::format("this {} '{}' was destroyed (object #{})", kind, named->second, id);
    }
    return std::format("this {} was destroyed or belongs to another device (object #{})", kind, id);
}

} // namespace RI::Validation

#pragma once

// Use-after-destroy detection for thin value objects (P81): RI objects are
// copyable and own nothing, so a destroyed object's copies stay around. In
// validation builds each object carries an id in its `internal` field; the
// device's tracker holds the ids still alive. Copies share the id, so destroying
// any copy makes all of them dead. Ids come from one process-wide counter, so
// they never repeat across devices and an object from another device is caught.
// The tracker is locked: objects of the any-thread list (P106) are created on
// other threads while the device thread checks liveness. It also keeps each
// object's name (given at creation or by setName), and the names of the last
// kDestroyedNames named objects destroyed, so a use after destroy is reported
// by name.

#include <RI/Validation/Validation.hpp>
#include <types.hpp>

#include <cstddef>
#include <deque>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace RI::Validation {

inline constexpr size_t kDestroyedNames = 4096;

struct ObjectTracker {
    // Live objects and their names (empty: unnamed). Id 0 is never handed out:
    // a default-constructed object is never alive.
    std::unordered_map<uint64, std::string> live;
    std::deque<std::pair<uint64, std::string>> destroyed; // named objects only, oldest first
    mutable std::shared_mutex mutex;
};

// Registers a newly created object and returns its id.
uint64 trackObject(ObjectTracker &tracker);

// Names a live object (an unknown id does nothing): names at creation and setName (P106).
void setTrackedName(ObjectTracker &tracker, uint64 id, std::string_view name);

// Forgets a destroyed object. Run checkAlive first: untracking an unknown id does nothing.
void untrackObject(ObjectTracker &tracker, uint64 id);

// Fails if the object was destroyed, never created, or created by another device.
// `kind` names the object in the message ("buffer", "texture", …).
CheckResult checkAlive(const ObjectTracker &tracker, uint64 id, std::string_view kind);

} // namespace RI::Validation

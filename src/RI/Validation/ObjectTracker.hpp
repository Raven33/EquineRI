#pragma once

// Use-after-destroy detection for thin value objects (P81): RI objects are
// copyable and own nothing, so a destroyed object's copies stay around. In
// validation builds each object carries an id in its `internal` field; the
// device's tracker holds the ids still alive. Copies share the id, so destroying
// any copy makes all of them dead. Ids come from one process-wide counter, so
// they never repeat across devices and an object from another device is caught.

#include <RI/Validation/Validation.hpp>
#include <types.hpp>

#include <string_view>
#include <unordered_set>

namespace RI::Validation {

struct ObjectTracker {
    std::unordered_set<uint64> live; // id 0 is never handed out: a default-constructed object is never alive
};

// Registers a newly created object and returns its id.
uint64 trackObject(ObjectTracker &tracker);

// Forgets a destroyed object. Run checkAlive first: untracking an unknown id does nothing.
void untrackObject(ObjectTracker &tracker, uint64 id);

// Fails if the object was destroyed, never created, or created by another device.
// `kind` names the object in the message ("buffer", "texture", …).
CheckResult checkAlive(const ObjectTracker &tracker, uint64 id, std::string_view kind);

} // namespace RI::Validation

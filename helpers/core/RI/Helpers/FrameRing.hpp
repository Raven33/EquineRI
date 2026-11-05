#pragma once

// Frames in flight (P41, P43, P20): N command lists used in turn. The CPU
// records frame n + 1 while the GPU runs frame n; beginFrame waits only when
// the list it reuses is still in flight. Per-frame data is indexed by
// frameIndex(ring).

#include <RI/Abi.hpp>
#include <RI/RI.hpp>

#include <vector>

namespace RI::inline EQUINE_RI_ABI::Helpers {

struct FrameRing {
    Device *device = nullptr;
    std::vector<CommandList> lists;
    std::vector<TimelinePoint> points; // each list's last submission
    uint32 index = 0;
    bool recording = false;
};

// Keeps &device: the device must stay at that address, alive, until destroyFrameRing.
FrameRing createFrameRing(Device &device, uint32 framesInFlight = 2);
// Waits for every frame in flight, then destroys the lists. Call it before
// destroying what the frames used.
void destroyFrameRing(FrameRing &ring);

// Waits until this frame's list is free, resets it and begins recording.
CommandList &beginFrame(FrameRing &ring);
// Ends recording, submits, and moves to the next frame. Returns the frame's point.
TimelinePoint endFrame(FrameRing &ring);
// 0 .. framesInFlight - 1: which per-frame copy to use while recording.
inline uint32 frameIndex(const FrameRing &ring) { return ring.index; }

} // namespace RI::Helpers

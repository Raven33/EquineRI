#include <RI/Helpers/FrameRing.hpp>

namespace RI::Helpers {

FrameRing createFrameRing(Device &device, uint32 framesInFlight) {
    FrameRing ring;
    ring.device = &device;
    for (uint32 i = 0; i < framesInFlight; ++i) {
        ring.lists.push_back(createCommandList(device));
        ring.points.push_back({});
    }
    return ring;
}

void destroyFrameRing(FrameRing &ring) {
    for (size_t i = 0; i < ring.lists.size(); ++i) {
        wait(ring.device->graphicsQueue, ring.points[i]);
        destroyCommandList(*ring.device, ring.lists[i]);
    }
    ring = {};
}

CommandList &beginFrame(FrameRing &ring) {
    if (ring.recording) {
        reportError(ErrorCategory::eProgrammerError, "Helpers::beginFrame: the previous frame was not ended");
    }
    wait(ring.device->graphicsQueue, ring.points[ring.index]);
    CommandList &cmd = ring.lists[ring.index];
    resetCommandList(cmd);
    cmdBegin(cmd);
    ring.recording = true;
    return cmd;
}

TimelinePoint endFrame(FrameRing &ring) {
    if (!ring.recording) {
        reportError(ErrorCategory::eProgrammerError, "Helpers::endFrame: no frame is being recorded");
    }
    CommandList &cmd = ring.lists[ring.index];
    cmdEnd(cmd);
    const TimelinePoint point = submit(ring.device->graphicsQueue, cmd);
    ring.points[ring.index] = point;
    ring.index = (ring.index + 1) % static_cast<uint32>(ring.lists.size());
    ring.recording = false;
    return point;
}

} // namespace RI::Helpers

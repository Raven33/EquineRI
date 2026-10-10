#pragma once

// Portable texture checks (tests/gpu/TextureTests.cpp) callable on a device
// the caller made, so backend tests can run them under backend options (e.g.
// each Vulkan layout mode, P78). Failures are reported through EQ_CHECK.

#include <RI/RI.hpp>

namespace gputest {

void renderThenSample(RI::Device &device);      // L1 item 2
void depthWriteThenSample(RI::Device &device);  // L1 item 4
void storageWriteThenSample(RI::Device &device); // L1 item 3

} // namespace gputest

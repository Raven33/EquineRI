// RI: list the GPUs, pick one, create a device on it, destroy the device.
// The site shows the marked sections; this whole file is a complete program,
// compiled against every enabled backend (EQUINE_BUILD_DOC_SNIPPETS).

#include <RI/RI.hpp>

#include <algorithm>
#include <cstdio>
#include <vector>

int main() {
    // --8<-- [start:list-gpus]
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    for (const RI::Gpu &gpu : gpus) {
        std::printf("%s (%llu MiB)\n", gpu.name.c_str(), static_cast<unsigned long long>(gpu.memory >> 20));
    }
    // --8<-- [end:list-gpus]
    if (gpus.empty()) {
        return 1;
    }

    // --8<-- [start:pick-gpu]
    // Prefer a discrete GPU; otherwise take the first one listed.
    const auto discrete =
        std::ranges::find_if(gpus, [](const RI::Gpu &gpu) { return gpu.kind == RI::GpuKind::eDiscrete; });
    const RI::Gpu &chosen = discrete != gpus.end() ? *discrete : gpus.front();
    // --8<-- [end:pick-gpu]

    // --8<-- [start:create-device]
    RI::Device device = RI::createDevice({.gpu = chosen});
    std::printf("Device created on %s\n", device.gpu.name.c_str());
    // --8<-- [end:create-device]

    // --8<-- [start:destroy-device]
    RI::destroyDevice(device);
    // device.internal.state is now null: using `device` again is an error.
    // --8<-- [end:destroy-device]
    return 0;
}

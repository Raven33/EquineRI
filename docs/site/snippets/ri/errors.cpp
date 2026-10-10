// RI: the error channel. A helper written on top of RI reports its own
// failures through the same channel as RI, so one policy covers both.

#include <RI/RI.hpp>

#include <cstdio>
#include <vector>

// --8<-- [start:report-error]
// A user helper: picks the first GPU with at least `minimum` bytes of memory.
RI::Gpu pickGpuWithMemory(const std::vector<RI::Gpu> &gpus, uint64 minimum) {
    for (const RI::Gpu &gpu : gpus) {
        if (gpu.memory >= minimum) {
            return gpu;
        }
    }
    RI::reportError(RI::ErrorCategory::eUnsupported, "no GPU has enough memory for this program");
}
// --8<-- [end:report-error]

int main() {
    const RI::Gpu gpu = pickGpuWithMemory(RI::listGpus(), uint64{4} << 30);
    // --8<-- [start:category-name]
    const std::string_view name = RI::toString(RI::ErrorCategory::eOutOfMemory); // "out of memory"
    // --8<-- [end:category-name]
    std::printf("%s %.*s\n", gpu.name.c_str(), static_cast<int>(name.size()), name.data());
    return 0;
}

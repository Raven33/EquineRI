// Backend conformance (P83): compiled once per backend, never run. Every
// backend must offer the same portable functions with the same signatures and
// the same portable fields, so this file builds against each of them.

#include <RI/RI.hpp>

#include <type_traits>
#include <vector>

namespace {

// Devices (P60, P92)
static_assert(std::is_same_v<decltype(&RI::listGpus), std::vector<RI::Gpu> (*)()>);
static_assert(std::is_same_v<decltype(static_cast<RI::Device (*)(const RI::DeviceDesc &)>(&RI::createDevice)),
                             RI::Device (*)(const RI::DeviceDesc &)>);
static_assert(std::is_same_v<decltype(&RI::destroyDevice), void (*)(RI::Device &)>);
static_assert(std::is_same_v<decltype(RI::Device::gpu), RI::Gpu>);
static_assert(std::is_copy_constructible_v<RI::Device>); // thin value (P81)

[[maybe_unused]] void usage() {
    std::vector<RI::Gpu> gpus = RI::listGpus();
    RI::Device device = RI::createDevice({.gpu = gpus[0]});
    [[maybe_unused]] const RI::Gpu &gpu = device.gpu;
    RI::destroyDevice(device);
}

} // namespace

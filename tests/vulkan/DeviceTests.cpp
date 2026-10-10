#include <TestHarness.hpp>

#include <RI/RI.hpp>
#include <RI/Vk/Native.hpp>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdlib>

EQ_TEST(vk, list_gpus) {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    EQ_CHECK(!gpus.empty());
    for (const RI::Gpu &gpu : gpus) {
        EQ_CHECK(!gpu.name.empty());
        EQ_CHECK(gpu.vendorId != 0);
        EQ_CHECK(gpu.memory > 0);
        EQ_CHECK((gpu.id != std::array<uint8, 16>{}));
    }
}

EQ_TEST(vk, create_destroy_device) {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    RI::Device device = RI::createDevice({.gpu = gpus[0]});
    EQ_CHECK(device.gpu.id == gpus[0].id);
    EQ_CHECK(device.gpu.name == gpus[0].name);

    const RI::Vk::NativeDevice n = RI::Vk::native(device);
    EQ_CHECK(n.instance != VK_NULL_HANDLE);
    EQ_CHECK(n.device != VK_NULL_HANDLE);
    EQ_CHECK(n.graphicsQueue != VK_NULL_HANDLE);
    EQ_CHECK(n.apiVersion >= VK_API_VERSION_1_3);
    EQ_CHECK(n.functions != nullptr && n.functions->vkDeviceWaitIdle != nullptr);
    EQ_CHECK(n.functions->vkDeviceWaitIdle(n.device) == VK_SUCCESS);

    RI::destroyDevice(device);
    EQ_CHECK(device.internal.state == nullptr);
}

// Two devices at once, each with its own instance (P92).
EQ_TEST(vk, two_devices) {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    RI::Device a = RI::createDevice({.gpu = gpus[0]});
    RI::Device b = RI::createDevice({.gpu = gpus[0]});
    EQ_CHECK(RI::Vk::native(a).instance != RI::Vk::native(b).instance);
    RI::destroyDevice(a);
    EQ_CHECK(RI::Vk::native(b).functions->vkDeviceWaitIdle(RI::Vk::native(b).device) == VK_SUCCESS);
    RI::destroyDevice(b);
}

// Robustness, off by default, turned on by editing RI's features (P92).
EQ_TEST(vk, native_features_turn_on_robustness) {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    RI::Vk::DeviceFeatures features = RI::Vk::defaultDeviceFeatures(gpus[0]);
    EQ_CHECK(features.core.robustBufferAccess == VK_FALSE);
    EQ_CHECK(features.v12.bufferDeviceAddress == VK_TRUE);
    features.core.robustBufferAccess = VK_TRUE;
    features.v13.robustImageAccess = VK_TRUE;
    features.v14.pipelineRobustness = VK_TRUE;
    RI::Device device = RI::Vk::createDevice({.gpu = gpus[0]}, {.features = &features});
    EQ_CHECK(RI::Vk::native(device).device != VK_NULL_HANDLE);
    RI::destroyDevice(device);
}

// An extension RI does not use, with its feature structure appended after RI's.
EQ_TEST(vk, native_extension_and_pnext) {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    const char *const extensions[] = {VK_KHR_SHADER_CLOCK_EXTENSION_NAME};
    VkPhysicalDeviceShaderClockFeaturesKHR clock{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR,
                                                 .shaderSubgroupClock = VK_TRUE};
    RI::Device device = RI::Vk::createDevice({.gpu = gpus[0]}, {.extensions = extensions, .pNext = &clock});
    EQ_CHECK(RI::Vk::native(device).device != VK_NULL_HANDLE);
    EQ_CHECK(clock.pNext == nullptr); // left untouched
    RI::destroyDevice(device);
}

// Listing with a device alive must not break the device's instance functions.
EQ_TEST(vk, list_gpus_while_device_alive) {
    RI::Device device = RI::createDevice({.gpu = RI::listGpus()[0]});
    EQ_CHECK(!RI::listGpus().empty());
    RI::Vk::defaultDeviceFeatures(device.gpu);
    RI::destroyDevice(device);
    EQ_CHECK(device.internal.state == nullptr);
}

// A broken driver manifest makes the loader report an error (CMake points
// VK_ADD_DRIVER_FILES at one). It is not the program's fault: no abort.
// Linux only: the Windows loader fails the whole instance instead (see
// tests/CMakeLists.txt).
#ifndef _WIN32
EQ_TEST(vk, loader_errors_do_not_abort) {
    RI::Device device = RI::createDevice({.gpu = RI::listGpus()[0]});
    RI::destroyDevice(device);
}
#endif

// Aborts by design (see tests/CMakeLists.txt).
EQ_TEST(vk, unknown_gpu_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    RI::Gpu missing;
    missing.name = "Nonexistent GPU";
    RI::createDevice({.gpu = missing});
}

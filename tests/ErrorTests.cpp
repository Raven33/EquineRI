#include <TestHarness.hpp>

#include <RI/Error.hpp>

#include <csignal>
#include <cstdlib>

EQ_TEST(error, category_names) {
    EQ_CHECK(RI::toString(RI::ErrorCategory::eProgrammerError) == "programmer error");
    EQ_CHECK(RI::toString(RI::ErrorCategory::eUnsupported) == "unsupported");
    EQ_CHECK(RI::toString(RI::ErrorCategory::eDeviceLost) == "device lost");
    EQ_CHECK(RI::toString(RI::ErrorCategory::eOutOfMemory) == "out of memory");
    EQ_CHECK(RI::toString(RI::ErrorCategory::eSwapchainOutOfDate) == "swapchain out of date");
}

// Aborts by design. The SIGABRT handler turns the abort into a clean exit, and
// CMake checks the printed message (tests/CMakeLists.txt).
EQ_TEST(error, report_prints_and_aborts) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    RI::reportError(RI::ErrorCategory::eUnsupported, "mesh shaders are not available");
}

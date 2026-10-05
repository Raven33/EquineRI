#include <TestHarness.hpp>

#include <handles.hpp>

namespace {
struct TestTag {};
} // namespace

EQ_TEST(handle, default_is_invalid) {
    const RI::Handle<TestTag> handle{};
    EQ_CHECK(!handle);
    EQ_CHECK(static_cast<uint64>(handle) == RI::INVALID_UUID);
}

EQ_TEST(handle, nonzero_is_valid) {
    const RI::Handle<TestTag> handle{.id = 42};
    EQ_CHECK(static_cast<bool>(handle));
}

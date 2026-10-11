// The test runner shared by every test executable.
//
// Usage:
//   EquineRITests                  run every case
//   EquineRITests <group.name>     run one case (CTest registers each this way)
//   EquineRITests --list           print registered case names
//   EquineRITests --expect-count N fail unless exactly N cases are registered

#include <TestHarness.hpp>

#include <cstdio>
#include <cstdlib>
#include <string_view>

int main(int argc, char **argv) {
    const auto &cases = eqtest::registry();

    if (argc == 2 && std::string_view(argv[1]) == "--list") {
        for (const auto &tc : cases) {
            std::printf("%.*s\n", static_cast<int>(tc.name.size()), tc.name.data());
        }
        return 0;
    }

    if (argc == 3 && std::string_view(argv[1]) == "--expect-count") {
        const long expected = std::strtol(argv[2], nullptr, 10);
        if (static_cast<long>(cases.size()) != expected) {
            std::fprintf(stderr, "registered %zu test cases, CMake registers %ld\n", cases.size(), expected);
            return 1;
        }
        return 0;
    }

    const std::string_view filter = argc >= 2 ? std::string_view(argv[1]) : std::string_view{};
    int ran = 0;
    for (const auto &tc : cases) {
        if (!filter.empty() && tc.name != filter) {
            continue;
        }
        const int failuresBefore = eqtest::failureCount();
        tc.fn();
        ++ran;
        const bool passed = eqtest::failureCount() == failuresBefore;
        std::printf("[%s] %.*s\n", passed ? "PASS" : "FAIL", static_cast<int>(tc.name.size()), tc.name.data());
    }

    if (ran == 0) {
        std::fprintf(stderr, "no test case matched '%.*s'\n", static_cast<int>(filter.size()), filter.data());
        return 1;
    }
    return eqtest::failureCount() == 0 ? 0 : 1;
}

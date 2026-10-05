#pragma once

// Minimal self-registering test harness. No third-party dependency: the tests
// only need named cases, a failing-check report, and a CTest-friendly exit code.

#include <cstdio>
#include <string_view>
#include <vector>

namespace eqtest {

using TestFn = void (*)();

struct TestCase {
    std::string_view name;
    TestFn fn;
};

inline std::vector<TestCase> &registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline int &failureCount() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(std::string_view name, TestFn fn) { registry().push_back({name, fn}); }
};

inline void reportFailure(const char *expr, const char *file, int line) {
    std::fprintf(stderr, "  FAILED: %s\n    at %s:%d\n", expr, file, line);
    ++failureCount();
}

} // namespace eqtest

#define EQ_TEST_CONCAT_INNER(a, b) a##b
#define EQ_TEST_CONCAT(a, b) EQ_TEST_CONCAT_INNER(a, b)

// EQ_TEST(group, name) registers "group.name".
#define EQ_TEST(group, name)                                                                   \
    static void EQ_TEST_CONCAT(eqtest_fn_, EQ_TEST_CONCAT(group, EQ_TEST_CONCAT(_, name)))();  \
    static const eqtest::Registrar EQ_TEST_CONCAT(eqtest_reg_,                                 \
                                                  EQ_TEST_CONCAT(group, EQ_TEST_CONCAT(_, name))){ \
        #group "." #name, &EQ_TEST_CONCAT(eqtest_fn_, EQ_TEST_CONCAT(group, EQ_TEST_CONCAT(_, name)))}; \
    static void EQ_TEST_CONCAT(eqtest_fn_, EQ_TEST_CONCAT(group, EQ_TEST_CONCAT(_, name)))()

#define EQ_CHECK(expr)                                              \
    do {                                                            \
        if (!(expr)) {                                              \
            eqtest::reportFailure(#expr, __FILE__, __LINE__);       \
        }                                                           \
    } while (0)

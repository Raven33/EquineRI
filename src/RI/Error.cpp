#include <RI/Error.hpp>

#include <cstdio>
#include <cstdlib>

namespace RI {

std::string_view toString(ErrorCategory category) {
    switch (category) {
    case ErrorCategory::eProgrammerError: return "programmer error";
    case ErrorCategory::eUnsupported: return "unsupported";
    case ErrorCategory::eDeviceLost: return "device lost";
    case ErrorCategory::eOutOfMemory: return "out of memory";
    case ErrorCategory::eSwapchainOutOfDate: return "swapchain out of date";
    }
    return "unknown error";
}

// The policy (P57, X1 open): print and abort, for every category.
void reportError(ErrorCategory category, std::string_view message, std::source_location where) {
    const std::string_view name = toString(category);
    std::fprintf(stderr, "[EquineRI] %.*s: %.*s\n  at %s:%u (%s)\n", static_cast<int>(name.size()), name.data(),
                 static_cast<int>(message.size()), message.data(), where.file_name(),
                 static_cast<unsigned>(where.line()), where.function_name());
    std::fflush(stderr);
#if defined(_MSC_VER)
    // No "abort() has been called" dialog: it would block ctest.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::abort();
}

} // namespace RI

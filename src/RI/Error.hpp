#pragma once

// The central error channel (CONTEXT.md P57). Every failure site in RI reports
// here with a category. What happens next (the policy) lives in Error.cpp only,
// so changing the error model touches one file, not every call site. The policy
// is open (X1): for now every category prints a message and aborts.

#include <types.hpp>

#include <source_location>
#include <string_view>

namespace RI {

enum class ErrorCategory : uint8 {
    eProgrammerError,     // the caller broke an RI rule
    eUnsupported,         // the GPU, driver or backend lacks a feature (P11)
    eDeviceLost,          // the GPU device was lost
    eOutOfMemory,         // a GPU or CPU allocation failed
    eSwapchainOutOfDate,  // the swapchain must be recreated
};

std::string_view toString(ErrorCategory category);

// Reports a failure. Today's policy never returns (P57 keeps signature changes
// acceptable while X1 is open).
[[noreturn]] void reportError(ErrorCategory category, std::string_view message,
                              std::source_location where = std::source_location::current());

} // namespace RI

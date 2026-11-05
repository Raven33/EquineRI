#pragma once

// RI's own checks (CONTEXT.md P88, P91). A check is a plain free function that
// returns a CheckResult instead of reporting: empty when the rule holds,
// otherwise the message. Checks are always compiled, so GPU-free tests call them
// directly in every configuration. Backend functions run them through
// EQ_VALIDATE, which reports a failure as a programmer error and compiles to
// nothing (the argument is not evaluated) when EQUINE_VALIDATION is off.
//
// Scope (P88): only what native layers and GPU-AV cannot see, i.e. RI's own rules.

#include <RI/Abi.hpp>
#include <RI/Error.hpp>

#include <optional>
#include <string>

namespace RI::inline EQUINE_RI_ABI::Validation {

using CheckResult = std::optional<std::string>;

} // namespace RI::Validation

#if EQUINE_VALIDATION
#define EQ_VALIDATE(check)                                                                         \
    do {                                                                                           \
        if (const ::RI::Validation::CheckResult eqValidateResult = (check)) {                      \
            ::RI::reportError(::RI::ErrorCategory::eProgrammerError, *eqValidateResult);           \
        }                                                                                          \
    } while (0)
#else
#define EQ_VALIDATE(check) ((void)0)
#endif

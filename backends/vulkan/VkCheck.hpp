#pragma once

// Turns a failed VkResult into a report on the error channel (P57), with the
// category that matches the result.

#include <RI/Abi.hpp>
#include <RI/Error.hpp>

#include <volk.h>

#include <source_location>
#include <string_view>

namespace RI::inline EQUINE_RI_ABI::Vk {

ErrorCategory categoryOf(VkResult result);

// Reports `call` as failed unless `result` is VK_SUCCESS or another non-error code.
void checkVk(VkResult result, std::string_view call, std::source_location where = std::source_location::current());

} // namespace RI::Vk

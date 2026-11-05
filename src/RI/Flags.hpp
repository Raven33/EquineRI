#pragma once

// Flag enums (P86): combined with |, &, ^, ~ from RavenLib's bitmask. The
// operators live in rvl::; the using-declarations make them found for RI's
// enums by argument-dependent lookup.

#include <RI/Abi.hpp>
#include <rvl/bitmask.hpp>

namespace RI::inline EQUINE_RI_ABI {
using rvl::operator|;
using rvl::operator&;
using rvl::operator^;
using rvl::operator~;
using rvl::operator|=;
using rvl::operator&=;
using rvl::operator^=;
} // namespace RI

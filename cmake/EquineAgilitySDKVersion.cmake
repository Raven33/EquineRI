# Which D3D12 Agility SDK this tree uses (CONTEXT.md P55, P110).
#
# RI pins a retail package. A parent project picks its own by setting both
# variables before add_subdirectory (or as cache variables):
#
#   EQUINE_AGILITY_SDK_VERSION   - NuGet package version, e.g. 1.715.1-preview
#   EQUINE_AGILITY_SDK_SHA256    - SHA-256 of that .nupkg
#
# Provides EQUINE_AGILITY_SDK_PREVIEW (0 or 1): whether the package is a preview.
# It picks RI's ABI tag (src/RI/Abi.hpp) for the whole tree, Vulkan included,
# since both backends share the EquineRI library.

include_guard(GLOBAL)

set(EQUINE_AGILITY_SDK_DEFAULT_VERSION "1.619.6")
set(EQUINE_AGILITY_SDK_DEFAULT_SHA256 "08f0489281401aa430fc37322d6c3fc98a8025175aacd714c10d562f4963f1e9")
# The oldest retail minor version RI's D3D12 backend builds against. Previews
# are numbered apart (1.7xx), and their headers decide.
set(EQUINE_AGILITY_SDK_MIN_MINOR 619)

if (DEFINED EQUINE_AGILITY_SDK_VERSION AND NOT DEFINED EQUINE_AGILITY_SDK_SHA256)
    message(FATAL_ERROR "EQUINE_AGILITY_SDK_VERSION is set without EQUINE_AGILITY_SDK_SHA256: give the package's hash too.")
endif()
if (DEFINED EQUINE_AGILITY_SDK_SHA256 AND NOT DEFINED EQUINE_AGILITY_SDK_VERSION)
    message(FATAL_ERROR "EQUINE_AGILITY_SDK_SHA256 is set without EQUINE_AGILITY_SDK_VERSION.")
endif()
if (NOT DEFINED EQUINE_AGILITY_SDK_VERSION)
    set(EQUINE_AGILITY_SDK_VERSION "${EQUINE_AGILITY_SDK_DEFAULT_VERSION}")
    set(EQUINE_AGILITY_SDK_SHA256 "${EQUINE_AGILITY_SDK_DEFAULT_SHA256}")
endif()

if (EQUINE_AGILITY_SDK_VERSION MATCHES "-preview")
    set(EQUINE_AGILITY_SDK_PREVIEW 1)
else()
    set(EQUINE_AGILITY_SDK_PREVIEW 0)
endif()

# The package's minor version (1.619.x -> 619) is the version the executable
# exports: D3D12_SDK_VERSION for a retail package, D3D12_PREVIEW_SDK_VERSION
# for a preview (D3D12AgilityExports.hpp).
string(REGEX MATCH "^[0-9]+\\.([0-9]+)\\." EQUINE_AGILITY_SDK_VERSION_MATCH "${EQUINE_AGILITY_SDK_VERSION}")
if (NOT EQUINE_AGILITY_SDK_VERSION_MATCH)
    message(FATAL_ERROR "Cannot parse the Agility SDK version '${EQUINE_AGILITY_SDK_VERSION}'.")
endif()
set(EQUINE_AGILITY_SDK_VERSION_MINOR "${CMAKE_MATCH_1}")
if (NOT EQUINE_AGILITY_SDK_PREVIEW AND EQUINE_AGILITY_SDK_VERSION_MINOR LESS EQUINE_AGILITY_SDK_MIN_MINOR)
    message(FATAL_ERROR "Agility SDK ${EQUINE_AGILITY_SDK_VERSION} is older than RI needs (1.${EQUINE_AGILITY_SDK_MIN_MINOR}).")
endif()

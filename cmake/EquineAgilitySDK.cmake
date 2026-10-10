# D3D12 Agility SDK (CONTEXT.md, P55)
#
# Work graphs, enhanced barriers, and GPU_UPLOAD memory need the Agility SDK
# runtime rather than the OS-provided D3D12. A pinned NuGet package is
# downloaded at configure time, verified by hash, and extracted into the
# build tree. Nothing is committed to the repository.
#
# Provides:
#   EquineAgilitySDK                      - interface target: Agility headers
#   EQUINE_AGILITY_SDK_VERSION            - pinned package version
#   equine_deploy_agility_sdk(<target>)   - copy the runtime next to an executable
#
# Executables must also export D3D12SDKVersion / D3D12SDKPath; see
# backends/d3d12/D3D12AgilityExports.hpp.

include_guard(GLOBAL)

set(EQUINE_AGILITY_SDK_VERSION "1.619.6")
set(EQUINE_AGILITY_SDK_SHA256 "08f0489281401aa430fc37322d6c3fc98a8025175aacd714c10d562f4963f1e9")
set(EQUINE_AGILITY_SDK_URL
    "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/${EQUINE_AGILITY_SDK_VERSION}/microsoft.direct3d.d3d12.${EQUINE_AGILITY_SDK_VERSION}.nupkg"
)

set(EQUINE_AGILITY_SDK_ROOT "${CMAKE_BINARY_DIR}/_deps/agility_sdk-${EQUINE_AGILITY_SDK_VERSION}")
set(EQUINE_AGILITY_SDK_PACKAGE "${EQUINE_AGILITY_SDK_ROOT}.nupkg")

if (NOT EXISTS "${EQUINE_AGILITY_SDK_ROOT}/build/native/include/d3d12.h")
    message(STATUS "Equine Agility SDK: downloading ${EQUINE_AGILITY_SDK_VERSION}")
    file(DOWNLOAD
        "${EQUINE_AGILITY_SDK_URL}"
        "${EQUINE_AGILITY_SDK_PACKAGE}"
        EXPECTED_HASH SHA256=${EQUINE_AGILITY_SDK_SHA256}
        TLS_VERIFY ON
        STATUS EQUINE_AGILITY_SDK_DOWNLOAD_STATUS
    )
    list(GET EQUINE_AGILITY_SDK_DOWNLOAD_STATUS 0 EQUINE_AGILITY_SDK_DOWNLOAD_CODE)
    if (NOT EQUINE_AGILITY_SDK_DOWNLOAD_CODE EQUAL 0)
        message(FATAL_ERROR
            "Failed to download the D3D12 Agility SDK ${EQUINE_AGILITY_SDK_VERSION}: ${EQUINE_AGILITY_SDK_DOWNLOAD_STATUS}"
        )
    endif()

    # A .nupkg is a zip archive; ARCHIVE_EXTRACT detects the format from content.
    file(ARCHIVE_EXTRACT
        INPUT "${EQUINE_AGILITY_SDK_PACKAGE}"
        DESTINATION "${EQUINE_AGILITY_SDK_ROOT}"
        PATTERNS "build/native/include/*" "build/native/bin/x64/*"
    )
    file(REMOVE "${EQUINE_AGILITY_SDK_PACKAGE}")
endif()

set(EQUINE_AGILITY_SDK_INCLUDE_DIR "${EQUINE_AGILITY_SDK_ROOT}/build/native/include")
set(EQUINE_AGILITY_SDK_BIN_DIR "${EQUINE_AGILITY_SDK_ROOT}/build/native/bin/x64")

foreach(EQUINE_AGILITY_SDK_FILE IN ITEMS
    "${EQUINE_AGILITY_SDK_INCLUDE_DIR}/d3d12.h"
    "${EQUINE_AGILITY_SDK_BIN_DIR}/D3D12Core.dll"
    "${EQUINE_AGILITY_SDK_BIN_DIR}/d3d12SDKLayers.dll"
)
    if (NOT EXISTS "${EQUINE_AGILITY_SDK_FILE}")
        message(FATAL_ERROR "Agility SDK extraction is missing '${EQUINE_AGILITY_SDK_FILE}'. Delete '${EQUINE_AGILITY_SDK_ROOT}' and reconfigure.")
    endif()
endforeach()

message(STATUS "Equine Agility SDK: ${EQUINE_AGILITY_SDK_VERSION} at ${EQUINE_AGILITY_SDK_ROOT}")

# The Agility headers must win over the Windows SDK copies of d3d12.h.
add_library(EquineAgilitySDK INTERFACE)
target_include_directories(EquineAgilitySDK SYSTEM BEFORE INTERFACE
    "${EQUINE_AGILITY_SDK_INCLUDE_DIR}"
)

# D3D12_SDK_VERSION is the package's minor version (1.619.x -> 619). Exposed so
# D3D12AgilityExports.hpp can detect a shadowing Windows SDK d3d12.h.
string(REGEX MATCH "^[0-9]+\\.([0-9]+)\\." EQUINE_AGILITY_SDK_VERSION_MATCH "${EQUINE_AGILITY_SDK_VERSION}")
if (NOT EQUINE_AGILITY_SDK_VERSION_MATCH)
    message(FATAL_ERROR "Cannot parse the Agility SDK version '${EQUINE_AGILITY_SDK_VERSION}'.")
endif()
target_compile_definitions(EquineAgilitySDK INTERFACE
    "EQUINE_AGILITY_SDK_VERSION_MINOR=${CMAKE_MATCH_1}"
)
# The runtime folder travels with the target: this file's variables exist only
# in the directory that include()s it, while executables deploy from anywhere.
set_target_properties(EquineAgilitySDK PROPERTIES
    EQUINE_AGILITY_SDK_BIN_DIR "${EQUINE_AGILITY_SDK_BIN_DIR}"
)

# Copy the Agility runtime into <exe dir>/D3D12/, matching the D3D12SDKPath
# exported by D3D12AgilityExports.hpp.
function(equine_deploy_agility_sdk target)
    if (NOT TARGET "${target}")
        message(FATAL_ERROR "Cannot deploy the Agility SDK for missing target '${target}'.")
    endif()

    get_target_property(EQUINE_AGILITY_SDK_DEPLOY_DIR EquineAgilitySDK EQUINE_AGILITY_SDK_BIN_DIR)
    if (NOT EQUINE_AGILITY_SDK_DEPLOY_DIR)
        message(FATAL_ERROR "The EquineAgilitySDK target carries no runtime folder.")
    endif()

    add_custom_command(TARGET "${target}" POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${EQUINE_AGILITY_SDK_DEPLOY_DIR}/D3D12Core.dll"
            "${EQUINE_AGILITY_SDK_DEPLOY_DIR}/d3d12SDKLayers.dll"
            "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMENT "Copying the D3D12 Agility SDK runtime to ${target}'s output directory"
    )
endfunction()

# Metal Shader Converter for build-time Metal shaders (CONTEXT.md P113, P114)
#
# MSC is an Apple download (an Apple ID is needed) that RI cannot fetch or
# redistribute, so EQUINE_MSC_DIR names a local copy laid out like the pkg's
# payload: bin/metal-shaderconverter, include/, lib/. The version is pinned and
# checked at configure time, as DXC's archive hash is (P93).
#
# When MSC is found (Apple hosts only), equine_add_shaders also writes
# <build>/shaders/<name>.mtl.dxil: the DXIL container plus an `MTLB` part
# holding the metallib (P114 (2)), converted with one fixed root signature.
#
# Provides (global properties, so equine_add_shaders works from a parent):
#   EQUINE_METAL_SHADERS          - ON when .mtl.dxil files are written
#   EQUINE_MSC_EXECUTABLE         - metal-shaderconverter
#   EQUINE_MSC_ROOT_SIGNATURE     - the fixed root signature (JSON)
#   EQUINE_MSC_INCLUDE_DIR        - MSC's headers (metal_irconverter_runtime.h)
#   target equine_metal_pack      - the packing tool

include_guard(GLOBAL)

set(EQUINE_MSC_VERSION "4.0.3") # Metal Shader Converter 4.0 beta 3 (P113 (4))
set_property(GLOBAL PROPERTY EQUINE_METAL_SHADERS OFF)

if (NOT APPLE)
    return()
endif()

set(EQUINE_MSC_DIR "" CACHE PATH "Local Metal Shader Converter ${EQUINE_MSC_VERSION} (bin/, include/, lib/)")
set(EQUINE_MSC_EXECUTABLE "${EQUINE_MSC_DIR}/bin/metal-shaderconverter")
if (NOT EQUINE_MSC_DIR OR NOT EXISTS "${EQUINE_MSC_EXECUTABLE}")
    if (EQUINE_METAL_BACKEND)
        message(FATAL_ERROR
            "EQUINE_METAL_BACKEND needs Metal Shader Converter ${EQUINE_MSC_VERSION}: "
            "set EQUINE_MSC_DIR to a directory with bin/metal-shaderconverter.")
    endif()
    message(STATUS "Equine Metal shaders: off (EQUINE_MSC_DIR not set)")
    return()
endif()

execute_process(
    COMMAND "${EQUINE_MSC_EXECUTABLE}" --version
    OUTPUT_VARIABLE EQUINE_MSC_VERSION_OUTPUT
    ERROR_VARIABLE EQUINE_MSC_VERSION_OUTPUT
    RESULT_VARIABLE EQUINE_MSC_VERSION_RESULT
)
if (NOT EQUINE_MSC_VERSION_RESULT EQUAL 0 OR
    NOT EQUINE_MSC_VERSION_OUTPUT MATCHES "metal-irconverter version: ${EQUINE_MSC_VERSION}[\r\n]")
    message(FATAL_ERROR
        "${EQUINE_MSC_EXECUTABLE} is not Metal Shader Converter ${EQUINE_MSC_VERSION}:\n${EQUINE_MSC_VERSION_OUTPUT}")
endif()
message(STATUS "Equine Metal shaders: Metal Shader Converter ${EQUINE_MSC_VERSION} (${EQUINE_MSC_DIR})")

# One root signature for every shader (P114 (1)): 64 root constants at
# register(b0, space999), the most root data RI allows (kMaxRootDataSize), and
# both heap-indexing flags. The flags are a number: MSC 4.0 beta 3 crashes on
# the "A | B" string form.
set(EQUINE_MSC_ROOT_SIGNATURE "${CMAKE_BINARY_DIR}/equine/msc_root_signature.json")
file(WRITE "${EQUINE_MSC_ROOT_SIGNATURE}" [=[{
  "RootSignature": {
    "Flags": 3072,
    "NumParameters": 1,
    "NumStaticSamplers": 0,
    "Parameters": [
      {
        "Constants": { "Num32BitValues": 64, "RegisterSpace": 999, "ShaderRegister": 0 },
        "ParameterType": "IRRootParameterType32BitConstants",
        "ShaderVisibility": "IRShaderVisibilityAll"
      }
    ],
    "StaticSamplers": []
  },
  "version": "IRRootSignatureVersion_1_1"
}
]=])

if (NOT TARGET equine_metal_pack)
    add_executable(equine_metal_pack "${CMAKE_CURRENT_LIST_DIR}/../tools/metal_pack/MetalPack.cpp")
endif()

set_property(GLOBAL PROPERTY EQUINE_METAL_SHADERS ON)
set_property(GLOBAL PROPERTY EQUINE_MSC_EXECUTABLE "${EQUINE_MSC_EXECUTABLE}")
set_property(GLOBAL PROPERTY EQUINE_MSC_ROOT_SIGNATURE "${EQUINE_MSC_ROOT_SIGNATURE}")
set_property(GLOBAL PROPERTY EQUINE_MSC_INCLUDE_DIR "${EQUINE_MSC_DIR}/include")

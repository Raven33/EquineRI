#pragma once

// Shader-compilation helper (P29, P41): HLSL to the bytecode of the backend RI
// is built with, at run time, through the DXC library, with RI's compile rule
// (P93, P94): SPIR-V for Vulkan (DX buffer layout, samplers in set 1), DXIL
// for D3D12. RI itself takes bytecode only.

#include <RI/RI.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace RI::Helpers {

struct ShaderCompiler {
    struct {
        void *library = nullptr;  // the loaded dxcompiler library
        void *compiler = nullptr; // IDxcCompiler3
        void *utils = nullptr;    // IDxcUtils
        void *includes = nullptr; // IDxcIncludeHandler
    } internal;
};

// `dxcLibrary`: the path of dxcompiler (libdxcompiler.so / dxcompiler.dll);
// nullptr loads the pinned DXC of the build.
// A compiler is used by one thread at a time; several may exist at once, one
// per thread, to build pipelines in parallel (P106).
ShaderCompiler createShaderCompiler(const char *dxcLibrary = nullptr);
void destroyShaderCompiler(ShaderCompiler &compiler);

struct ShaderSource {
    std::string_view code;
    std::string_view profile;      // e.g. cs_6_6, vs_6_6, ps_6_6, lib_6_8
    std::string_view entry = "main";
    std::string_view name = "shader.hlsl"; // for messages
    std::vector<std::string> includeDirectories; // RI's <equine/root.hlsli> is always found
    std::vector<std::string> defines;            // NAME or NAME=VALUE
};

struct CompiledShader {
    std::vector<std::byte> bytecode; // empty when compilation failed
    std::string messages;            // errors and warnings
};

CompiledShader compileShader(ShaderCompiler &compiler, const ShaderSource &source);

} // namespace RI::Helpers

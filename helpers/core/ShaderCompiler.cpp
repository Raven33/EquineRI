#include <RI/Helpers/ShaderCompiler.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <dxcapi.h>

#include <format>

namespace RI::Helpers {

namespace {

std::wstring wide(std::string_view text) { return {text.begin(), text.end()}; } // arguments are ASCII

void *loadLibrary(const char *path) {
#if defined(_WIN32)
    return LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void *symbol(void *library, const char *name) {
#if defined(_WIN32)
    return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

template <typename T>
void releaseCom(void *&object) {
    if (object != nullptr) {
        static_cast<T *>(object)->Release();
        object = nullptr;
    }
}

} // namespace

ShaderCompiler createShaderCompiler(const char *dxcLibrary) {
    const char *path = dxcLibrary != nullptr ? dxcLibrary : EQUINE_DXC_LIBRARY;
    ShaderCompiler compiler;
    compiler.internal.library = loadLibrary(path);
    if (compiler.internal.library == nullptr) {
        reportError(ErrorCategory::eUnsupported, std::format("Helpers::createShaderCompiler: cannot load {}", path));
    }
    const auto create = reinterpret_cast<DxcCreateInstanceProc>(symbol(compiler.internal.library, "DxcCreateInstance"));
    IDxcCompiler3 *dxc = nullptr;
    IDxcUtils *utils = nullptr;
    IDxcIncludeHandler *includes = nullptr;
    if (create == nullptr || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc))) ||
        FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) || FAILED(utils->CreateDefaultIncludeHandler(&includes))) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("Helpers::createShaderCompiler: {} is not a usable DXC library", path));
    }
    compiler.internal.compiler = dxc;
    compiler.internal.utils = utils;
    compiler.internal.includes = includes;
    return compiler;
}

void destroyShaderCompiler(ShaderCompiler &compiler) {
    releaseCom<IDxcIncludeHandler>(compiler.internal.includes);
    releaseCom<IDxcUtils>(compiler.internal.utils);
    releaseCom<IDxcCompiler3>(compiler.internal.compiler);
    // The library stays loaded: DXC keeps process-wide state.
    compiler = {};
}

CompiledShader compileShader(ShaderCompiler &compiler, const ShaderSource &source) {
    std::vector<std::wstring> args = {wide(source.name), L"-T", wide(source.profile), L"-HV", L"2021",
                                      L"-I", wide(EQUINE_SHADER_INCLUDE_DIR)};
    if (!source.profile.starts_with("lib_")) {
        args.insert(args.end(), {L"-E", wide(source.entry)});
    }
    for (const std::string &dir : source.includeDirectories) {
        args.insert(args.end(), {L"-I", wide(dir)});
    }
    for (const std::string &define : source.defines) {
        args.insert(args.end(), {L"-D", wide(define)});
    }
#if EQUINE_BACKEND_VULKAN
    // RI's rule (P93, P94): DX layout, samplers in their own set.
    args.insert(args.end(), {L"-spirv", L"-fspv-target-env=vulkan1.3", L"-fvk-use-dx-layout", L"-fvk-bind-sampler-heap",
                             L"0", L"1"});
#endif
    std::vector<LPCWSTR> pointers;
    for (const std::wstring &arg : args) {
        pointers.push_back(arg.c_str());
    }
    const DxcBuffer buffer{.Ptr = source.code.data(), .Size = source.code.size(), .Encoding = DXC_CP_UTF8};
    IDxcResult *result = nullptr;
    auto *dxc = static_cast<IDxcCompiler3 *>(compiler.internal.compiler);
    CompiledShader out;
    if (FAILED(dxc->Compile(&buffer, pointers.data(), static_cast<UINT32>(pointers.size()),
                            static_cast<IDxcIncludeHandler *>(compiler.internal.includes), IID_PPV_ARGS(&result)))) {
        out.messages = "DXC could not run";
        return out;
    }
    IDxcBlobUtf8 *errors = nullptr;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors != nullptr) {
        out.messages.assign(errors->GetStringPointer(), errors->GetStringLength());
        errors->Release();
    }
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    IDxcBlob *object = nullptr;
    if (SUCCEEDED(status) && SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) &&
        object != nullptr) {
        const auto *bytes = static_cast<const std::byte *>(object->GetBufferPointer());
        out.bytecode.assign(bytes, bytes + object->GetBufferSize());
        object->Release();
    }
    result->Release();
    return out;
}

} // namespace RI::Helpers

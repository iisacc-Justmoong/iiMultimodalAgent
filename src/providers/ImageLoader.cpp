#if defined(_WIN32)
# include <windows.h>
#else
# include <dlfcn.h>
#endif
#include <iiMultimodalAgent/detail/ImageLoader.h>
#include <iiMultimodalAgent/Inference.h>
#include <filesystem>

namespace iiMultimodalAgent::detail {
namespace {
std::filesystem::path providerPath() {
#if defined(_WIN32)
    HMODULE module{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&imageEntry),&module))
        throw Error(ErrorCode::RuntimeUnavailable,"Cannot locate the agent DLL");
    std::wstring path(32768,L'\0');const auto size=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
    if(!size||size>=path.size())throw Error(ErrorCode::RuntimeUnavailable,"Cannot locate the agent DLL path");
    path.resize(size);
    const auto library=std::filesystem::canonical(path);
#else
    Dl_info info{};
    if(!dladdr(reinterpret_cast<const void*>(&imageEntry),&info)||!info.dli_fname)
        throw Error(ErrorCode::RuntimeUnavailable,"Cannot locate the agent shared library");
    const auto library=std::filesystem::canonical(info.dli_fname);
#endif
    return library.parent_path()/IIMULTIMODALAGENT_IMAGE_PLUGIN_NAME;
}
}

ImageEntry imageEntry() {
    // Keep the native adapter resident: its backend caches and worker lifetime
    // can outlive one tool call. No model- or wire-controlled library search.
    static const auto entry=[] {
        const auto path=providerPath();
#if defined(_WIN32)
        const auto module=LoadLibraryExW(path.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
        if(!module)throw Error(ErrorCode::RuntimeUnavailable,"Cannot load the packaged native image adapter");
        const auto symbol=GetProcAddress(module,"iimagent_native_image_v1");
#else
        const auto module=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);
        if(!module)throw Error(ErrorCode::RuntimeUnavailable,QStringLiteral("Cannot load the packaged native image adapter: ")+QString::fromUtf8(dlerror()));
        const auto symbol=dlsym(module,"iimagent_native_image_v1");
#endif
        if(!symbol)throw Error(ErrorCode::RuntimeUnavailable,"Native image adapter ABI is unavailable");
        return reinterpret_cast<ImageEntry>(symbol);
    }();
    return entry;
}
}

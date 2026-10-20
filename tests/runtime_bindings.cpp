#if defined(_WIN32)
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#else
# include <dlfcn.h>
#endif
#include <iiMultimodalAgent.h>
#include <iiFileProviderNative.h>
#include <iiAudioGen/Diffusers.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

std::filesystem::path loaded(const void* address) {
#if defined(_WIN32)
    HMODULE module{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address),&module))throw std::runtime_error("Cannot locate loaded module");
    std::wstring path(32768,L'\0');const auto size=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
    if(!size||size>=path.size())throw std::runtime_error("Cannot resolve loaded module path");
    path.resize(size);return std::filesystem::canonical(path);
#else
    Dl_info info{};
    if(!dladdr(address,&info)||!info.dli_fname)throw std::runtime_error("Cannot locate loaded module");
    return std::filesystem::canonical(info.dli_fname);
#endif
}
void check(const char* label,const char* expected,const void* address) {
    const auto actual=loaded(address);
    std::cout<<label<<": "<<actual<<std::endl;
    if(!std::filesystem::equivalent(actual,std::filesystem::path(expected)))
        throw std::runtime_error(std::string(label)+" loaded a different SDK package: "+actual.string());
}
int main(int argc,char** argv) {
    if(argc!=7)return 2;
    try {
        check("iiFileProviderNative",argv[1],reinterpret_cast<const void*>(&iiFileProvider::native::utcNow));
        check("iiLocalLLM",argv[2],reinterpret_cast<const void*>(&iiLocalLLM::helloWorld));
        check("iiLocalDiffusion",argv[3],reinterpret_cast<const void*>(&iiLocalDiffusion::nativeDiffusionAvailable));
#if !defined(IIAUDIOGEN_STATIC_DEFINE)
        check("iiAudioGen",argv[4],reinterpret_cast<const void*>(&iiAudioGen::createDiffusersBackend));
#endif
        check("iiVoiceOver",argv[5],reinterpret_cast<const void*>(&iiVoiceOver::helloWorld));
        check("iiMultimodalAgent",argv[6],reinterpret_cast<const void*>(&iiMultimodalAgent::agent::multimodalCapabilities));
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}

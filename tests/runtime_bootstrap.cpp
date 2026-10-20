#if defined(_WIN32)
# define WIN32_LEAN_AND_MEAN
# define NOMINMAX
# include <windows.h>
#else
# include <dlfcn.h>
#endif
#include <iiMultimodalAgent.h>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if(argc != 2) return 2;
    const std::filesystem::path provider(argv[1]);
#if defined(_WIN32)
    const auto loaded = GetModuleHandleW(provider.filename().c_str()) != nullptr;
#else
    const auto module = dlopen(provider.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    const bool loaded = module != nullptr;
    if(module) dlclose(module);
#endif
    if(loaded) { std::cerr << "Image provider was loaded before image work\n"; return 1; }
    const auto capabilities = iiMultimodalAgent::agent::multimodalCapabilities({});
    if(capabilities["image_generation"].toBool()) return 3;
    std::cout << "Agent bootstrap does not load the image provider\n";
    return 0;
}

#include <iiMultimodalAgent/detail/ImagePlugin.h>
#include <exception>

#if defined(_WIN32)
# define IMAGE_ENTRY __declspec(dllexport)
#else
# define IMAGE_ENTRY __attribute__((visibility("default")))
#endif

extern "C" IMAGE_ENTRY void iimagent_native_image_v1(
    const iiLocalDiffusion::NativeGenerationRequest* request,
    const std::atomic_bool* cancelled,
    const iiLocalDiffusion::NativeProgressCallback* progress,
    iiLocalDiffusion::NativeGenerationResult* result) {
    if(!request || !cancelled || !progress || !result) return;
    try {
        *result = iiLocalDiffusion::generateNativeImageWithProgress(*request, *cancelled, *progress);
    } catch(const std::exception& error) {
        *result = {};
        result->error = error.what();
    } catch(...) {
        *result = {};
        result->error = "Native image backend failed";
    }
}

extern "C" IMAGE_ENTRY void iimagent_native_image_v2(
    const iiLocalDiffusion::NativeGenerationRequest* request,
    const std::atomic_bool* cancelled, const iiLocalDiffusion::NativeProgressCallback* progress,
    int maximumImageDimension, iiLocalDiffusion::NativeImageExecution* result) {
    if (!request || !cancelled || !progress || !result) return;
    try {
        const auto resolved = iiLocalDiffusion::resolveNativeImageDefaults(*request);
        if (resolved.width > maximumImageDimension || resolved.height > maximumImageDimension) {
            result->image.error = "Model default image dimensions exceed the host budget";
            return;
        }
        *result = iiLocalDiffusion::generateNativeImageWithReport(resolved, *cancelled, *progress);
    } catch (const std::exception& error) {
        *result = {}; result->image.error = error.what();
    } catch (...) {
        *result = {}; result->image.error = "Native image backend failed";
    }
}

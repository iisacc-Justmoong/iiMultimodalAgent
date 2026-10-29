#pragma once
#include <Generation/NativeDiffusion.hpp>

namespace iiMultimodalAgent::detail {
// Private native ABI, built with the same compiler/runtime as the agent. Never
// exposed on the wire; model values remain host-owned NativeGenerationRequest.
using ImageEntry = void (*)(const iiLocalDiffusion::NativeGenerationRequest*,
    const std::atomic_bool*, const iiLocalDiffusion::NativeProgressCallback*,
    iiLocalDiffusion::NativeGenerationResult*);
}

#pragma once
#include "Tools.h"
#include <Generation/NativeDiffusion.hpp>
#include <iiAudioGen/iiAudioGen.h>
#include <iiVoiceOver.h>
#include <filesystem>
#include <memory>

namespace iiMultimodalAgent::agent {

// Host-owned configuration. Model paths, runtimes and destinations never come
// from a model's tool arguments. Empty services are explicitly unavailable.
struct MultimodalServices {
    std::filesystem::path outputDirectory;
    std::optional<iiLocalDiffusion::NativeGenerationRequest> image;
    // Empty selects iiLocalDiffusion's in-process native implementation.
    // A host may inject its own native model adapter using the same value types.
    using ImageExecutor = std::function<iiLocalDiffusion::NativeGenerationResult(
        const iiLocalDiffusion::NativeGenerationRequest&, const CancellationToken&,
        const iiLocalDiffusion::NativeProgressCallback&)>;
    ImageExecutor imageExecutor;
    // A loaded host-owned generator; the agent never creates a Python backend,
    // downloads a model, or changes the generator's selected backend.
    std::shared_ptr<iiAudioGen::Generator> audio;
    std::shared_ptr<iiVoiceOver::SpeechSynthesizer> speech;
    iiVoiceOver::SpeechOptions speechOptions;
    int maximumImageDimension = 2048;
    double maximumAudioSeconds = 30;
    std::size_t maximumArtifactBytes = 32 * 1024 * 1024;
    std::size_t maximumInlineBytes = 512 * 1024; // Larger results remain provider files with metadata.
    bool inlineMedia = true;
};

// Exposes only configured services. Registered tools pass through the ordinary
// permission, plan-mode, cancellation, host decision and transcript contracts.
// The caller installs this batch once (or uses ToolRegistry::replace).
[[nodiscard]] IIMULTIMODALAGENT_EXPORT QList<Tool>
multimodalTools(MultimodalServices services);
[[nodiscard]] IIMULTIMODALAGENT_EXPORT QJsonObject
multimodalCapabilities(const MultimodalServices& services);

} // namespace iiMultimodalAgent::agent

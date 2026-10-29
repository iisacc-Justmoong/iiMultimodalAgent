#pragma once
#include <iiMultimodalAgent/agent/Tools.h>
#include <Generation/NativeDiffusion.hpp>
#include <iiAudioGen/iiAudioGen.h>
#include <iiVoiceOver.h>
#include <filesystem>
#include <map>
#include <memory>
#include <string_view>

namespace iiMultimodalAgent::agent {

inline bool validImageModelId(std::string_view id) {
    if (id.empty() || id.size() > 64 || id == "default") return false;
    for (const unsigned char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    return true;
}

// Host-owned configuration. Tool arguments select only registered model IDs;
// model paths, runtimes and destinations remain under host control.
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
    // Optional additional image models, keyed by a host-defined ID. "default"
    // is reserved for image->modelPath; IDs never act as paths.
    std::map<std::string, std::filesystem::path> imageModels;
};

// Exposes only configured services. Registered tools pass through the ordinary
// permission, plan-mode, cancellation, host decision and transcript contracts.
// The caller installs this batch once (or uses ToolRegistry::replace).
[[nodiscard]] IIMULTIMODALAGENT_EXPORT QList<Tool>
multimodalTools(MultimodalServices services);
[[nodiscard]] IIMULTIMODALAGENT_EXPORT QJsonObject
multimodalCapabilities(const MultimodalServices& services);

} // namespace iiMultimodalAgent::agent

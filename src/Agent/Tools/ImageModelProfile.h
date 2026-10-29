#pragma once

#include <QtCore/QString>
#include <filesystem>
#include <optional>

namespace iiMultimodalAgent::agent::image_detail {

// Only values actually declared by the package or a recognized family rule
// are present. The caller supplies host fallbacks and request overrides.
struct ImageModelProfile {
    QString family;
    std::optional<int> steps, width, height;
    bool stepsFromPackage = false;
};

ImageModelProfile inspectImageModel(const std::filesystem::path& path, int maximumDimension);
}

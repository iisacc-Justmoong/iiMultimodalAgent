#pragma once
#include <iiMultimodalAgent/Inference.h>

namespace iiMultimodalAgent::agent {
// Pure nbformat-4 transformation. File access, permissions, observed hashes and
// atomic persistence belong to the workspace tool that calls this function.
struct NotebookEditResult {
    QByteArray content;
    QJsonObject metadata;
};
IIMULTIMODALAGENT_EXPORT void validateNotebookEditArguments(const QJsonObject&);
IIMULTIMODALAGENT_EXPORT NotebookEditResult editNotebook(const QByteArray&,const QJsonObject&,
    const CancellationToken& = {});
}

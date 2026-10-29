#include <iiMultimodalAgent/agent/Rag.h>
#include <iiMultimodalAgent/agent/Engine.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <iostream>

using namespace iiLocalLLM;
namespace a = iiMultimodalAgent::agent;

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    try {
        QTemporaryDir root(QDir::current().filePath("rag-native-XXXXXX"));
        if (!root.isValid()) throw std::runtime_error("Cannot create fixture directory");
        const QFileInfo weights(QString::fromLocal8Bit(argv[1]));
        const auto modelRoot = weights.absolutePath();
        QFile file(QDir(modelRoot).filePath("manifest.json"));
        if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
            throw std::runtime_error("Cannot read the existing model manifest");
        const auto manifest = parseModelManifest(QJsonDocument::fromJson(file.readAll()).object());
        if (manifest.id != QFileInfo(modelRoot).fileName())
            throw std::runtime_error("Fixture manifest does not match its catalog directory");
        ServiceOptions options; options.modelsDirectory = QFileInfo(modelRoot).absolutePath();
        Service service(options);
        (void)service.loadModel({modelUri(manifest.id), 4096}).get();
        auto index = std::make_shared<a::KnowledgeIndex>(root.filePath("knowledge.json"));
        index->upsert({"canvas", "Canvas project", "society://canvas/42", "rev-7", "document",
            {{"canvas:42", "The canvas project identifier is KITE-731."}}});
        auto model = std::make_shared<a::ServiceModel>(service);
        a::Rag rag(index, model);
        const auto result = rag.answer("What is the canvas project identifier?", modelUri(manifest.id));
        std::cout << "response=" << result.text.toStdString() << '\n';
        if (result.status != a::RagStatus::Answered || !result.text.contains("KITE-731")
            || result.evidence.size() != 1 || result.usage.generatedTokens <= 0)
            throw std::runtime_error("Model did not return the retrieved identifier with a valid citation");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

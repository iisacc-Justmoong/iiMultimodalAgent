#include <iiMultimodalAgent.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <iostream>

namespace agent = iiMultimodalAgent::agent;
class ConsumerModel final : public agent::Model {
public:
    agent::ModelReply generate(const agent::ModelRequest&, const iiMultimodalAgent::CancellationToken&,
        const iiMultimodalAgent::TextCallback&) override { return {"Original artwork [S1]", {}}; }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir root(QDir::current().filePath("rag-consumer-XXXXXX"));
    if (!root.isValid()) return 1;
    auto index = std::make_shared<agent::KnowledgeIndex>(root.filePath("index.json"));
    index->upsert({"asset-1", "Canvas", "society://asset/1", "revision-1", "document",
        {{"canvas:1", "The original artwork is preserved."}}});
    const auto answer = agent::Rag(index, std::make_shared<ConsumerModel>()).answer("original artwork", "fixture");
    if (answer.status != agent::RagStatus::Answered || answer.evidence.size() != 1
        || answer.evidence.front().sourceUri != "society://asset/1") return 1;
    std::cout << "installed consumer retrieved and cited the source\n";
}

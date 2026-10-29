#include <iiMultimodalAgent/agent/Rag.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTextStream>

namespace a = iiMultimodalAgent::agent;
namespace {
void output(const QJsonObject& object) {
    QTextStream(stdout) << QJsonDocument(object).toJson(QJsonDocument::Compact) << '\n';
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    try {
        if (args.size() == 2 && args[1] == "--version") {
            QTextStream(stdout) << "iimagent-rag " << IILOCALLLM_APP_VERSION << '\n';
            return 0;
        }
        if (args.size() < 4 || args.size() > 5) {
            QTextStream(stderr) << "Usage: iimagent-rag put STORE SOURCE_JSON | remove STORE SOURCE_ID | search STORE QUERY [LIMIT]\n";
            return 2;
        }
        const auto operation = args[1];
        a::KnowledgeIndex index(args[2]);
        if (operation == "put" && args.size() == 4) {
            QFile file(args[3]);
            if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot open source JSON");
            const auto bytes = file.read(2 * 1024 * 1024 + 1);
            if (bytes.size() > 2 * 1024 * 1024 || file.error() != QFile::NoError)
                throw std::runtime_error("Source JSON is too large or cannot be read");
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(bytes, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject())
                throw std::runtime_error("Invalid source JSON");
            const auto source = a::knowledgeSourceFromJson(document.object());
            index.upsert(source);
            output({{"source_id", source.id}, {"revision", source.revision}, {"updated", true}});
        } else if (operation == "remove" && args.size() == 4) {
            output({{"source_id", args[3]}, {"removed", index.remove(args[3])}});
        } else if (operation == "search") {
            bool ok = true;
            const int limit = args.size() == 5 ? args[4].toInt(&ok) : 6;
            if (!ok) throw std::runtime_error("Limit must be an integer");
            QJsonArray hits;
            for (const auto& hit : index.search(args[3], limit)) hits.append(a::toJson(hit));
            output({{"hits", hits}, {"ranking", "lexical"}, {"evidence_is_untrusted", true}});
        } else {
            throw std::runtime_error("Invalid operation or arguments");
        }
        return 0;
    } catch (const std::exception& error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
}

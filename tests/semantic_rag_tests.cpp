#include <iiMultimodalAgent/agent/SemanticRag.h>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <limits>

using namespace iiLocalLLM;
namespace a = iiMultimodalAgent::agent;

namespace {
class Embeddings final : public a::EmbeddingProvider {
public:
    QString version = "fixture-model-v1";
    mutable int documents = 0, queries = 0;
    bool invalid = false;
    bool wrongQueryDimension = false;
    mutable std::function<void()> onDocument;
    QString fingerprint(const CancellationToken& token) const override { token.throwIfCancelled(); return version; }
    QList<float> embed(const QString& text, a::EmbeddingPurpose purpose, const CancellationToken& token) const override {
        token.throwIfCancelled();
        if (purpose == a::EmbeddingPurpose::Document) {
            ++documents;
            if (onDocument) { auto callback = std::move(onDocument); callback(); }
        } else ++queries;
        if (invalid) return {std::numeric_limits<float>::quiet_NaN(), 1.0f};
        if (purpose == a::EmbeddingPurpose::Query && wrongQueryDimension) return {1.0f, 0.0f, 0.0f};
        if (text.contains("painting", Qt::CaseInsensitive) || text.contains("recover", Qt::CaseInsensitive)) return {1.0f, 0.0f};
        return {0.0f, 1.0f};
    }
};
class LocalModel final : public RuntimeModel, public EmbeddingRuntimeModel {
public:
    TokenList tokenize(const QList<ChatMessage>&, const CancellationToken&) override { return {1}; }
    std::unique_ptr<RuntimeContext> createContext(const CancellationToken&) override { return {}; }
    QList<float> embedText(const QString& text, const CancellationToken& token) override {
        token.throwIfCancelled(); return text.startsWith("query: ") ? QList<float>{1, 0} : QList<float>{0, 1};
    }
};
class LocalRuntime final : public Runtime {
public:
    QString id() const override { return "embedding-fixture"; }
    bool supportsModel(const ModelSpec& spec) const override { return spec.format == "test"; }
    QList<RuntimeDevice> devices(const HardwareInfo&) const override { return {{ComputeBackend::Cpu, {}}}; }
    MemoryEstimate estimateMemory(const ModelSpec&, int) const override { return {1024, 0, 0, "fixture"}; }
    std::shared_ptr<RuntimeModel> load(const ModelSpec&, const RuntimeDevice&, const CancellationToken&) override {
        return std::make_shared<LocalModel>();
    }
};
class AnswerModel final : public a::Model {
public:
    a::ModelRequest observed;
    a::ModelReply generate(const a::ModelRequest& request, const CancellationToken&,
                           const TextCallback&) override {
        observed = request; return {"The artwork was archived [S1].", {}};
    }
};
a::KnowledgeSource source(QString id, QString text, QString revision = "v1") {
    return {id, "Creative project", "society://asset/" + id, revision, "document", {{"canvas:" + id, text}}};
}
struct Fixture {
    QTemporaryDir root;
    std::shared_ptr<a::KnowledgeIndex> knowledge = std::make_shared<a::KnowledgeIndex>(root.filePath("sources.json"));
    std::shared_ptr<Embeddings> embeddings = std::make_shared<Embeddings>();
    QString vectors = root.filePath("vectors.json");
    std::shared_ptr<a::SemanticKnowledgeIndex> semantic = std::make_shared<a::SemanticKnowledgeIndex>(
        knowledge, embeddings, vectors, a::SemanticOptions{.minimumSimilarity = 0.1});
};
}

class SemanticRagTests final : public QObject {
    Q_OBJECT
private slots:
    void semanticMatchesBeyondLexicalAndRetainsProvenance() {
        Fixture f;
        f.knowledge->upsert(source("art", "The painting stays safe in its original archive."));
        f.knowledge->upsert(source("meal", "A recipe contains tomatoes."));
        QVERIFY(f.knowledge->search("recover work", 5).isEmpty());
        QVERIFY_THROWS_EXCEPTION(Error, (void)f.semantic->search("recover work", 5));
        f.semantic->refresh();
        const auto hits = f.semantic->search("recover work", 5);
        QCOMPARE(hits.size(), 1); QCOMPARE(hits.front().sourceId, QString("art"));
        QCOMPARE(hits.front().sourceUri, QString("society://asset/art"));
        QCOMPARE(hits.front().revision, QString("v1"));
        QCOMPARE(hits.front().locator, QString("canvas:art"));
        QCOMPARE(hits.front().sha256.size(), 64);
        QVERIFY(hits.front().rankingScore > 0.99);
        QCOMPARE(f.embeddings->documents, 2); QCOMPARE(f.embeddings->queries, 1);
        auto model = std::make_shared<AnswerModel>();
        const auto answer = a::Rag(f.semantic, model).answer("recover work", "fixture");
        QCOMPARE(answer.status, a::RagStatus::Answered);
        QCOMPARE(answer.evidence.front().sourceId, QString("art"));
        QVERIFY(model->observed.messages.back().text.contains("The painting stays safe"));
    }
    void changesAndModelVersionsRequireRefreshWithoutStaleHits() {
        Fixture f;
        f.knowledge->upsert(source("art", "A painting is archived."));
        f.knowledge->upsert(source("meal", "A recipe is saved."));
        f.semantic->refresh(); QCOMPARE(f.embeddings->documents, 2);
        f.semantic->refresh(); QCOMPARE(f.embeddings->documents, 2);
        f.knowledge->upsert(source("art", "The painting changed.", "v2"));
        QVERIFY_THROWS_EXCEPTION(Error, (void)f.semantic->search("recover", 5));
        f.semantic->refresh(); QCOMPARE(f.embeddings->documents, 3);
        QCOMPARE(f.semantic->search("recover", 5).front().revision, QString("v2"));
        QVERIFY(f.knowledge->remove("art"));
        QVERIFY_THROWS_EXCEPTION(Error, (void)f.semantic->search("recover", 5));
        f.semantic->refresh(); QVERIFY(f.semantic->search("recover", 5).isEmpty());
        f.embeddings->version = "fixture-model-v2";
        QVERIFY_THROWS_EXCEPTION(Error, (void)f.semantic->search("recover", 5));
        f.semantic->refresh(); QCOMPARE(f.embeddings->documents, 4);
    }
    void invalidEmbeddingsAndDamagedCachesFailClosed() {
        Fixture f; f.knowledge->upsert(source("art", "A painting.")); f.semantic->refresh();
        QFile before(f.vectors); QVERIFY(before.open(QIODevice::ReadOnly)); const auto original = before.readAll(); before.close();
        f.knowledge->upsert(source("art", "New painting.")); f.embeddings->invalid = true;
        QVERIFY_THROWS_EXCEPTION(Error, f.semantic->refresh());
        QVERIFY(before.open(QIODevice::ReadOnly)); QCOMPARE(before.readAll(), original); before.close();
        QVERIFY(before.open(QIODevice::WriteOnly | QIODevice::Truncate)); before.write("bad"); before.close();
        QVERIFY_THROWS_EXCEPTION(Error, f.semantic->refresh());
        QVERIFY_THROWS_EXCEPTION(Error, (void)f.semantic->search("recover", 5));
    }
    void hybridIncludesLexicalAndSemanticMatches() {
        Fixture f;
        f.knowledge->upsert(source("art", "The painting is safely stored."));
        f.knowledge->upsert(source("keyword", "Recover a lost layer from the archive."));
        f.semantic->refresh();
        a::HybridKnowledgeRetriever hybrid(f.knowledge, f.semantic);
        const auto hits = hybrid.search("recover", 10);
        QCOMPARE(hits.size(), 2);
        // The keyword hit appears in both ranked lists, so reciprocal rank
        // fusion boosts it above the semantic-only hit.
        QCOMPARE(hits.front().sourceId, QString("keyword"));
        QCOMPARE(hits.back().sourceId, QString("art"));
        const auto lexical = f.knowledge->search("recover", 5);
        QVERIFY(!lexical.isEmpty()); QCOMPARE(lexical.front().sourceId, QString("keyword"));
    }
    void cancellationAndBadQueryStopWork() {
        Fixture f; f.knowledge->upsert(source("art", "A painting."));
        CancellationToken token; token.cancel();
        QVERIFY_THROWS_EXCEPTION(Error, f.semantic->refresh(token));
        QVERIFY_THROWS_EXCEPTION(Error, (void)f.semantic->search("recover", 5, token));
        QVERIFY_THROWS_EXCEPTION(Error, (void)f.semantic->search(" ", 5));
        QVERIFY_THROWS_EXCEPTION(Error, (void)a::SemanticKnowledgeIndex(f.knowledge, f.embeddings, f.root.filePath("sources.json")));
    }
    void modelSizedChunksKeepExactUtf8Offsets() {
        Fixture f;
        const auto text = QString("painting ") + QString(200, QChar(u'가'));
        f.knowledge->upsert(source("art", text));
        a::SemanticOptions options; options.minimumSimilarity = 0.1;
        options.maxChunkCodeUnits = 64; options.overlapCodeUnits = 8;
        a::SemanticKnowledgeIndex shortChunks(f.knowledge, f.embeddings, f.vectors, options);
        shortChunks.refresh();
        const auto hits = shortChunks.search("recover", 5);
        QVERIFY(!hits.isEmpty());
        for (const auto& hit : hits) {
            QVERIFY(hit.text.size() <= 64);
            QCOMPARE(text.toUtf8().mid(hit.startByte, hit.endByte - hit.startByte), hit.text.toUtf8());
        }
        QVERIFY(f.embeddings->documents > 1);
    }
    void changingSourcesDuringRefreshCannotPublishMixedVectors() {
        Fixture f; f.knowledge->upsert(source("art", "A painting.")); f.semantic->refresh();
        QFile file(f.vectors); QVERIFY(file.open(QIODevice::ReadOnly)); const auto original = file.readAll(); file.close();
        f.knowledge->upsert(source("art", "The painting changed."));
        f.embeddings->onDocument = [&] { f.knowledge->upsert(source("other", "A recipe.")); };
        QVERIFY_THROWS_EXCEPTION(Error, f.semantic->refresh());
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), original); file.close();
        f.semantic->refresh();
        f.embeddings->wrongQueryDimension = true;
        QVERIFY_THROWS_EXCEPTION(Error, (void)f.semantic->search("recover", 5));
    }
    void localProviderUsesServiceAndFingerprintsModelConfiguration() {
        Fixture f;
        const auto package = f.root.filePath("package"); QVERIFY(QDir().mkpath(package));
        QFile weights(QDir(package).filePath("model.test")); QVERIFY(weights.open(QIODevice::WriteOnly));
        QCOMPARE(weights.write("fixture"), qint64(7)); weights.close();
        ModelManifest manifest{"local-embed", "test", "test", "none", 128, {"embedding"}, "model.test"};
        QFile metadata(QDir(package).filePath("manifest.json")); QVERIFY(metadata.open(QIODevice::WriteOnly));
        QVERIFY(metadata.write(QJsonDocument(manifestObject(manifest)).toJson()) > 0); metadata.close();
        ServiceOptions options; options.modelsDirectory = f.root.filePath("models"); options.keepAliveMs = 300000;
        Service service(options); service.registerRuntime(std::make_shared<LocalRuntime>()).get();
        (void)service.installModel(package).get(); (void)service.loadModel({"model://local-embed", 128}).get();
        a::LocalEmbeddingProvider provider(service, {"model://local-embed", "query: ", "passage: "});
        const auto first = provider.fingerprint(); QCOMPARE(first.size(), 64);
        QCOMPARE(provider.embed("hello", a::EmbeddingPurpose::Query, {})[0], 1.0f);
        QCOMPARE(provider.embed("hello", a::EmbeddingPurpose::Document, {})[1], 1.0f);
        service.unloadModel("model://local-embed").get();
        QVERIFY_THROWS_EXCEPTION(Error, (void)provider.fingerprint());
        (void)service.loadModel({"model://local-embed", 128, {{"fixture_config", "changed"}}}).get();
        QVERIFY(provider.fingerprint() != first);
    }
};

QTEST_GUILESS_MAIN(SemanticRagTests)
#include "semantic_rag_tests.moc"

#include "SemanticRag.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

namespace iiMultimodalAgent::agent {
namespace {
constexpr qint64 maxCacheBytes = 64 * 1024 * 1024;
constexpr int maxChunks = 8192, maxDimension = 4096;
constexpr qint64 maxCoordinates = 2 * 1024 * 1024;
void require(bool ok, const QString& message, ErrorCode code = ErrorCode::InvalidArgument) {
    if (!ok) throw Error(code, message);
}
QString digest(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QString key(const KnowledgeHit& hit) {
    return digest(QJsonDocument(QJsonArray{hit.sourceId, hit.page, hit.sha256, hit.startByte, hit.endByte})
        .toJson(QJsonDocument::Compact));
}
QString fusionKey(const KnowledgeHit& hit) {
    return digest(QJsonDocument(QJsonArray{hit.sourceId, hit.sourceUri, hit.revision, hit.locator,
        hit.page, hit.sha256, hit.startByte, hit.endByte}).toJson(QJsonDocument::Compact));
}
void queryLimits(const QString& query, int limit) {
    const auto bytes = query.toUtf8();
    require(!query.trimmed().isEmpty() && bytes.size() <= 4096 && !query.contains(QChar(0))
        && QString::fromUtf8(bytes) == query && limit >= 1 && limit <= 50, "Invalid semantic query or limit");
}
QString fingerprint(const EmbeddingProvider& provider, const CancellationToken& token) {
    token.throwIfCancelled();
    const auto value = provider.fingerprint(token);
    const auto bytes = value.toUtf8();
    require(!value.trimmed().isEmpty() && bytes.size() <= 1024 && !value.contains(QChar(0))
        && QString::fromUtf8(bytes) == value,
            "Invalid embedding fingerprint");
    return value;
}
QList<float> unit(const QList<float>& values) {
    require(!values.isEmpty() && values.size() <= maxDimension, "Invalid embedding dimension", ErrorCode::IntegrityFailure);
    double squared = 0;
    for (float value : values) {
        require(std::isfinite(value), "Non-finite embedding", ErrorCode::IntegrityFailure);
        squared += double(value) * double(value);
    }
    require(std::isfinite(squared) && squared > 0, "Zero or invalid embedding", ErrorCode::IntegrityFailure);
    const double norm = std::sqrt(squared);
    QList<float> result; result.reserve(values.size());
    for (float value : values) result.append(float(double(value) / norm));
    return result;
}
struct Cache {
    QString fingerprint;
    int dimension = 0;
    std::map<QString, QList<float>> vectors;
};
Cache readCache(const QString& path, bool missingAllowed) {
    const QFileInfo info(path);
    require(!info.isSymLink(), "Vector cache may not be a symbolic link", ErrorCode::StorageFailure);
    if (!info.exists()) {
        require(missingAllowed, "Semantic index is not built; refresh it first", ErrorCode::NotFound);
        return {};
    }
    QFile file(path);
    require(info.isFile() && file.open(QIODevice::ReadOnly), "Cannot read vector cache", ErrorCode::StorageFailure);
    const auto bytes = file.read(maxCacheBytes + 1);
    require(bytes.size() <= maxCacheBytes && file.error() == QFile::NoError,
            "Vector cache exceeds its size limit or cannot be read", ErrorCode::StorageFailure);
    QJsonParseError error;
    const auto parsed = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && parsed.isObject(), "Invalid vector cache", ErrorCode::IntegrityFailure);
    const auto object = parsed.object();
    require(object.value("schema") == "iisacc.embeddings/1" && object.value("fingerprint").isString()
        && object.value("dimension").isDouble() && object.value("vectors").isArray(),
        "Unsupported vector cache schema", ErrorCode::IntegrityFailure);
    Cache result; result.fingerprint = object.value("fingerprint").toString();
    const double dimension = object.value("dimension").toDouble();
    require(!result.fingerprint.isEmpty() && std::isfinite(dimension) && dimension >= 0
        && dimension <= maxDimension && std::floor(dimension) == dimension, "Invalid vector cache metadata", ErrorCode::IntegrityFailure);
    result.dimension = int(dimension);
    const auto rows = object.value("vectors").toArray();
    require(rows.size() <= maxChunks && qint64(rows.size()) * result.dimension <= maxCoordinates
        && (rows.isEmpty() == (result.dimension == 0)),
            "Invalid vector cache size", ErrorCode::IntegrityFailure);
    for (const auto& value : rows) {
        require(value.isObject(), "Invalid vector cache entry", ErrorCode::IntegrityFailure);
        const auto row = value.toObject();
        const auto id = row.value("key").toString();
        const auto vector = row.value("value").toArray();
        require(id.size() == 64 && row.value("value").isArray() && vector.size() == result.dimension,
                "Invalid cached embedding", ErrorCode::IntegrityFailure);
        QList<float> values; values.reserve(vector.size());
        for (const auto& number : vector) {
            require(number.isDouble() && std::isfinite(number.toDouble()), "Invalid cached coordinate", ErrorCode::IntegrityFailure);
            values.append(float(number.toDouble()));
        }
        auto normalized = unit(values);
        require(result.vectors.emplace(id, std::move(normalized)).second, "Duplicate cached embedding", ErrorCode::IntegrityFailure);
    }
    return result;
}
void writeCache(const QString& path, const Cache& cache, const CancellationToken& token) {
    QJsonArray entries;
    for (const auto& [id, values] : cache.vectors) {
        token.throwIfCancelled();
        QJsonArray vector; for (float value : values) vector.append(double(value));
        entries.append(QJsonObject{{"key", id}, {"value", vector}});
    }
    const auto bytes = QJsonDocument(QJsonObject{{"schema", "iisacc.embeddings/1"},
        {"fingerprint", cache.fingerprint}, {"dimension", cache.dimension}, {"vectors", entries}}).toJson(QJsonDocument::Compact);
    require(bytes.size() <= maxCacheBytes, "Vector cache exceeds its size limit", ErrorCode::ResourceLimit);
    require(!QFileInfo(path).isSymLink(), "Vector cache may not be a symbolic link", ErrorCode::StorageFailure);
    QSaveFile file(path); file.setDirectWriteFallback(false);
    require(file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner),
            "Cannot create private vector cache", ErrorCode::StorageFailure);
    require(file.write(bytes) == bytes.size(), "Cannot write vector cache", ErrorCode::StorageFailure);
    token.throwIfCancelled();
    require(file.commit(), "Cannot commit vector cache", ErrorCode::StorageFailure);
}
std::map<QString, KnowledgeHit> snapshot(const KnowledgeIndex& source, const SemanticOptions& options, const CancellationToken& token) {
    const auto chunks = source.chunks(options.maxChunkCodeUnits, options.overlapCodeUnits, token);
    require(chunks.size() <= maxChunks, "Knowledge snapshot exceeds vector index limit", ErrorCode::ResourceLimit);
    std::map<QString, KnowledgeHit> result;
    for (const auto& hit : chunks) {
        token.throwIfCancelled();
        require(result.emplace(key(hit), hit).second, "Duplicate knowledge chunk", ErrorCode::IntegrityFailure);
    }
    return result;
}
template<class T> T await(std::future<T> future, const CancellationToken& token) {
    while (future.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) token.throwIfCancelled();
    token.throwIfCancelled();
    return future.get();
}
}

LocalEmbeddingProvider::LocalEmbeddingProvider(Service& service, LocalEmbeddingOptions options)
    :service_(&service), options_(std::move(options)) {
    const auto valid = [](const QString& value, int max) {
        const auto bytes = value.toUtf8();
        return bytes.size() <= max && !value.contains(QChar(0)) && QString::fromUtf8(bytes) == value;
    };
    require(!options_.model.trimmed().isEmpty() && options_.model.toUtf8().size() <= 1024
        && valid(options_.model, 1024) && valid(options_.queryPrefix, 4096) && valid(options_.documentPrefix, 4096),
        "Invalid local embedding options");
}
QString LocalEmbeddingProvider::fingerprint(const CancellationToken& token) const {
    const auto record = await(service_->resolveModel(options_.model), token);
    require(record.manifest.capabilities.contains("embedding") && !record.manifest.files.isEmpty(),
            "Model must declare embedding and contain fingerprinted files", ErrorCode::RuntimeUnavailable);
    for (const auto& file : record.manifest.files)
        require(file.sha256.size() == 64, "Model file has no SHA-256 fingerprint", ErrorCode::IntegrityFailure);
    const auto loaded = await(service_->models(), token);
    for (const auto& info : loaded) if (info.model.manifest.id == record.manifest.id) {
        require(iiLocalLLM::manifestObject(info.model.manifest) == iiLocalLLM::manifestObject(record.manifest),
                "Loaded embedding model no longer matches the installed manifest", ErrorCode::IntegrityFailure);
        const QJsonObject descriptor{{"adapter", "iilocal-embedding/1"},
            {"manifest", iiLocalLLM::manifestObject(record.manifest)}, {"context", info.contextTokens},
            {"options", info.options}, {"query_prefix", options_.queryPrefix}, {"document_prefix", options_.documentPrefix}};
        return digest(QJsonDocument(descriptor).toJson(QJsonDocument::Compact));
    }
    throw Error(ErrorCode::RuntimeUnavailable, "Load the embedding model before indexing or searching");
}
QList<float> LocalEmbeddingProvider::embed(const QString& text, EmbeddingPurpose purpose, const CancellationToken& token) const {
    const auto prefix = purpose == EmbeddingPurpose::Query ? options_.queryPrefix : options_.documentPrefix;
    return await(service_->embedText(options_.model, prefix + text, token), token);
}

class SemanticKnowledgeIndex::Impl {
public:
    Impl(std::shared_ptr<const KnowledgeIndex> source, std::shared_ptr<const EmbeddingProvider> provider,
         QString path, SemanticOptions options)
        :source(std::move(source)), provider(std::move(provider)), path(std::move(path)), options(options) {
        const QFileInfo info(this->path);
        require(this->source && this->provider && info.isAbsolute() && QFileInfo(info.absolutePath()).isDir()
            && !info.isSymLink() && std::isfinite(options.minimumSimilarity)
            && options.minimumSimilarity >= -1 && options.minimumSimilarity <= 1
            && options.maxChunkCodeUnits >= 32 && options.maxChunkCodeUnits <= 900
            && options.overlapCodeUnits >= 0 && options.overlapCodeUnits < options.maxChunkCodeUnits / 2,
            "Invalid semantic index configuration");
        this->path = info.absoluteFilePath();
        const QFileInfo sourceInfo(this->source->storagePath());
        const auto sourcePath = (QFileInfo(sourceInfo.absolutePath()).canonicalFilePath() + '/' + sourceInfo.fileName()).toCaseFolded();
        const auto vectorPath = (QFileInfo(info.absolutePath()).canonicalFilePath() + '/' + info.fileName()).toCaseFolded();
        require(sourcePath != vectorPath && sourcePath + ".lock" != vectorPath && vectorPath + ".lock" != sourcePath,
                "Vector cache must not overlap the knowledge store or its lock");
    }
    std::shared_ptr<const KnowledgeIndex> source;
    std::shared_ptr<const EmbeddingProvider> provider;
    QString path;
    SemanticOptions options;
};
SemanticKnowledgeIndex::SemanticKnowledgeIndex(std::shared_ptr<const KnowledgeIndex> source,
    std::shared_ptr<const EmbeddingProvider> provider, QString path, SemanticOptions options)
    :d(std::make_shared<Impl>(std::move(source), std::move(provider), std::move(path), options)) {}
void SemanticKnowledgeIndex::refresh(const CancellationToken& token) {
    token.throwIfCancelled();
    QLockFile lock(d->path + ".lock");
    require(lock.tryLock(5000), "Vector cache is locked", ErrorCode::Timeout);
    const auto initial = snapshot(*d->source, d->options, token);
    const auto model = fingerprint(*d->provider, token);
    const auto previous = readCache(d->path, true);
    Cache next; next.fingerprint = model;
    bool changed = previous.fingerprint != model || previous.vectors.size() != initial.size();
    for (const auto& [id, hit] : initial) {
        token.throwIfCancelled();
        QList<float> vector;
        const auto old = previous.vectors.find(id);
        if (previous.fingerprint == model && old != previous.vectors.end()) vector = old->second;
        else { changed = true; vector = unit(d->provider->embed(hit.text, EmbeddingPurpose::Document, token)); }
        if (!next.dimension) next.dimension = int(vector.size());
        require(vector.size() == next.dimension, "Embedding dimensions changed", ErrorCode::IntegrityFailure);
        require(qint64(initial.size()) * next.dimension <= maxCoordinates,
                "Embedding index exceeds coordinate limit", ErrorCode::ResourceLimit);
        next.vectors.emplace(id, std::move(vector));
    }
    require(fingerprint(*d->provider, token) == model, "Embedding model changed during indexing", ErrorCode::IntegrityFailure);
    const auto current = snapshot(*d->source, d->options, token);
    require(current.size() == initial.size() && std::equal(initial.begin(), initial.end(), current.begin(),
        [](const auto& a, const auto& b) { return a.first == b.first; }),
        "Knowledge changed during indexing; retry refresh", ErrorCode::IntegrityFailure);
    if (!changed) return;
    writeCache(d->path, next, token);
}
QList<KnowledgeHit> SemanticKnowledgeIndex::search(const QString& query, int limit, const CancellationToken& token) const {
    queryLimits(query, limit); token.throwIfCancelled();
    const auto chunks = snapshot(*d->source, d->options, token);
    const auto model = fingerprint(*d->provider, token);
    const auto cache = readCache(d->path, false);
    require(cache.fingerprint == model && cache.vectors.size() == chunks.size(),
            "Semantic index is stale; refresh it first", ErrorCode::IntegrityFailure);
    for (const auto& [id, hit] : chunks) {
        (void)hit;
        require(cache.vectors.contains(id), "Semantic index is stale; refresh it first", ErrorCode::IntegrityFailure);
    }
    if (chunks.empty()) return {};
    const auto vector = unit(d->provider->embed(query, EmbeddingPurpose::Query, token));
    require(vector.size() == cache.dimension && fingerprint(*d->provider, token) == model,
            "Embedding model or dimension changed", ErrorCode::IntegrityFailure);
    QList<KnowledgeHit> result;
    for (const auto& [id, hit] : chunks) {
        token.throwIfCancelled();
        const auto& candidate = cache.vectors.at(id);
        double cosine = 0;
        for (qsizetype i = 0; i < vector.size(); ++i) cosine += double(vector[i]) * double(candidate[i]);
        cosine = std::clamp(cosine, -1.0, 1.0);
        if (cosine < d->options.minimumSimilarity) continue;
        auto item = hit; item.rankingScore = cosine; result.append(std::move(item));
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.rankingScore != b.rankingScore) return a.rankingScore > b.rankingScore;
        if (a.sourceId != b.sourceId) return a.sourceId < b.sourceId;
        if (a.page != b.page) return a.page < b.page;
        return a.startByte < b.startByte;
    });
    if (result.size() > limit) result.resize(limit);
    return result;
}

HybridKnowledgeRetriever::HybridKnowledgeRetriever(std::shared_ptr<const KnowledgeRetriever> lexical,
    std::shared_ptr<const KnowledgeRetriever> semantic):lexical_(std::move(lexical)), semantic_(std::move(semantic)) {
    require(lexical_ && semantic_, "Hybrid retrieval requires two retrievers");
}
QList<KnowledgeHit> HybridKnowledgeRetriever::search(const QString& query, int limit, const CancellationToken& token) const {
    queryLimits(query, limit); token.throwIfCancelled();
    std::map<QString, KnowledgeHit> combined;
    auto add = [&](const QList<KnowledgeHit>& hits) {
        require(hits.size() <= 50, "Retriever exceeded hybrid candidate limit", ErrorCode::IntegrityFailure);
        QSet<QString> seen;
        for (qsizetype i = 0; i < hits.size(); ++i) {
            token.throwIfCancelled();
            (void)toJson(hits[i]); // Validate source metadata and byte range before exposing it.
            const auto id = fusionKey(hits[i]);
            require(!seen.contains(id), "Duplicate hybrid candidate", ErrorCode::IntegrityFailure);
            seen.insert(id);
            const double rank = 1.0 / (60.0 + double(i + 1));
            const auto it = combined.find(id);
            if (it == combined.end()) { auto item = hits[i]; item.rankingScore = rank; combined.emplace(id, std::move(item)); }
            else it->second.rankingScore += rank;
        }
    };
    add(lexical_->search(query, 50, token));
    add(semantic_->search(query, 50, token));
    QList<KnowledgeHit> result; for (const auto& [id, hit] : combined) { (void)id; result.append(hit); }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.rankingScore != b.rankingScore) return a.rankingScore > b.rankingScore;
        if (a.sourceId != b.sourceId) return a.sourceId < b.sourceId;
        if (a.page != b.page) return a.page < b.page;
        return a.startByte < b.startByte;
    });
    if (result.size() > limit) result.resize(limit);
    return result;
}
} // namespace iiMultimodalAgent::agent

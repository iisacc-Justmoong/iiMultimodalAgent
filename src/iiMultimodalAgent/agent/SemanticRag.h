#pragma once

#include "Rag.h"

namespace iiMultimodalAgent::agent {

enum class EmbeddingPurpose { Query, Document };

// The fingerprint must change with weights, pooling, preprocessing or prefixes.
// Implementations must be safe for concurrent use and honor cancellation.
class IIMULTIMODALAGENT_EXPORT EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;
    virtual QString fingerprint(const CancellationToken& = {}) const = 0;
    virtual QList<float> embed(const QString&, EmbeddingPurpose, const CancellationToken&) const = 0;
};

struct LocalEmbeddingOptions {
    QString model; // A model:// reference already loaded in the host's Service.
    QString queryPrefix;
    QString documentPrefix;
};

// The host retains Service and controls model loading and residency.
class IIMULTIMODALAGENT_EXPORT LocalEmbeddingProvider final : public EmbeddingProvider {
public:
    LocalEmbeddingProvider(Service&, LocalEmbeddingOptions);
    QString fingerprint(const CancellationToken& = {}) const override;
    QList<float> embed(const QString&, EmbeddingPurpose, const CancellationToken&) const override;
private:
    Service* service_;
    LocalEmbeddingOptions options_;
};

struct SemanticOptions {
    // Cosine threshold is a ranking cutoff, never a probability.
    double minimumSimilarity = 0.0;
    int maxChunkCodeUnits = 512;
    int overlapCodeUnits = 64;
};

// Separate atomic vector cache over a KnowledgeIndex. refresh() is explicit;
// search fails closed when the source snapshot or model fingerprint is stale.
class IIMULTIMODALAGENT_EXPORT SemanticKnowledgeIndex final : public KnowledgeRetriever {
public:
    SemanticKnowledgeIndex(std::shared_ptr<const KnowledgeIndex>, std::shared_ptr<const EmbeddingProvider>,
                           QString absoluteVectorPath, SemanticOptions = {});
    void refresh(const CancellationToken& = {});
    QList<KnowledgeHit> search(const QString&, int limit, const CancellationToken& = {}) const override;
private:
    class Impl;
    std::shared_ptr<Impl> d;
};

// Combines two ranked retrievers by reciprocal rank fusion, not score averaging.
class IIMULTIMODALAGENT_EXPORT HybridKnowledgeRetriever final : public KnowledgeRetriever {
public:
    HybridKnowledgeRetriever(std::shared_ptr<const KnowledgeRetriever> lexical,
                             std::shared_ptr<const KnowledgeRetriever> semantic);
    QList<KnowledgeHit> search(const QString&, int limit, const CancellationToken& = {}) const override;
private:
    std::shared_ptr<const KnowledgeRetriever> lexical_, semantic_;
};

} // namespace iiMultimodalAgent::agent

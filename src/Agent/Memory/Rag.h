#pragma once

#include <iiMultimodalAgent/agent/Tools.h>
#include <functional>
#include <memory>

namespace iiMultimodalAgent::agent {

// The host extracts and supplies text. A caption or transcript is text, not
// proof that this component has inspected the corresponding media bytes.
struct KnowledgePage {
    QString locator; // Host-owned page, time range, canvas object, or other locator.
    QString text;
};
struct KnowledgeSource {
    QString id, title, uri, revision, kind;
    QList<KnowledgePage> pages;
};
struct KnowledgeHit {
    QString sourceId, title, sourceUri, revision, kind, locator, sha256, text;
    int page = 0; // One-based index into the submitted pages.
    qint64 startByte = 0, endByte = 0; // UTF-8 byte interval in the submitted page.
    double rankingScore = 0; // Retriever-specific ranking signal, not a calibrated probability.
};

class IIMULTIMODALAGENT_EXPORT KnowledgeRetriever {
public:
    virtual ~KnowledgeRetriever() = default;
    // Implementations may be called concurrently and must cooperate with cancellation.
    virtual QList<KnowledgeHit> search(const QString& query, int limit,
                                       const CancellationToken& = {}) const = 0;
};

// Explicitly populated local snapshot. No automatic scanning or network access.
// Updates are process-locked and atomically replace the JSON snapshot.
class IIMULTIMODALAGENT_EXPORT KnowledgeIndex final : public KnowledgeRetriever {
public:
    explicit KnowledgeIndex(QString absoluteFilePath);
    QString storagePath() const;
    void upsert(const KnowledgeSource&, const CancellationToken& = {});
    bool remove(const QString& sourceId, const CancellationToken& = {});
    // A point-in-time chunk snapshot, including pages with no lexical match.
    QList<KnowledgeHit> chunks(const CancellationToken& = {}) const;
    QList<KnowledgeHit> chunks(int maxCodeUnits, int overlapCodeUnits, const CancellationToken& = {}) const;
    QList<KnowledgeHit> search(const QString&, int limit,
                               const CancellationToken& = {}) const override;
private:
    QString path_;
};

IIMULTIMODALAGENT_EXPORT QJsonObject toJson(const KnowledgeSource&);
IIMULTIMODALAGENT_EXPORT KnowledgeSource knowledgeSourceFromJson(const QJsonObject&);
IIMULTIMODALAGENT_EXPORT QJsonObject toJson(const KnowledgeHit&);

struct EvidenceJudgment {
    long double relevance = 0, answerability = 0;
    bool complete = false;
};
struct RagOptions {
    int initialLimit = 6, expandedLimit = 24, maxEvidenceBytes = 16000;
    long double minRelevance = 0.7L, minAnswerability = 0.8L;
    // Optional trusted, calibrated host scorer. When absent, lexical matches
    // are presented without asserting relevance or answerability probabilities.
    std::function<EvidenceJudgment(const QString&, const KnowledgeHit&, const CancellationToken&)> score;
};
enum class RagStatus { Answered, InsufficientEvidence, InvalidCitations };
struct RagResult {
    RagStatus status = RagStatus::InsufficientEvidence;
    QString text;
    QList<KnowledgeHit> evidence; // Evidence supplied to the model, not claim-level verification.
    int attempts = 0;
    bool decisionGateUsed = false;
    Usage usage;
};

// Direct host API: it calls the injected Model without an Engine tool policy.
// Hosts that require Engine permission/decision handling should use the tool.
class IIMULTIMODALAGENT_EXPORT Rag final {
public:
    Rag(std::shared_ptr<const KnowledgeRetriever>, std::shared_ptr<Model>, RagOptions = {});
    RagResult answer(const QString& query, const QString& modelName,
                     const CancellationToken& = {}) const;
private:
    std::shared_ptr<const KnowledgeRetriever> retriever_;
    std::shared_ptr<Model> model_;
    RagOptions options_;
};

// The authorizer is mandatory and evaluates every invocation. It must enforce
// tenant/session ownership; workspace equality alone is not tenant isolation.
IIMULTIMODALAGENT_EXPORT Tool knowledgeSearchTool(
    std::shared_ptr<const KnowledgeRetriever>, QString workspace,
    std::function<bool(const ToolContext&)> authorize, bool deferred = false);

} // namespace iiMultimodalAgent::agent

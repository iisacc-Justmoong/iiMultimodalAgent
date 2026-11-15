#include "NativeQuestionBridge.h"

#include <QtCore/QThread>
#include <stdexcept>

namespace iiMultimodalAgent::agent::presentation {
namespace {
std::string utf8(const QString& text) {
    const auto bytes = text.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}
QString qstring(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
QuestionRequest fromVariant(const QVariantMap& row) {
    QuestionRequest result;
    result.id = utf8(row.value("request_id").toString());
    const auto request = row.value("request").toMap();
    const auto input = request.value("input").toMap();
    for (const auto& item : input.value("questions").toList()) {
        const auto source = item.toMap();
        Question question;
        question.text = utf8(source.value("question").toString());
        question.header = utf8(source.value("header").toString());
        question.multiple = source.value("multiSelect").toBool();
        for (const auto& value : source.value("options").toList()) {
            const auto option = value.toMap();
            question.options.push_back({utf8(option.value("label").toString()),
                utf8(option.value("description").toString()), utf8(option.value("preview").toString())});
        }
        result.questions.push_back(std::move(question));
    }
    return result;
}
}

NativeQuestionBridge::NativeQuestionBridge(QuestionInbox* inbox, std::shared_ptr<NativeQuestionViewModel> model,
                                           QObject* parent)
    : QObject(parent), inbox_(inbox), model_(model) {
    if (!inbox || !model || inbox->thread() != QThread::currentThread() || thread() != QThread::currentThread())
        throw std::invalid_argument("The question bridge requires an inbox and model on its owner thread");
    const QPointer<NativeQuestionBridge> self(this);
    model->setActions([self](const QuestionResponse& response) { return self && self->submit(response); },
                      [self](std::string_view id) { return self && self->reject(id); });
    connect(inbox, &QuestionInbox::requestDelta, this, [this](const QString& id) { changed(id); });
    connect(inbox, &QuestionInbox::requestsReconciled, this, [this] { (void)reconcile(); });
    connect(inbox, &QuestionInbox::errorChanged, this, [this] { errorChanged(); });
    const auto weak = model_;
    connect(inbox, &QObject::destroyed, this, [weak] {
        if (auto target = weak.lock()) (void)target->update.send({{}, ""});
    });
    if (!reconcile()) throw std::invalid_argument("Could not reconcile the initial question inbox");
}

bool NativeQuestionBridge::reconcile() {
    auto model = model_.lock();
    if (!model || !inbox_) return false;
    QuestionUpdate value;
    for (const auto& item : inbox_->requests()) value.requests.push_back(fromVariant(item.toMap()));
    value.error = utf8(inbox_->errorString());
    return model->update.send(value).has_value();
}
void NativeQuestionBridge::changed(const QString& id) {
    auto model = model_.lock();
    if (!model || !inbox_) return;
    const auto item = inbox_->requestSnapshot(id);
    QuestionDelta value{.id = utf8(id)};
    if (item.position >= 0) {
        value.value = fromVariant(item.request);
        value.index = static_cast<std::size_t>(item.position);
    }
    // A gap, stale index or inconsistent payload requires one explicit
    // reconciliation; unchanged notifications do not trigger a full refresh.
    if (!model->change.send(value)) (void)reconcile();
}
void NativeQuestionBridge::errorChanged() {
    auto model = model_.lock();
    if (model && inbox_) (void)model->setError.send({utf8(inbox_->errorString())});
}
bool NativeQuestionBridge::submit(const QuestionResponse& response) {
    const auto inbox = inbox_;
    if (!inbox) return false;
    QVariantMap answers, annotations;
    for (const auto& [key, value] : response.answers) answers.insert(qstring(key), qstring(value));
    for (const auto& [key, value] : response.annotations) {
        QVariantMap annotation;
        if (!value.preview.empty()) annotation.insert("preview", qstring(value.preview));
        if (!value.notes.empty()) annotation.insert("notes", qstring(value.notes));
        annotations.insert(qstring(key), annotation);
    }
    return inbox->submit(qstring(response.id), answers, annotations);
}
bool NativeQuestionBridge::reject(std::string_view id) {
    const auto inbox = inbox_;
    return inbox && inbox->reject(qstring(id));
}
} // namespace iiMultimodalAgent::agent::presentation

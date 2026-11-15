#pragma once

#include <iiMultimodalAgent/agent/NativeQuestionViewModel.h>
#include <iiMultimodalAgent/agent/QuestionInbox.h>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <memory>

namespace iiMultimodalAgent::agent::presentation {
// Optional Qt transport adapter; no view, LVUI, or iiGUI dependency. It borrows
// the inbox and model and observes their existing owner-thread events.
class IIMULTIMODALAGENT_EXPORT NativeQuestionBridge final : public QObject {
public:
    NativeQuestionBridge(QuestionInbox* inbox, std::shared_ptr<NativeQuestionViewModel> model,
                         QObject* parent = nullptr);
    // Explicit full recovery after reconnect or suspected missed notifications.
    [[nodiscard]] bool reconcile();
private:
    void changed(const QString& id);
    void errorChanged();
    bool submit(const QuestionResponse& result);
    bool reject(std::string_view id);
    QPointer<QuestionInbox> inbox_;
    std::weak_ptr<NativeQuestionViewModel> model_;
};
} // namespace iiMultimodalAgent::agent::presentation

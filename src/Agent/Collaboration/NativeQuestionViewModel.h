#pragma once

#include <iiMultimodalAgent/Export.h>
#include <iiSDKCore/Core/Property/Observable.h>
#include <iiSDKCore/Runtime/Query.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace iiMultimodalAgent::agent::presentation {

struct QuestionOption final {
    std::string label, description, preview;
    bool operator==(const QuestionOption&) const = default;
};
struct Question final {
    std::string text, header;
    bool multiple = false;
    std::vector<QuestionOption> options;
    bool operator==(const Question&) const = default;
};
struct QuestionRequest final {
    std::string id;
    std::vector<Question> questions;
    bool operator==(const QuestionRequest&) const = default;
};
struct QuestionUpdate final {
    std::vector<QuestionRequest> requests;
    std::string error;
    bool operator==(const QuestionUpdate&) const = default;
};
// A single request notification. index is the final position after removing
// the old entry; a missing value removes id. Full updates are for initial or
// explicitly requested reconciliation.
struct QuestionDelta final {
    std::string id;
    std::optional<QuestionRequest> value;
    std::size_t index = 0;
};
struct QuestionAnnotation final {
    std::string preview, notes;
};
struct QuestionResponse final {
    std::string id;
    std::map<std::string, std::string> answers;
    std::map<std::string, QuestionAnnotation> annotations;
};
struct QuestionStrings final {
    std::string title = "Agent questions";
    std::string description = "Choose an option or write your own answer. You can leave questions unanswered.";
    std::string waiting = "%1 requests waiting";
    std::string other = "Your own answer";
    std::string notes = "Additional notes (optional)";
    std::string otherAccessibility = "Your answer to %1";
    std::string notesAccessibility = "Notes for %1";
    std::string decline = "Decline", skip = "Skip", send = "Send answers";
};

class QuestionRow;
class IIMULTIMODALAGENT_EXPORT QuestionOptionRow final {
public:
    const std::string id, label, description, preview;
    ii::core::Observable<bool> checked{false};
    void toggled(bool value);
    void clicked();
    void showPreview();
private:
    friend class QuestionRow;
    QuestionOptionRow(std::string id, QuestionOption option, std::weak_ptr<QuestionRow> owner, std::size_t index);
    std::weak_ptr<QuestionRow> owner_;
    std::size_t index_;
};

class IIMULTIMODALAGENT_EXPORT QuestionRow final : public std::enable_shared_from_this<QuestionRow> {
public:
    const std::string id, text, header;
    const bool multiple;
    std::vector<std::shared_ptr<QuestionOptionRow>> options;
    ii::core::Observable<std::string> other{""}, notes{""}, preview{""}, otherAccessibility{""}, notesAccessibility{""};
    [[nodiscard]] bool active() const;
    void editOther(std::string value);
    void editNotes(std::string value);
private:
    friend class QuestionOptionRow;
    friend class NativeQuestionViewModel;
    QuestionRow(std::string id, const Question& question);
    static std::shared_ptr<QuestionRow> make(std::string id, const Question& question);
    void select(std::size_t index, bool value);
    void showPreview(std::size_t index);
    void localize(const QuestionStrings& strings);
    void retire();
    bool active_ = true;
};

struct ReadQuestions final {};
struct QuestionAction final {};
struct QuestionError final { std::string message; };

class IIMULTIMODALAGENT_EXPORT NativeQuestionViewModel final {
    struct Lifetime;
    QuestionUpdate snapshot_;
    QuestionStrings strings_;
    std::function<bool(const QuestionResponse&)> submitAction_;
    std::function<bool(std::string_view)> declineAction_;
    bool updating_ = false;
    std::shared_ptr<Lifetime> lifetime_;
public:
    NativeQuestionViewModel();
    ~NativeQuestionViewModel();
    void setStrings(QuestionStrings strings);
    void setActions(std::function<bool(const QuestionResponse&)> submit,
                    std::function<bool(std::string_view)> decline);
    ii::core::Observable<std::vector<std::shared_ptr<QuestionRow>>> rows{{}};
    ii::core::Observable<bool> open{false}, hasWaiting{false}, hasError{false}, motionEnabled{true};
    ii::core::Observable<std::string> title{""}, description{""}, waiting{""}, error{""},
        otherPlaceholder{""}, notesPlaceholder{""}, declineText{""}, skipText{""}, sendText{""};
    ii::runtime::QueryPort<QuestionUpdate, bool> update;
    ii::runtime::QueryPort<QuestionDelta, bool> change;
    ii::runtime::QueryPort<QuestionError, bool> setError;
    ii::runtime::QueryPort<ReadQuestions, QuestionUpdate> read;
    ii::runtime::QueryPort<QuestionAction, bool> submitQuery, skipQuery, declineQuery;
    void submit();
    void skip();
    void decline();
private:
    bool apply(const QuestionUpdate& value);
    bool applyChange(const QuestionDelta& value);
    bool applyError(const QuestionError& value);
    bool sendResponse(bool skip);
    bool sendDecline();
    void refreshStrings();
};

// A generated LVUI scope can construct this adapter without owning the inbox.
// The application may instead supply the same model to both this adapter and
// its transport bridge through LVUI's backend factory.
class IIMULTIMODALAGENT_EXPORT NativeQuestionBackend final {
public:
    NativeQuestionBackend();
    explicit NativeQuestionBackend(std::shared_ptr<NativeQuestionViewModel> model);
    std::shared_ptr<NativeQuestionViewModel> model;
    ii::core::Observable<std::vector<std::shared_ptr<QuestionRow>>>& rows;
    ii::core::Observable<bool>& open, &hasWaiting, &hasError, &motionEnabled;
    ii::core::Observable<std::string>& title, &description, &waiting, &error,
        &otherPlaceholder, &notesPlaceholder, &declineText, &skipText, &sendText;
    void submit() { model->submit(); }
    void skip() { model->skip(); }
    void decline() { model->decline(); }
};
} // namespace iiMultimodalAgent::agent::presentation

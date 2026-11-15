#include "NativeQuestionViewModel.h"
#include <iiSDKCore/Core/Text/Utf8.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace iiMultimodalAgent::agent::presentation {
namespace {
// UTF-8 encodings of the Unicode White_Space characters used by QString::trimmed.
std::size_t spaceBytes(std::string_view text, std::size_t at) {
    const auto c = static_cast<unsigned char>(text[at]);
    if (c == ' ' || (c >= '\t' && c <= '\r')) return 1;
    if (at + 1 < text.size() && c == 0xc2 &&
        (static_cast<unsigned char>(text[at + 1]) == 0x85 || static_cast<unsigned char>(text[at + 1]) == 0xa0)) return 2;
    if (at + 2 >= text.size()) return 0;
    const auto b = static_cast<unsigned char>(text[at + 1]);
    const auto d = static_cast<unsigned char>(text[at + 2]);
    if ((c == 0xe1 && b == 0x9a && d == 0x80) ||
        (c == 0xe2 && b == 0x80 && ((d >= 0x80 && d <= 0x8a) || d == 0xa8 || d == 0xa9 || d == 0xaf)) ||
        (c == 0xe2 && b == 0x81 && d == 0x9f) || (c == 0xe3 && b == 0x80 && d == 0x80)) return 3;
    return 0;
}
std::string trim(std::string_view value) {
    std::size_t first = value.size(), last = 0;
    for (std::size_t i = 0; i < value.size();) {
        if (auto size = spaceBytes(value, i)) { i += size; continue; }
        const auto c = static_cast<unsigned char>(value[i]);
        std::size_t width = c < 0x80 ? 1 : (c < 0xe0 ? 2 : (c < 0xf0 ? 3 : 4));
        width = std::min(width, value.size() - i);
        if (first == value.size()) first = i;
        i += width;
        last = i;
    }
    return first == value.size() ? std::string{} : std::string(value.substr(first, last - first));
}
void checkText(const std::string& text, std::size_t limit, bool required = false) {
    if (text.size() > limit || text.find('\0') != std::string::npos || !ii::core::valid_utf8(text) ||
        (required && trim(text).empty()))
        throw std::invalid_argument("Invalid user question text");
}
void validateRequest(const QuestionRequest& request) {
    checkText(request.id, 2048, true);
    if (request.questions.empty() || request.questions.size() > 4)
        throw std::invalid_argument("Invalid user question request");
    std::set<std::string> questions;
    for (const auto& question : request.questions) {
        checkText(question.text, 32768, true); checkText(question.header, 512, true);
        if (!questions.insert(question.text).second || question.options.size() < 2 || question.options.size() > 4)
            throw std::invalid_argument("Invalid user question options");
        std::set<std::string> labels;
        for (const auto& option : question.options) {
            checkText(option.label, 2048, true); checkText(option.description, 32768); checkText(option.preview, 131072);
            if (!labels.insert(option.label).second) throw std::invalid_argument("Duplicate user question option");
        }
    }
}
void validate(const QuestionUpdate& value) {
    if (value.requests.size() > 64) throw std::invalid_argument("Too many user question requests");
    checkText(value.error, 131072);
    std::set<std::string> requestIds;
    for (const auto& request : value.requests) {
        validateRequest(request);
        if (!requestIds.insert(request.id).second) throw std::invalid_argument("Duplicate user question request");
    }
}
std::string insertText(std::string text, std::string_view replacement) {
    for (std::size_t at = 0; (at = text.find("%1", at)) != std::string::npos; at += replacement.size())
        text.replace(at, 2, replacement);
    return text;
}
std::string countText(std::string text, std::size_t count) { return insertText(std::move(text), std::to_string(count)); }
} // namespace

QuestionOptionRow::QuestionOptionRow(std::string idValue, QuestionOption option,
                                     std::weak_ptr<QuestionRow> owner, std::size_t index)
    : id(std::move(idValue)), label(std::move(option.label)), description(std::move(option.description)),
      preview(std::move(option.preview)), owner_(std::move(owner)), index_(index) {}
void QuestionOptionRow::toggled(bool value) { if (auto row = owner_.lock()) row->select(index_, value); }
void QuestionOptionRow::clicked() {
    if (auto row = owner_.lock(); row && row->active()) row->select(index_, row->multiple ? !checked.get() : true);
}
void QuestionOptionRow::showPreview() { if (auto row = owner_.lock()) row->showPreview(index_); }

QuestionRow::QuestionRow(std::string idValue, const Question& question)
    : id(std::move(idValue)), text(question.text), header(question.header), multiple(question.multiple) {}
std::shared_ptr<QuestionRow> QuestionRow::make(std::string id, const Question& question) {
    auto result = std::shared_ptr<QuestionRow>(new QuestionRow(std::move(id), question));
    result->options.reserve(question.options.size());
    for (std::size_t i = 0; i < question.options.size(); ++i)
        result->options.push_back(std::shared_ptr<QuestionOptionRow>(
            new QuestionOptionRow(std::to_string(i), question.options[i], result, i)));
    return result;
}
bool QuestionRow::active() const { (void)other.revision(); return active_; }
void QuestionRow::retire() { (void)other.revision(); active_ = false; }
void QuestionRow::editOther(std::string value) {
    (void)other.revision();
    if (!active_) return;
    checkText(value, 131072);
    if (!multiple && !trim(value).empty()) for (const auto& option : options) {
        option->checked.set(false); if (!active_) return;
    }
    other.set(std::move(value));
}
void QuestionRow::editNotes(std::string value) {
    (void)notes.revision();
    if (!active_) return;
    checkText(value, 131072);
    notes.set(std::move(value));
}
void QuestionRow::select(std::size_t index, bool value) {
    (void)other.revision();
    if (!active_ || index >= options.size()) return;
    if (!multiple) {
        if (!value) return; // A radio choice is replaced, never toggled off.
        for (std::size_t i = 0; i < options.size(); ++i) { options[i]->checked.set(i == index); if (!active_) return; }
        other.set(""); if (!active_) return;
    } else { options[index]->checked.set(value); if (!active_) return; }
    preview.set(options[index]->preview);
}
void QuestionRow::showPreview(std::size_t index) {
    (void)other.revision();
    if (active_ && index < options.size()) preview.set(options[index]->preview);
}
void QuestionRow::localize(const QuestionStrings& strings) {
    if (!active()) return;
    otherAccessibility.set(insertText(strings.otherAccessibility, text));
    if (active()) notesAccessibility.set(insertText(strings.notesAccessibility, text));
}

struct NativeQuestionViewModel::Lifetime final { NativeQuestionViewModel* owner = nullptr; std::uint64_t stringsRevision=0; };

NativeQuestionViewModel::NativeQuestionViewModel()
    : lifetime_(std::make_shared<Lifetime>()), update([this](const QuestionUpdate& value) { return apply(value); }),
      change([this](const QuestionDelta& value) { return applyChange(value); }),
      setError([this](const QuestionError& value) { return applyError(value); }),
      read([this](const ReadQuestions&) { (void)title.revision(); return snapshot_; }),
      submitQuery([this](const QuestionAction&) { return sendResponse(false); }),
      skipQuery([this](const QuestionAction&) { return sendResponse(true); }),
      declineQuery([this](const QuestionAction&) { return sendDecline(); }) { lifetime_->owner=this; refreshStrings(); }
NativeQuestionViewModel::~NativeQuestionViewModel() {
    lifetime_->owner=nullptr;
    (void)update.close(); (void)change.close(); (void)setError.close(); (void)read.close(); (void)submitQuery.close();
    (void)skipQuery.close(); (void)declineQuery.close();
    for (const auto& row : rows.get()) row->retire();
}
void NativeQuestionViewModel::setStrings(QuestionStrings strings) {
    (void)title.revision();
    if(lifetime_->stringsRevision==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("Question strings revision exhausted");
    ++lifetime_->stringsRevision;strings_ = std::move(strings); refreshStrings();
}
void NativeQuestionViewModel::setActions(std::function<bool(const QuestionResponse&)> submit,
                                        std::function<bool(std::string_view)> declineAction) {
    (void)title.revision(); submitAction_ = std::move(submit); declineAction_ = std::move(declineAction);
}
void NativeQuestionViewModel::refreshStrings() {
    const auto life=lifetime_;const auto revision=life->stringsRevision;const auto strings=strings_;const auto count=snapshot_.requests.size();
    const auto current=[&]{return life->owner==this&&life->stringsRevision==revision;};
    const auto activeRows = rows.get();
    title.set(strings.title);if(!current())return;
    description.set(strings.description);if(!current())return;
    waiting.set(countText(strings.waiting,count));if(!current())return;
    otherPlaceholder.set(strings.other);if(!current())return;
    notesPlaceholder.set(strings.notes);if(!current())return;
    declineText.set(strings.decline);if(!current())return;
    skipText.set(strings.skip);if(!current())return;
    sendText.set(strings.send);if(!current())return;
    for (const auto& row : activeRows) {row->localize(strings);if(!current())return;}
}
bool NativeQuestionViewModel::apply(const QuestionUpdate& value) {
    (void)title.revision();
    if (updating_) throw std::logic_error("A question update is already in progress");
    validate(value);
    if (snapshot_ == value) return false;
    const bool replace = snapshot_.requests.empty() != value.requests.empty() ||
        (!value.requests.empty() && (snapshot_.requests.empty() || snapshot_.requests.front() != value.requests.front()));
    std::vector<std::shared_ptr<QuestionRow>> nextRows;
    if (replace && !value.requests.empty()) {
        const auto& questions = value.requests.front().questions;
        nextRows.reserve(questions.size());
        for (std::size_t i = 0; i < questions.size(); ++i) {
            auto row = QuestionRow::make(std::to_string(i), questions[i]);
            row->localize(strings_); nextRows.push_back(std::move(row));
        }
    }
    auto next = value;
    updating_ = true;
    struct Reset final { bool& flag; ~Reset() { flag = false; } } reset{updating_};
    if (replace) for (const auto& row : rows.get()) row->retire();
    snapshot_ = std::move(next);
    if (replace) rows.set(std::move(nextRows));
    open.set(!snapshot_.requests.empty());
    hasWaiting.set(snapshot_.requests.size() > 1);
    waiting.set(countText(strings_.waiting, snapshot_.requests.size()));
    error.set(snapshot_.error); hasError.set(!snapshot_.error.empty());
    return true;
}
bool NativeQuestionViewModel::applyChange(const QuestionDelta& value) {
    (void)title.revision();
    if (updating_) throw std::logic_error("A question update is already in progress");
    checkText(value.id, 2048, true);
    auto& requests = snapshot_.requests;
    auto found = std::find_if(requests.begin(), requests.end(), [&](const auto& entry) { return entry.id == value.id; });
    const auto oldIndex = static_cast<std::size_t>(found - requests.begin());
    const bool exists = found != requests.end();
    if (!value.value && !exists) return false;
    const auto remaining = requests.size() - (exists ? 1u : 0u);
    std::optional<QuestionRequest> incoming;
    if (value.value) {
        validateRequest(*value.value);
        if (value.value->id != value.id || value.index > remaining || remaining >= 64)
            throw std::invalid_argument("Invalid user question change");
        if (exists && oldIndex == value.index && *found == *value.value) return false;
        incoming = *value.value; // Allocate before retiring the current row.
    }

    const QuestionRequest* oldHead = requests.empty() ? nullptr : &requests.front();
    const QuestionRequest* nextHead = oldHead;
    if (incoming && value.index == 0) nextHead = &*incoming;
    else if (exists && oldIndex == 0) nextHead = requests.size() > 1 ? &requests[1] : nullptr;
    const bool replace = (oldHead == nullptr) != (nextHead == nullptr) ||
        (oldHead && nextHead && *oldHead != *nextHead);
    std::vector<std::shared_ptr<QuestionRow>> nextRows;
    if (replace && nextHead) {
        nextRows.reserve(nextHead->questions.size());
        for (std::size_t i = 0; i < nextHead->questions.size(); ++i) {
            auto row = QuestionRow::make(std::to_string(i), nextHead->questions[i]);
            row->localize(strings_); nextRows.push_back(std::move(row));
        }
    }
    if (incoming && !exists) requests.reserve(requests.size() + 1);
    updating_ = true;
    struct Reset final { bool& flag; ~Reset() { flag = false; } } reset{updating_};
    if (replace) for (const auto& row : rows.get()) row->retire();
    if (exists) requests.erase(requests.begin() + static_cast<std::ptrdiff_t>(oldIndex));
    if (incoming) requests.insert(requests.begin() + static_cast<std::ptrdiff_t>(value.index), std::move(*incoming));
    if (replace) rows.set(std::move(nextRows));
    open.set(!requests.empty());
    hasWaiting.set(requests.size() > 1);
    waiting.set(countText(strings_.waiting, requests.size()));
    return true;
}
bool NativeQuestionViewModel::applyError(const QuestionError& value) {
    (void)title.revision();
    if (updating_) throw std::logic_error("A question update is already in progress");
    checkText(value.message, 131072);
    if (snapshot_.error == value.message) return false;
    auto next = value.message;
    updating_ = true;
    struct Reset final { bool& flag; ~Reset() { flag = false; } } reset{updating_};
    snapshot_.error = std::move(next);
    error.set(snapshot_.error);
    hasError.set(!snapshot_.error.empty());
    return true;
}
bool NativeQuestionViewModel::sendResponse(bool skip) {
    (void)title.revision();
    if (snapshot_.requests.empty() || !submitAction_ || updating_) return false;
    QuestionResponse result; result.id = snapshot_.requests.front().id;
    if (!skip) for (const auto& row : rows.get()) {
        std::string labels;
        const QuestionOptionRow* chosen = nullptr;
        std::size_t selected = 0;
        for (const auto& option : row->options) if (option->checked.get()) {
            if (!labels.empty()) labels += ", ";
            labels += option->label; chosen = option.get(); ++selected;
        }
        const auto other = row->other.get();
        if (!trim(other).empty()) {
            if (!row->multiple) labels.clear();
            if (!labels.empty()) labels += ", ";
            labels += other;
        }
        if (!labels.empty()) result.answers.emplace(row->text, std::move(labels));
        QuestionAnnotation annotation;
        if (selected == 1 && trim(other).empty() && chosen) annotation.preview = chosen->preview;
        annotation.notes = trim(row->notes.get());
        if (!annotation.preview.empty() || !annotation.notes.empty()) result.annotations.emplace(row->text, std::move(annotation));
    }
    auto action = submitAction_;
    return action(result);
}
bool NativeQuestionViewModel::sendDecline() {
    (void)title.revision();
    if (snapshot_.requests.empty() || !declineAction_ || updating_) return false;
    auto action = declineAction_;
    auto id = snapshot_.requests.front().id;
    return action(id);
}
void NativeQuestionViewModel::submit() { (void)submitQuery.send({}); }
void NativeQuestionViewModel::skip() { (void)skipQuery.send({}); }
void NativeQuestionViewModel::decline() { (void)declineQuery.send({}); }

NativeQuestionBackend::NativeQuestionBackend() : NativeQuestionBackend(std::make_shared<NativeQuestionViewModel>()) {}
NativeQuestionBackend::NativeQuestionBackend(std::shared_ptr<NativeQuestionViewModel> value)
    : model(value ? std::move(value) : throw std::invalid_argument("A question model is required")),
      rows(model->rows), open(model->open), hasWaiting(model->hasWaiting), hasError(model->hasError), motionEnabled(model->motionEnabled),
      title(model->title), description(model->description), waiting(model->waiting), error(model->error),
      otherPlaceholder(model->otherPlaceholder), notesPlaceholder(model->notesPlaceholder),
      declineText(model->declineText), skipText(model->skipText), sendText(model->sendText) {}
} // namespace iiMultimodalAgent::agent::presentation

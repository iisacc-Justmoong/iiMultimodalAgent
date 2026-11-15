#include <iiMultimodalAgent/agent/NativeQuestionViewModel.h>

#include <stdexcept>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace iiMultimodalAgent::agent::presentation;
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

static QuestionRequest request(std::string id, bool multiple = false) {
    return {std::move(id), {{"__proto__", "Renderer", multiple,
        {{"Qt", "Native", "<b>Native</b>"}, {"Web", "Browser", "**Browser**"}}}}};
}

int main() {
    NativeQuestionViewModel model;
    std::vector<QuestionResponse> responses;
    std::vector<std::string> declined;
    model.setActions([&](const QuestionResponse& result) { responses.push_back(result); return true; },
                     [&](std::string_view id) { declined.emplace_back(id); return true; });
    CHECK(model.update.send({{request("first", true)}, ""}));
    CHECK(model.open.get() && model.rows.get().size() == 1);
    auto first = model.rows.get().at(0);
    CHECK(first->otherAccessibility.get() == "Your answer to __proto__");
    CHECK(first->notesAccessibility.get() == "Notes for __proto__");
    QuestionStrings translated;
    translated.otherAccessibility = "%1에 대한 답변";
    translated.notesAccessibility = "%1 메모";
    model.setStrings(translated);
    CHECK(first->otherAccessibility.get() == "__proto__에 대한 답변");
    CHECK(first->notesAccessibility.get() == "__proto__ 메모");
    first->options.at(1)->clicked();
    CHECK(first->options.at(1)->checked.get());
    first->options.at(1)->clicked();
    CHECK(!first->options.at(1)->checked.get());
    first->options.at(1)->clicked();
    first->options.at(0)->toggled(true);
    first->editOther("직접 입력\n두 번째 줄");
    first->editNotes("  검토 메모\n두 번째 줄  ");
    const auto rowsRevision = model.rows.revision();
    auto inserted = model.change.send({"second", request("second"), 1});
    CHECK(inserted && *inserted);
    CHECK(model.rows.get().at(0) == first && model.rows.revision() == rowsRevision);
    CHECK(first->other.get() == "직접 입력\n두 번째 줄" && model.hasWaiting.get());
    auto duplicate = model.change.send({"second", request("second"), 1});
    CHECK(duplicate && !*duplicate && model.rows.revision() == rowsRevision);
    auto errorChange = model.setError.send({"Temporarily unavailable"});
    CHECK(errorChange && *errorChange && model.hasError.get());
    CHECK(model.rows.get().at(0) == first && model.rows.revision() == rowsRevision);
    CHECK(model.setError.send({""}) && !model.hasError.get());
    CHECK(!model.change.send({"third", request("third"), 9}));
    auto invalidUtf8 = request("invalid");
    invalidUtf8.questions.front().text = std::string("\xff", 1);
    CHECK(!model.change.send({"invalid", invalidUtf8, 2}));
    CHECK(model.rows.get().front() == first && model.read.send({})->requests.size() == 2);
    CHECK(model.read.send({})->requests.size() == 2);
    model.submit();
    CHECK(responses.size() == 1 && responses[0].id == "first");
    CHECK(responses[0].answers.at("__proto__") == "Qt, Web, 직접 입력\n두 번째 줄");
    CHECK(responses[0].annotations.at("__proto__").notes == "검토 메모\n두 번째 줄");
    CHECK(responses[0].annotations.at("__proto__").preview.empty());

    auto removed = model.change.send({"first", std::nullopt, 0});
    CHECK(removed && *removed);
    auto second = model.rows.get().at(0);
    CHECK(second != first && !first->active());
    first->options.at(0)->toggled(false);
    first->options.at(1)->clicked();
    first->editOther("stale");
    CHECK(second->other.get().empty() && !second->options.at(0)->checked.get());
    second->options.at(0)->clicked();
    CHECK(second->preview.get() == "<b>Native</b>");
    model.submit();
    CHECK(responses.back().annotations.at("__proto__").preview == "<b>Native</b>");
    second->editOther("Own answer");
    CHECK(!second->options.at(0)->checked.get());
    model.submit();
    CHECK(responses.back().answers.at("__proto__") == "Own answer");
    CHECK(responses.back().annotations.empty());
    model.skip();
    CHECK(responses.back().id == "second" && responses.back().answers.empty());
    model.decline();
    CHECK(declined.back() == "second");

    auto changed = request("second");
    changed.questions[0].text = "Changed identity";
    CHECK(model.update.send({{changed}, "Host error"}));
    CHECK(!second->active() && model.error.get() == "Host error");
    CHECK(model.rows.get().at(0)->other.get().empty());
    CHECK(model.update.send({{}, ""}));
    CHECK(!model.open.get() && model.rows.get().empty());
    const auto count = responses.size();
    model.submit(); model.skip(); model.decline();
    CHECK(responses.size() == count && declined.size() == 1);

    bool rejected = false;
    std::thread worker([&] { rejected = !model.update.send({{request("foreign")}, ""}); });
    worker.join();
    CHECK(rejected && !model.open.get());

    auto retiring=std::make_unique<NativeQuestionViewModel>();
    auto retirement=retiring->title.observe([&](const auto&,std::uint64_t){retiring.reset();});
    QuestionStrings replacement;replacement.title="Changed title";
    retiring->setStrings(std::move(replacement));CHECK(!retiring);

    NativeQuestionViewModel nestedStrings;
    QuestionStrings earlier,later;earlier.title="Earlier";earlier.description="Earlier description";
    later.title="Later";later.description="Later description";
    auto nestedChange=nestedStrings.title.observe([&](const std::string& value,std::uint64_t){
        if(value=="Earlier")nestedStrings.setStrings(later);
    });
    nestedStrings.setStrings(earlier);
    CHECK(nestedStrings.title.get()=="Later"&&nestedStrings.description.get()=="Later description");

    NativeQuestionViewModel reentrant;
    CHECK(reentrant.update.send({{request("reentrant")}, ""}));
    auto oldRow=reentrant.rows.get().front();
    auto retireOnSelection=oldRow->options.front()->checked.observe([&](bool checked,std::uint64_t){
        if(checked)CHECK(reentrant.update.send({{}, ""}));
    });
    oldRow->options.front()->toggled(true);
    CHECK(!oldRow->active()&&oldRow->preview.get().empty());

    NativeQuestionViewModel reentrantText;
    CHECK(reentrantText.update.send({{request("text")}, ""}));
    auto oldTextRow=reentrantText.rows.get().front();oldTextRow->options.front()->toggled(true);
    auto retireOnClear=oldTextRow->options.front()->checked.observe([&](bool checked,std::uint64_t){
        if(!checked)CHECK(reentrantText.update.send({{}, ""}));
    });
    oldTextRow->editOther("late answer");
    CHECK(!oldTextRow->active()&&oldTextRow->other.get().empty());
}

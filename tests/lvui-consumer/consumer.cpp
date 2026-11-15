#include "UserQuestionsSheet.generated.h"
#include <iiMultimodalAgent/agent/NativeQuestionPresentationC.h>
#include <iiGUI/PaintView.h>
#include <iiGUI/WidgetState.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T take(ii::core::Result<T> value) {
    if (!value) {
        if (value.error().cause) std::rethrow_exception(value.error().cause);
        throw std::runtime_error(value.error().message);
    }
    return std::move(*value);
}
void good(ii::core::Result<void> value) {
    if (!value) {
        if (value.error().cause) std::rethrow_exception(value.error().cause);
        throw std::runtime_error(value.error().message);
    }
}
ii::gui::Widget find(const std::shared_ptr<ii::gui::detail::WidgetState>& tree, std::string_view suffix) {
    const auto nodes = take(tree->snapshots());
    const auto found = std::ranges::find_if(nodes, [suffix](const auto& node) { return node.id.ends_with(suffix); });
    if (found == nodes.end()) throw std::runtime_error("Missing generated question widget: " + std::string(suffix));
    return take(tree->find(found->id));
}
std::vector<ii::gui::Widget> findAll(const std::shared_ptr<ii::gui::detail::WidgetState>& tree, std::string_view suffix) {
    std::vector<ii::gui::Widget> result;
    for (const auto& node : take(tree->snapshots()))
        if (node.id.ends_with(suffix)) result.push_back(take(tree->find(node.id)));
    return result;
}
int cAction(void* context,int kind,const char* id,const iiquestion_response_item* items,std::size_t count){
    auto& output=*static_cast<std::string*>(context);
    if(kind==IIQUESTION_ACTION_SUBMIT&&count==1&&items[0].answer)output=std::string(id)+":"+items[0].answer;
    return 1;
}
struct ExternalPaintDevice final : ii::gui::FramePaintDevice {
    int frames=0;
    ii::gui::PaintCapabilities capabilities()const noexcept override {
        return {.api=ii::gui::GraphicsApi::vulkan,.hardware=true,.native_api=true,
                .backdrop=true,.shadow=true,.content_transform=true,.scene_sources=true};
    }
    ii::core::Result<void> paint_frame(const ii::gui::CanvasFrame&,const ii::gui::PaintFrameInfo&,
                                      std::stop_token)override {++frames;return {};}
};
}
int main() {
    using View = lvui_generated::UserQuestionsSheet;
    using namespace iiMultimodalAgent::agent::presentation;
    auto model = std::make_shared<NativeQuestionViewModel>();
    model->motionEnabled.set(false);
    QuestionResponse received;
    int sends = 0;
    std::vector<std::string> declined;
    model->setActions([&](const QuestionResponse& response) { received = response; ++sends; return true; },
                      [&](std::string_view id) { declined.emplace_back(id); return true; });
    const auto factories = lvui::native::BackendFactories{}.with<NativeQuestionBackend>([model] {
        return NativeQuestionBackend(model);
    });
    auto tree = std::make_shared<ii::gui::detail::WidgetState>([](ii::runtime::Task task) { task(); });
    auto root = take(tree->content_host({}, "host", {800, 700}));
    std::shared_ptr<View::Scope> scope;
    good(root.set_content([&](ii::gui::WidgetBuilder& builder) {
        return View::build(builder, {}, {}, &scope, factories);
    }));
    QuestionRequest request{"first", {{"Which renderer?", "Renderer", false,
        {{"Qt", "Native", "<b>Native</b>"}, {"Web", "Browser", "**Browser**"}}}}};
    request.questions.front().options.front().description = std::string(240, 'M');
    check(bool(model->update.send({{request}, ""})), "Question snapshot was rejected");
    (void)take(tree->drawing_snapshot({800, 700}));
    const auto initialSheet=take(find(tree, "/agentQuestionsSheet").snapshot());
    check(initialSheet.opened, "The question sheet did not open");
    if(!ii::gui::sheet_mobile_platform())
        check(std::abs(initialSheet.bounds.x*2+initialSheet.bounds.width-800)<.1 &&
              std::abs(initialSheet.bounds.y*2+initialSheet.bounds.height-700)<.1,
              "The desktop question sheet was not centered");
    const auto description = take(find(tree, "/optionDescription").snapshot());
    check(!description.text_input && description.bounds.height > 46,
          "A long option description did not wrap as content-height text");
    auto choices = findAll(tree, "/choice");
    check(choices.size() == 2, "Single-choice controls were not generated");
    check(take(choices.front().snapshot()).accessibility_name == "Qt", "Option lost its accessible label");
    check(take(find(tree, "/other").snapshot()).accessibility_name == "Your answer to Which renderer?" &&
          take(find(tree, "/notes").snapshot()).accessibility_name == "Notes for Which renderer?",
          "Question editors lost their distinct accessible names");
    good(choices.back().focus());
    check(model->rows.get().front()->preview.get() == "**Browser**",
          "Focusing an option did not display its preview");
    good(tree->input({.kind=ii::gui::WidgetInputKind::pointer_move,
                      .x=description.bounds.x+4, .y=description.bounds.y+4}));
    check(model->rows.get().front()->preview.get() == "<b>Native</b>",
          "Hovering an option description did not display its preview");
    good(tree->input({.kind=ii::gui::WidgetInputKind::pointer_down,
                      .x=description.bounds.x+4, .y=description.bounds.y+4, .buttons=1}));
    good(tree->input({.kind=ii::gui::WidgetInputKind::pointer_up,
                      .x=description.bounds.x+4, .y=description.bounds.y+4}));
    check(model->rows.get().front()->options.front()->checked.get() &&
          model->rows.get().front()->preview.get() == "<b>Native</b>",
          "Clicking an option description did not update its model and preview");
    good(choices.back().activate());
    check(model->rows.get().front()->options.back()->checked.get() &&
          !model->rows.get().front()->options.front()->checked.get(), "Radio selection was not exclusive");
    good(choices.front().activate());
    auto field = find(tree, "/other");
    good(field.focus());
    good(tree->input({.kind=ii::gui::WidgetInputKind::text, .text="Own answer"}));
    check(model->rows.get().front()->other.get() == "Own answer" &&
          !model->rows.get().front()->options.front()->checked.get(),
          "Editor did not update the model or clear the single selection");
    good(find(tree, "/submit").activate());
    check(sends == 1 && received.id == "first" && received.answers.at("Which renderer?") == "Own answer",
          "Submit did not deliver the edited answer");
    auto retained = model->rows.get().front();
    QuestionRequest second{"second", request.questions};
    check(bool(model->change.send({"second", second, 1})), "A new request was rejected");
    check(model->rows.get().front() == retained && retained->other.get() == "Own answer",
          "A queued request replaced the active editor");
    check(bool(model->change.send({"first", std::nullopt, 0})), "Removing the first request failed");
    check(!retained->active() && model->rows.get().front()->other.get().empty(),
          "The next request inherited the previous draft");
    check(bool(model->update.send({{}, ""})) && declined.empty(),
          "Programmatic removal rejected the previous request");
    QuestionRequest third{"third", request.questions};
    third.questions.front().multiple = true;
    check(bool(model->update.send({{third}, ""})), "The next request did not open");
    const auto multiDescription = take(find(tree, "/optionDescription").snapshot()).bounds;
    good(tree->input({.kind=ii::gui::WidgetInputKind::pointer_down,
                      .x=multiDescription.x+4, .y=multiDescription.y+4, .buttons=1}));
    good(tree->input({.kind=ii::gui::WidgetInputKind::pointer_up,
                      .x=multiDescription.x+4, .y=multiDescription.y+4}));
    check(model->rows.get().front()->options.front()->checked.get(), "Multiple-choice description did not select its option");
    good(find(tree, "/option").activate());
    check(!model->rows.get().front()->options.front()->checked.get(), "Checkbox did not toggle the option off");
    good(tree->input({.kind=ii::gui::WidgetInputKind::key_down, .key=ii::gui::WidgetKey::escape}));
    check(declined.size() == 1 && declined.front() == "third", "Escape did not reject only the active request");
    check(bool(model->update.send({{}, ""})) && !model->open.get(),
          "The fake inbox did not remove the declined request");

    QuestionRequest lengthy{"lengthy", {}};
    for (int q=0;q<4;++q) {
        Question question{"Question " + std::to_string(q), "Details", true, {}};
        for (int o=0;o<4;++o)
            question.options.push_back({"Choice " + std::to_string(o), std::string(180, 'a'+o), ""});
        lengthy.questions.push_back(std::move(question));
    }
    check(bool(model->update.send({{lengthy}, ""})), "The long question request was rejected");
    good(root.set_extent({360,260}));
    (void)take(tree->drawing_snapshot({360, 260}));
    auto sheet=take(root.content_item("agentQuestionsSheet"));
    auto viewport = take(sheet.popup_content("agentQuestionsSheet.viewport"));
    auto bounds = take(viewport.snapshot());
    const auto sheetBounds=take(sheet.snapshot()).bounds;
    if(sheetBounds.x<0 || sheetBounds.y<0 || sheetBounds.x+sheetBounds.width>360 || sheetBounds.y+sheetBounds.height>260)
        throw std::runtime_error("Question sheet did not follow the resized window: "+
            std::to_string(sheetBounds.x)+":"+std::to_string(sheetBounds.y)+":"+
            std::to_string(sheetBounds.width)+":"+std::to_string(sheetBounds.height));
    if(bounds.bounds.height<=0 || bounds.content_height<=bounds.bounds.height) {
        std::string detail;
        for(const auto& node:take(tree->snapshots()))
            if(node.id.ends_with("/questions") || node.id.ends_with("/questionText") ||
               node.id.ends_with("/agentQuestionsSheet.body") || node.id.ends_with("/agentQuestionsSheet") || node.id=="host")
                detail += " " + node.id.substr(node.id.rfind('/')==node.id.npos?0:node.id.rfind('/')) +
                          "=" + std::to_string(node.bounds.y) + ":" + std::to_string(node.bounds.height);
        throw std::runtime_error("Long questions did not create a usable scroll viewport: height="+
            std::to_string(bounds.bounds.height)+" content="+std::to_string(bounds.content_height)+detail);
    }
    auto firstOption = model->rows.get().front()->options.front();
    firstOption->clicked();
    bounds=take(viewport.snapshot());
    good(tree->input({.kind=ii::gui::WidgetInputKind::scroll,
                      .x=bounds.bounds.x+20,.y=bounds.bounds.y+20,.scroll_y=80}));
    auto scrolled=take(viewport.snapshot());
    if(scrolled.scroll_offset<=0)
        throw std::runtime_error("User scroll did not move the question sheet: viewport="+
            std::to_string(bounds.bounds.x)+":"+std::to_string(bounds.bounds.y)+":"+
            std::to_string(bounds.bounds.width)+":"+std::to_string(bounds.bounds.height)+
            " content="+std::to_string(bounds.content_height)+" sheet="+
            std::to_string(take(find(tree,"/agentQuestionsSheet").snapshot()).bounds.y));
    good(viewport.set_scroll_offset(bounds.content_height));
    auto end=take(viewport.snapshot());auto send=take(find(tree,"/submit").snapshot());
    check(send.visible && send.bounds.y>=end.bounds.y && send.bounds.y+send.bounds.height<=end.bounds.y+end.bounds.height+.01,
          "The final action is not reachable in a short window");
    check(firstOption->checked.get(), "Scrolling discarded a previous answer");
    good(find(tree,"/submit").activate());
    check(sends==2 && received.id=="lengthy" && received.answers.at("Question 0")=="Choice 0",
          "Submitting after scrolling lost the selected answer");
    good(root.clear_content());
    scope.reset();
    tree->close();

    // Exercise the Python-accessible C model as the backend of the actual
    // generated LVUI, with an explicit lease spanning the mounted view.
    auto* cModel=iiquestion_create();check(cModel,"Cannot create C question model");
    check(!iiquestion_set_motion(cModel,0),"Cannot disable headless question motion");
    iiquestion_model_lease* lease=nullptr;void* pointer=nullptr;
    check(!iiquestion_acquire_model(cModel,&lease)&&!iiquestion_lease_model(lease,&pointer)&&pointer,
          "Cannot lease C question model");
    auto leased=std::shared_ptr<NativeQuestionViewModel>(static_cast<NativeQuestionViewModel*>(pointer),[lease](NativeQuestionViewModel*){
        (void)iiquestion_release_model(lease);
    });
    std::string cResponse;check(!iiquestion_set_actions(cModel,cAction,&cResponse),"Cannot attach C question actions");
    const auto cFactories=lvui::native::BackendFactories{}.with<NativeQuestionBackend>([leased]{return NativeQuestionBackend(leased);});
    auto cTree=std::make_shared<ii::gui::detail::WidgetState>([](ii::runtime::Task task){task();});
    auto cRoot=take(cTree->content_host({},"host",{800,700}));
    good(cRoot.set_content([&](auto& builder){return View::build(builder,{}, {},nullptr,cFactories);}));
    const iiquestion_option_data cOptions[]={{"Native","Direct","Preview"},{"Web","Browser","Other"}};
    const iiquestion_question_data cQuestions[]={{"Renderer?","Renderer",0,cOptions,2}};
    const iiquestion_request_data cRequests[]={{"first",cQuestions,1}};
    int changed=0;check(!iiquestion_update(cModel,cRequests,1,"",&changed)&&changed,"C question snapshot failed");
    auto cChoices=findAll(cTree,"/choice");check(cChoices.size()==2,"C question state did not create LVUI choices");
    good(cChoices.front().activate());good(find(cTree,"/submit").activate());
    check(cResponse=="first:Native","LVUI did not send the C question response");
    good(cRoot.clear_content());cTree->close();
    check(!iiquestion_destroy(cModel)&&leased->open.get(),"A lease did not retain the detached question model");

    auto externalModel=std::make_shared<NativeQuestionViewModel>();externalModel->motionEnabled.set(false);
    std::string externalResponse;
    externalModel->setActions([&](const QuestionResponse& response){externalResponse=response.id;return true;},
                              [](std::string_view){return true;});
    auto externalFactories=lvui::native::BackendFactories{}.with<NativeQuestionBackend>(
        [externalModel]{return NativeQuestionBackend(externalModel);});
    auto device=std::make_shared<ExternalPaintDevice>();std::shared_ptr<View::Scope> externalScope;
    auto external=take(ii::gui::PaintView::create(device,[&](auto& builder){
        return View::build(builder,{}, {},&externalScope,externalFactories);
    }));
    check(device->frames==0&&take(external->needs_paint()),"External question view painted before a request");
    QuestionRequest externalRequest{"external",{{"Continue?","Question",false,{{"Yes","",""},{"No","",""}}}}};
    auto externalUpdate=externalModel->update.send({{externalRequest},""});
    if(!externalUpdate&&externalUpdate.error().cause)std::rethrow_exception(externalUpdate.error().cause);
    check(externalUpdate&&*externalUpdate,"External question snapshot failed");
    good(external->paint({800,700}));
    auto externalSheet=take(external->find("agentQuestionsSheet"));auto skip=take(externalSheet.popup_content("skip"));
    auto id=take(skip.snapshot()).id;
    good(external->input({.kind=ii::gui::WidgetInputKind::activate,.target=id}));
    check(externalResponse=="external"&&device->frames==1,"External question input or on-demand paint failed");
    std::weak_ptr<View::Scope> externalRetired=externalScope;
    good(external->close());externalScope.reset();
    check(externalRetired.expired(),"External question view retained its model");
}

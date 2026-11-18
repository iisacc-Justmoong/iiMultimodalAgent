#include <iiMultimodalAgent/agent/Engine.h>
#include <iiMultimodalAgent/agent/PermissionRequests.h>
#include <iiMultimodalAgent/agent/McpServer.h>
#include <QtCore/QTemporaryDir>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QCoreApplication>
#include <future>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
QJsonObject question() {
    return {{"questions",QJsonArray{QJsonObject{{"question","Which renderer?"},{"header","Renderer"},
        {"options",QJsonArray{QJsonObject{{"label","Qt"},{"description","Native UI"},{"preview","**Qt**"}},
            QJsonObject{{"label","Web"},{"description","Browser UI"}}}},{"multiSelect",false}}}}};
}
class Model final:public a::Model {
public:
    int turn=0;QList<a::ModelRequest> requests;
    std::function<a::ModelReply(const a::ModelRequest&)> reply;
    a::ModelReply generate(const a::ModelRequest& r,const CancellationToken&,const TextCallback&)override {
        if(reply){requests.append(r);++turn;return reply(r);}
        requests.append(r);if(++turn==1)return {{},{{"question","AskUserQuestion",question()}}};
        return {r.messages.last().text,{}};
    }
};
struct Fixture {
    QTemporaryDir root;std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<const a::PermissionPolicy> policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    a::ToolRunnerOptions options;a::ToolContext context{"owner","run",root.path(),root.filePath("artifacts")};
    explicit Fixture(QString format="markdown") {registry->add(a::userQuestionTool({false,format}));}
    a::ToolResult run(QJsonObject args=question(),a::EventCallback event={}) {
        return a::ToolRunner(registry,policy,options).run({"call","AskUserQuestion",args},context,event);
    }
    void answer(QJsonObject answers={{"Which renderer?","Qt"}},QJsonObject annotations={}) {
        options.permissionResponse=[=](const auto& call,const auto&,const auto&) {
            auto args=call.arguments;args["answers"]=answers;if(!annotations.isEmpty())args["annotations"]=annotations;
            return a::PermissionResponse{a::PermissionBehavior::Allow,{},args};
        };
    }
};
}
class UserQuestionsTests final:public QObject {
public:
    void deferredQuestionsAreDiscoveredAndResumeWithTheAnswer() {
        QTemporaryDir root;auto model=std::make_shared<Model>();a::EngineOptions config{.decision={.enabled=false}};config.sessionsDirectory=root.filePath("sessions");
        config.userQuestionsEnabled=true;config.projectContext.enabled=false;config.compaction.automatic=false;config.skills.enabled=false;
        config.permissionResponse=[](const auto& call,const auto&,const auto&){auto args=call.arguments;args["answers"]=QJsonObject{{"Which renderer?","discovered"}};return a::PermissionResponse{a::PermissionBehavior::Allow,{},args};};
        model->reply=[raw=model.get()](const auto& r)->a::ModelReply {
            if(raw->turn==1)return {{},{{"search","ToolSearch",{{"query","select:AskUserQuestion"}}}}};
            if(raw->turn==2)return {{},{{"ask","AskUserQuestion",question()}}};return {r.messages.last().text,{}};
        };
        a::Engine engine(model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),config);
        auto id=engine.createSession("fixture",root.path()).id;const auto result=engine.run({id,"Clarify UI requirements"}).result.get();
        CTEST_COMPARE(result.status,a::RunStatus::Completed);CTEST_CHECK(result.text.contains("discovered"));
        auto has=[](const auto& r){return std::any_of(r.tools.begin(),r.tools.end(),[](const auto& t){return t.name=="AskUserQuestion";});};
        CTEST_CHECK(!has(model->requests.first()));CTEST_CHECK(has(model->requests[1]));
        const auto fork=engine.forkSession(id);CTEST_CHECK(std::any_of(fork.messages.begin(),fork.messages.end(),[](const auto& m){return m.role==a::MessageRole::Tool&&m.text.contains("discovered");}));
    }
    void missingResponseAndDontAskNeverInventAnAnswer() {
        Fixture f;CTEST_CHECK(f.run().isError);f.options.permission=[](const auto&,const auto&,const auto&){return true;};
        auto skipped=f.run();CTEST_CHECK(!skipped.isError);CTEST_CHECK(skipped.data["answers"].toObject().isEmpty());
        f.policy=std::make_shared<a::RulePolicy>(a::PermissionMode::DontAsk);int called=0;
        f.options.permission=[&](const auto&,const auto&,const auto&){++called;return true;};
        CTEST_CHECK(f.run().isError);CTEST_COMPARE(called,0);
        f.policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"AskUserQuestion",a::PermissionBehavior::Deny}});
        CTEST_CHECK(f.run().isError);CTEST_COMPARE(called,0);
    }
    void questionsOptionsFreeTextAndAnnotationsRoundTrip() {
        Fixture f;auto args=question();auto list=args["questions"].toArray();auto q=list.first().toObject();
        q["question"]="Which features?";q["multiSelect"]=true;list.append(q);q["question"]="Any constraints?";list.append(q);args["questions"]=list;
        args["metadata"]=QJsonObject{{"source","society.settings"}};
        const QJsonObject answers{{"Which renderer?","A custom renderer"},{"Which features?","Qt, Web"}};
        const QJsonObject annotations{{"Which renderer?",QJsonObject{{"preview","**Custom**"},{"notes","한국어 메모"}}}};
        f.answer(answers,annotations);QJsonObject preview;int requests=0;
        auto result=f.run(args,[&](const auto& e){if(e.kind==a::EventKind::PermissionRequested){++requests;preview=e.data["permission_preview"].toObject()["_meta"].toObject()["user_question"].toObject();}});
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(requests,1);CTEST_COMPARE(result.data["questions"].toArray(),list);
        CTEST_COMPARE(result.data["answers"],answers);CTEST_COMPARE(result.data["annotations"],annotations);
        CTEST_COMPARE(preview["input"],args);CTEST_COMPARE(preview["preview_format"],"markdown");CTEST_CHECK(preview["partial_answers"].toBool());
        CTEST_CHECK(!result.data.contains("metadata"));CTEST_CHECK(result.text.contains("한국어 메모"));
    }
    static QList<std::pair<QString, QJsonObject>> malformedQuestionsData() {
        QList<std::pair<QString, QJsonObject>> rows; auto base=question();
        auto add=[&](const char* name,QJsonObject value){rows.append(std::make_pair(QString::fromUtf8(name), std::move(value)));};
        auto v=base;v["answers"]=QJsonObject{{"Which renderer?","forged"}};add("model-answers",v);
        v=base;v["annotations"]=QJsonObject{};add("model-annotations",v);
        v=base;v["session_id"]="foreign";add("wire-owner",v);
        v=base;v["questions"]=QJsonArray{};add("empty",v);
        const auto q=base["questions"].toArray().first().toObject();
        v=base;v["questions"]=QJsonArray{q,q};add("duplicate",v);
        v=base;v["questions"]=QJsonArray{q,q,q,q,q};add("too-many",v);
        auto altered=q;altered["options"]=QJsonArray{q["options"].toArray().first()};v["questions"]=QJsonArray{altered};add("one-option",v);
        altered=q;altered["options"]=QJsonArray{q["options"].toArray().first(),q["options"].toArray().first()};v["questions"]=QJsonArray{altered};add("duplicate-label",v);
        altered=q;altered["question"]="   ";v["questions"]=QJsonArray{altered};add("blank",v);
        altered=q;altered["question"]=QString("bad")+QChar::Null;v["questions"]=QJsonArray{altered};add("nul",v);
        altered=q;altered["multiSelect"]="true";v["questions"]=QJsonArray{altered};add("type",v);
        altered=q;altered["header"]=QString(129,'x');v["questions"]=QJsonArray{altered};add("header-limit",v);
        auto largeOptions=q["options"].toArray();auto option=largeOptions.first().toObject();option["preview"]=QString(32768,QChar(0x97d3));
        largeOptions[0]=option;option["label"]="second";largeOptions[1]=option;option["label"]="third";largeOptions.append(option);
        altered=q;altered["options"]=largeOptions;v["questions"]=QJsonArray{altered};add("utf8-payload-limit",v);
        return rows;
    }
    void malformedQuestionsFailBeforeReview(const QJsonObject& args) {
        Fixture f;int reviews=0;f.options.permission=[&](const auto&,const auto&,const auto&){++reviews;return true;};
        CTEST_CHECK(f.run(args).isError);CTEST_COMPARE(reviews,0);
    }
    static QStringList hostResponseChanges() {
        return {"question", "options", "metadata", "missing-questions", "unknown-answer", "unknown-annotation", "answer-type"};
    }
    void hostResponseCannotReplaceQuestionsOrInjectUnknownAnswers(const QString& change) {
        Fixture f;f.options.permissionResponse=[&](const auto& call,const auto&,const auto&) {
            auto args=call.arguments;args["answers"]=QJsonObject{{"Which renderer?","Qt"}};
            if(change=="metadata")args["metadata"]=QJsonObject{{"source","altered"}};
            else if(change=="missing-questions")args.remove("questions");
            else if(change=="unknown-answer")args["answers"]=QJsonObject{{"unasked","fake"}};
            else if(change=="unknown-annotation")args["annotations"]=QJsonObject{{"unasked",QJsonObject{{"notes","fake"}}}};
            else if(change=="answer-type")args["answers"]=QJsonObject{{"Which renderer?",QJsonArray{"Qt","Web"}}};
            else {auto q=args["questions"].toArray().first().toObject();q[change]=change=="question"?QJsonValue("Different?"):QJsonValue(QJsonArray{});args["questions"]=QJsonArray{q};}
            return a::PermissionResponse{a::PermissionBehavior::Allow,{},args};
        };
        int started=0;const auto result=f.run(question(),[&](const auto& e){if(e.kind==a::EventKind::ToolStarted)++started;});
        CTEST_CHECK(result.isError);CTEST_COMPARE(started,0);
    }
    void htmlValidationIsAnIntentCheckNotASanitizer() {
        CTEST_THROWS(Error,a::userQuestionTool({false,"xml"}));Fixture f("html");f.answer();
        auto args=question();auto q=args["questions"].toArray().first().toObject();auto opts=q["options"].toArray();auto opt=opts.first().toObject();
        for(auto preview:{"plain text","<script>x</script>","<style>p{}</style>","<!DOCTYPE html><p>x</p>","<BODY><p>x</p></BODY>"}) {
            opt["preview"]=preview;opts[0]=opt;q["options"]=opts;args["questions"]=QJsonArray{q};CTEST_CHECK(f.run(args).isError);
        }
        opt["preview"]="<div onclick='untrusted()'>Render only after host sanitization</div>";opts[0]=opt;q["options"]=opts;args["questions"]=QJsonArray{q};
        CTEST_CHECK(!f.run(args).isError);f.answer({{"Which renderer?","Qt"}},{{"Which renderer?",QJsonObject{{"preview","<script>x</script>"}}}});CTEST_CHECK(f.run(args).isError);
    }
    void beforeToolCannotForgeAnswersButPermissionRequestHookCanRespond() {
        Fixture f;f.options.hooks={[](const auto& input,const auto&) {
            a::HookResult result;if(input.kind==a::HookKind::BeforeTool){auto args=input.call.arguments;args["answers"]=QJsonObject{{"Which renderer?","fake"}};result.updatedArguments=args;}return result;
        }};CTEST_CHECK(f.run().isError);
        f.options.hooks={[](const auto& input,const auto&) {
            a::HookResult result;if(input.kind==a::HookKind::PermissionRequest){auto args=input.call.arguments;args["answers"]=QJsonObject{{"Which renderer?","host-hook"}};
                result.permissionResponse=a::PermissionResponse{a::PermissionBehavior::Allow,{},args};}return result;
        }};
        auto result=f.run();CTEST_CHECK(!result.isError);CTEST_COMPARE(result.data["answers"].toObject()["Which renderer?"],"host-hook");
        f.context.verificationAgent=true;CTEST_CHECK(f.run().isError);
    }
    void channelAnswerIsOwnedAndReplayCannotChangeIt() {
        Fixture f;auto channel=std::make_shared<a::PermissionRequests>();f.options.permissionRequests=channel;a::PermissionRequests foreign;
        QString id;QJsonObject response;bool resolved=false, ownerMatches=false, foreignRejected=false, accepted=false;
        auto result=f.run(question(),[&](const auto& e) {
            if(e.kind==a::EventKind::PermissionRequested) {
                id=e.data["request_id"].toString();ownerMatches=(e.data["session_id"]=="owner");
                auto args=question();args["answers"]=QJsonObject{{"Which renderer?","channel"}};response={{"behavior","allow"},{"updatedInput",args}};
                try { (void)foreign.respond(id,response); } catch (const Error&) { foreignRejected=true; }
                accepted=channel->respond(id,response)["accepted"].toBool();
            }
            if(e.kind==a::EventKind::PermissionResolved)resolved=true;
        });
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_CHECK(ownerMatches);CTEST_CHECK(foreignRejected);CTEST_CHECK(accepted);CTEST_CHECK(resolved);CTEST_COMPARE(result.data["answers"].toObject()["Which renderer?"],"channel");
        CTEST_CHECK(channel->pending()["requests"].toArray().isEmpty());CTEST_CHECK(channel->respond(id,response)["accepted"].toBool());
        CTEST_THROWS(Error,channel->respond(id,{{"behavior","deny"}}));
    }
    void expiredAndCancelledQuestionsCannotComplete() {
        Fixture f;a::PermissionRequestsOptions limits;limits.timeoutMs=25;auto channel=std::make_shared<a::PermissionRequests>(limits);f.options.permissionRequests=channel;
        QString id;QString status;auto observe=[&](const auto& e){if(e.kind==a::EventKind::PermissionRequested)id=e.data["request_id"].toString();if(e.kind==a::EventKind::PermissionResolved)status=e.data["status"].toString();};
        CTEST_CHECK(f.run(question(),observe).isError);CTEST_COMPARE(status,"expired");CTEST_CHECK(!channel->respond(id,{{"behavior","allow"}})["accepted"].toBool());
        f.context.cancellation=CancellationToken{};
        CTEST_THROWS(Error,f.run(question(),[&](const auto& e){observe(e);if(e.kind==a::EventKind::PermissionRequested)f.context.cancellation.cancel();}));
        CTEST_CHECK(channel->pending()["requests"].toArray().isEmpty());CTEST_CHECK(!channel->respond(id,{{"behavior","allow"}})["accepted"].toBool());
    }
    void disabledEngineAndReservedIdentity() {
        QTemporaryDir root;a::EngineOptions config{.decision={.enabled=false}};config.sessionsDirectory=root.filePath("sessions");
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>();
        a::Engine disabled(model,registry,policy,config);CTEST_CHECK(!disabled.userQuestionTool());const auto id=disabled.createSession("fixture",root.path()).id;
        CTEST_THROWS(Error,disabled.runQuestionTool(id,question()));
        config.userQuestionsEnabled=true;registry->add(a::userQuestionTool());CTEST_THROWS(Error,a::Engine(model,registry,policy,config));
    }
    void endingSessionCancelsAQuestionAndPlanCanContinueAfterAnswer() {
        QTemporaryDir root;a::EngineOptions config{.decision={.enabled=false}};config.sessionsDirectory=root.filePath("sessions");config.userQuestionsEnabled=true;config.planToolsEnabled=true;
        config.permissionRequests=std::make_shared<a::PermissionRequests>();config.projectContext.enabled=false;config.skills.enabled=false;
        a::Engine engine(std::make_shared<Model>(),std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),config);
        const auto id=engine.createSession("fixture",root.path()).id;CTEST_CHECK(!engine.runPlanTool(id,"EnterPlanMode").isError);
        auto answered=engine.runQuestionTool(id,question(),{},[&](const auto& e) {
            if(e.kind==a::EventKind::PermissionRequested){auto args=question();args["answers"]=QJsonObject{{"Which renderer?","Qt"}};
                config.permissionRequests->respond(e.data["request_id"].toString(),{{"behavior","allow"},{"updatedInput",args}});}
        });CTEST_CHECK(!answered.isError);CTEST_COMPARE(engine.planStatus(id)["phase"],"planning");
        auto pending=std::async(std::launch::async,[&]{return engine.runQuestionTool(id,question());});
        CTEST_WAIT_VERIFY(!config.permissionRequests->pending()["requests"].toArray().isEmpty(),3000);
        (void)engine.endSession(id);CTEST_THROWS(Error,(void)pending.get());CTEST_CHECK(config.permissionRequests->pending()["requests"].toArray().isEmpty());
    }
    void modelConsumesTrustedHostAnswer() {
        QTemporaryDir root;auto model=std::make_shared<Model>();a::EngineOptions config{.decision={.enabled=false}};config.sessionsDirectory=root.filePath("sessions");
        config.projectContext.enabled=false;config.compaction.automatic=false;config.skills.enabled=false;
        config.userQuestionsEnabled=true;config.userQuestions.deferred=false;
        int reviews=0;config.permissionResponse=[&](const auto& call,const auto&,const auto&) {
            ++reviews;auto args=call.arguments;args["answers"]=QJsonObject{{"Which renderer?","HOST_QT"}};
            return a::PermissionResponse{a::PermissionBehavior::Allow,{},args};
        };
        a::Engine engine(model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),config);
        const auto id=engine.createSession("fixture",root.path()).id;
        const auto result=engine.run({id,"Ask which renderer to use"}).result.get();
        CTEST_COMPARE(result.status,a::RunStatus::Completed);CTEST_CHECK_MESSAGE(result.text.contains("HOST_QT"),qPrintable(result.text));CTEST_COMPARE(reviews,1);
        CTEST_CHECK(!model->requests.last().messages.last().isError);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) { std::cerr << "Expected a case name" << std::endl; return 1; }
    const std::string_view selection = argc == 2 ? argv[1] : "";
    const bool malformedRows = selection == "malformedQuestionsFailBeforeReview";
    const bool responseRows = selection == "hostResponseCannotReplaceQuestionsOrInjectUnknownAnswers";
    const bool onlyRows = malformedRows || responseRows;
    agent_ctest::CTestRunner runner(onlyRows ? std::string_view{} : selection);
    UserQuestionsTests tests;
    if (!onlyRows)
        runner.run("deferredQuestionsAreDiscoveredAndResumeWithTheAnswer", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.deferredQuestionsAreDiscoveredAndResumeWithTheAnswer(); });
    if (!onlyRows)
        runner.run("missingResponseAndDontAskNeverInventAnAnswer", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.missingResponseAndDontAskNeverInventAnAnswer(); });
    if (!onlyRows)
        runner.run("questionsOptionsFreeTextAndAnnotationsRoundTrip", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.questionsOptionsFreeTextAndAnnotationsRoundTrip(); });
    if (!responseRows) for (const auto& row : tests.malformedQuestionsData()) {
        const auto name = std::string("malformedQuestionsFailBeforeReview:") + row.first.toStdString();
        runner.run(name, [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.malformedQuestionsFailBeforeReview(row.second); });
    }
    if (!malformedRows) for (const auto& row : tests.hostResponseChanges()) {
        const auto name = std::string("hostResponseCannotReplaceQuestionsOrInjectUnknownAnswers:") + row.toStdString();
        runner.run(name, [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.hostResponseCannotReplaceQuestionsOrInjectUnknownAnswers(row); });
    }
    if (!onlyRows)
        runner.run("htmlValidationIsAnIntentCheckNotASanitizer", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.htmlValidationIsAnIntentCheckNotASanitizer(); });
    if (!onlyRows)
        runner.run("beforeToolCannotForgeAnswersButPermissionRequestHookCanRespond", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.beforeToolCannotForgeAnswersButPermissionRequestHookCanRespond(); });
    if (!onlyRows)
        runner.run("channelAnswerIsOwnedAndReplayCannotChangeIt", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.channelAnswerIsOwnedAndReplayCannotChangeIt(); });
    if (!onlyRows)
        runner.run("expiredAndCancelledQuestionsCannotComplete", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.expiredAndCancelledQuestionsCannotComplete(); });
    if (!onlyRows)
        runner.run("disabledEngineAndReservedIdentity", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.disabledEngineAndReservedIdentity(); });
    if (!onlyRows)
        runner.run("endingSessionCancelsAQuestionAndPlanCanContinueAfterAnswer", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.endingSessionCancelsAQuestionAndPlanCanContinueAfterAnswer(); });
    if (!onlyRows)
        runner.run("modelConsumesTrustedHostAnswer", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.modelConsumesTrustedHostAnswer(); });
    return runner.result();
}

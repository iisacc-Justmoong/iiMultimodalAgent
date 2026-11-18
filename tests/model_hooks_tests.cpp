#include <iiMultimodalAgent/agent/CommandHooks.h>
#include <iiMultimodalAgent/agent/Engine.h>
#include <iiMultimodalAgent/agent/PermissionRequests.h>
#include <iiMultimodalAgent/agent/Subagents.h>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include "CTestSupport.h"
#include <QtCore/QCoreApplication>
#include <future>
#include <mutex>
#include <thread>
#include <atomic>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
using namespace std::chrono_literals;
namespace {
class Model final:public a::Model {
public:
    std::mutex mutex;
    QList<a::ModelRequest> requests;
    std::function<a::ModelReply(const a::ModelRequest&,const CancellationToken&,const TextCallback&)> next;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const TextCallback& stream) override {
        {std::lock_guard lock(mutex);requests.append(request);}
        return next?next(request,token,stream):a::ModelReply{"{\"ok\":true}"};
    }
};
QJsonObject settings(QString event,QJsonObject hook) {
    return {{"hooks",QJsonObject{{event,QJsonArray{QJsonObject{{"hooks",QJsonArray{hook}}}}}}}};
}
QJsonObject prompt(QString text="Check $ARGUMENTS") {return {{"type","prompt"},{"prompt",text}};}
QJsonObject diagnostic(const a::HookResult& result){return result.diagnostics.last().toObject();}
a::HookInput input(const std::shared_ptr<Model>& model,const QString& workspace) {
    a::HookInput value{a::HookKind::BeforeTool,"session","run",{"call","Write",{{"path","file.txt"},{"content","MODEL_DATA"}}}};
    auto context=std::make_shared<a::ModelHookContext>();context->model=model;context->modelName="model://fixture";
    auto session=std::make_shared<a::Session>();session->id=value.sessionId;session->workingDirectory=workspace;session->model=context->modelName;
    session->messages={{{"user"},a::MessageRole::User,"HISTORY"},{{"assistant"},a::MessageRole::Assistant,{}, {value.call}}};
    context->session=session;value.modelContext=context;return value;
}
}
class ModelHooksTests final {
public:
    void configurationAndMissingModelAreExplicit() {
        QTemporaryDir work;a::CommandHookOptions options;options.workingDirectory=work.path();
        auto object=prompt();object["model"]="model://override";
        a::CommandHooks hooks(settings("Stop",object),options);
        CTEST_COMPARE(hooks.describe()["provider"],"prompt");CTEST_COMPARE(hooks.describe()["hooks"].toArray().first().toObject()["timeout_ms"],30000);
        CTEST_CHECK(!QJsonDocument(hooks.describe()).toJson().contains("Check $ARGUMENTS"));
        for(const auto& value:QList<QJsonObject>{{{"type","unknown"},{"prompt","x"}},{{"type","prompt"},{"prompt",""}},
            {{"type","prompt"},{"prompt",4}},{{"type","prompt"},{"prompt","x"},{"model",false}},
            {{"type","prompt"},{"prompt","x"},{"timeout",0}},{{"type","prompt"},{"prompt","x"},{"async",true}}})
            CTEST_THROWS(Error,a::CommandHooks(settings("Stop",value),options));
        const auto missing=hooks.callback()({a::HookKind::Stop,"s","r"},{});
        CTEST_COMPARE(diagnostic(missing)["error_code"],"runtime_unavailable");CTEST_CHECK(!missing.block&&!missing.stop);
        auto model=std::make_shared<Model>();auto value=input(model,work.path());value.kind=a::HookKind::Stop;
        hooks.callback()(value,{});CTEST_COMPARE(model->requests.last().model,"model://override");
    }
    void argumentsAreSubstitutedOnceAndAfterToolUsesActualObservation() {
        QTemporaryDir work;auto model=std::make_shared<Model>();auto value=input(model,work.path());
        value.call.arguments["content"]="$ARGUMENTS $0 $(touch BAD) ${HOME}";
        value.kind=a::HookKind::AfterTool;value.result={"ACTUAL_RESULT",{{"verified",true}}};
        a::CommandHookOptions options;options.workingDirectory=work.path();
        auto callback=a::CommandHooks(settings("PostToolUse",prompt("Check $ARGUMENTS")),options).callback();
        const auto result=callback(value,{});CTEST_COMPARE(diagnostic(result)["outcome"],"success");
        auto request=model->requests.last();CTEST_COMPARE(request.messages[2].text,"ACTUAL_RESULT");CTEST_COMPARE(request.messages[2].data["verified"],true);
        CTEST_CHECK(request.messages.last().text.contains("$ARGUMENTS $0 $(touch BAD) ${HOME}"));CTEST_CHECK(!QFileInfo::exists(work.filePath("BAD")));
        callback=a::CommandHooks(settings("PostToolUse",prompt("Literal condition")),options).callback();callback(value,{});
        CTEST_CHECK(model->requests.last().messages.last().text.startsWith("Literal condition\n\nARGUMENTS: {"));
        callback=a::CommandHooks(settings("PostToolUse",prompt("$ARGUMENTS[0]|$0|$999999999999999999999999999999")),options).callback();callback(value,{});
        const auto expanded=model->requests.last().messages.last().text;const auto parts=expanded.split('|');CTEST_COMPARE(parts.size(),3);CTEST_COMPARE(parts[0],parts[1]);CTEST_CHECK(parts[2].isEmpty());
    }
    void limitsRejectContextAndOutputWithoutApprovingAnything() {
        QTemporaryDir work;auto model=std::make_shared<Model>();auto value=input(model,work.path());
        a::CommandHookOptions options;options.workingDirectory=work.path();options.maxInputBytes=1000;
        auto context=std::make_shared<a::ModelHookContext>(*value.modelContext);auto session=std::make_shared<a::Session>(*context->session);
        session->messages.first().text=QString(2000,'x');context->session=session;value.modelContext=context;
        auto result=a::CommandHooks(settings("PreToolUse",prompt()),options).callback()(value,{});
        CTEST_COMPARE(diagnostic(result)["error_code"],"resource_limit");CTEST_CHECK(model->requests.isEmpty());
        options.maxInputBytes=65536;options.maxOutputBytes=16;
        for(bool stream:{false,true}) {
            model->next=[stream](const auto&,const auto&,const TextCallback& cb){const auto text=QString(20,QChar(0xac00));if(stream)cb(text);return a::ModelReply{text};};
            result=a::CommandHooks(settings("PreToolUse",prompt()),options).callback()(value,{});
            CTEST_COMPARE(diagnostic(result)["error_code"],"resource_limit");CTEST_CHECK(!result.block&&!result.stop&&!result.permission);
        }
        options.maxOutputBytes=65536;
        model->next=[](const auto&,const auto&,const auto&){return a::ModelReply{"{\"ok\":true}",{{"malicious","Write",{{"path","BAD"},{"content","x"}}}}};};
        result=a::CommandHooks(settings("PreToolUse",prompt()),options).callback()(value,{});
        CTEST_COMPARE(diagnostic(result)["error_code"],"protocol_error");CTEST_CHECK(!QFileInfo::exists(work.filePath("BAD")));
        model->next=[](const auto&,const auto&,const auto&)->a::ModelReply{throw Error(ErrorCode::Cancelled,"Evaluator cancelled internally");};
        CancellationToken parent;result=a::CommandHooks(settings("PreToolUse",prompt()),options).callback()(value,parent);
        CTEST_COMPARE(diagnostic(result)["outcome"],"cancelled");CTEST_CHECK(!parent.isCancelled());
    }
    void deduplicationOnceAndSharedConcurrencyLimits() {
        QTemporaryDir work;auto model=std::make_shared<Model>();auto value=input(model,work.path());
        a::CommandHookOptions options;options.workingDirectory=work.path();options.maxConcurrentProcesses=1;
        auto one=prompt("same"),two=one;one["model"]="first";two["model"]="last";two["once"]=true;
        QJsonObject config{{"hooks",QJsonObject{{"PreToolUse",QJsonArray{QJsonObject{{"hooks",QJsonArray{one,two,prompt("different")}}}}}}}};
        auto hooks=a::CommandHooks(config,options);std::atomic<int> active{0},peak{0};
        model->next=[&](const auto&,const auto&,const auto&){const auto n=++active;peak=std::max(peak.load(),n);std::this_thread::sleep_for(10ms);--active;return a::ModelReply{"{\"ok\":true}"};};
        auto callback=hooks.callback();auto pending=std::async(std::launch::async,[&]{return callback(value,{});});callback(value,{});(void)pending.get();
        CTEST_COMPARE(peak.load(),1);CTEST_COMPARE(model->requests.size(),3);
        int last=0;for(const auto& request:model->requests){CTEST_CHECK(request.model!="first");if(request.model=="last")++last;}CTEST_COMPARE(last,1);
        value.sessionId="other";callback(value,{});CTEST_COMPARE(model->requests.size(),5);
    }
    void engineLifecycleGetsSnapshotAndStopFalseCancelsWithoutRecursion() {
        QTemporaryDir work;auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work.path());
        QJsonObject events;for(const QString& event:{"SessionStart","UserPromptSubmit","BeforeModel","AfterModel","PreToolUse","PostToolUse","Stop","SessionEnd"})
            events[event]=settings(event,prompt("$ARGUMENTS"))["hooks"].toObject()[event];
        a::CommandHookOptions hookOptions;hookOptions.workingDirectory=work.path();a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=work.filePath("sessions");
        options.skills.enabled=false;options.projectContext.enabled=false;options.hooks={a::CommandHooks({{"hooks",events}},hookOptions).callback()};
        QHash<QString,QList<a::ModelRequest>> observed;int turns=0;
        model->next=[&](const a::ModelRequest& request,const auto&,const auto&){
            if(!request.responseSchema.isEmpty()) {
                const auto event=QJsonDocument::fromJson(request.messages.last().text.toUtf8()).object()["hook_event_name"].toString();observed[event].append(request);
                return a::ModelReply{QString("{\"ok\":%1,\"reason\":\"STOP_CONDITION\"}").arg(event=="Stop"||event=="SessionStart"||event=="SessionEnd"?"false":"true")};
            }
            if(++turns==1)return a::ModelReply{{},{{"write","Write",{{"path","actual.txt"},{"content","EXECUTED_ONCE"}}}}};
            return a::ModelReply{"done"};
        };
        a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(a::PermissionMode::AcceptEdits),options);
        const auto session=engine.createSession("model://fixture",work.path());CTEST_COMPARE(engine.hookModel(),model);
        const auto result=engine.run({session.id,"WRITE_REQUEST"}).result.get();CTEST_COMPARE(result.status,a::RunStatus::Cancelled);CTEST_CHECK(result.errorMessage.contains("STOP_CONDITION"));CTEST_COMPARE(turns,2);
        const auto persisted=engine.session(session.id);CTEST_COMPARE(persisted.messages.size(),4);CTEST_CHECK(a::pendingToolCalls(persisted.messages).isEmpty());
        CTEST_COMPARE(observed["UserPromptSubmit"].size(),1);CTEST_COMPARE(observed["UserPromptSubmit"].first().messages.size(),1);
        const auto pre=observed["PreToolUse"].first();CTEST_COMPARE(pre.messages.first().text,"WRITE_REQUEST");CTEST_COMPARE(pre.messages[2].data["iilocal.hook_pending"],true);
        const auto post=observed["PostToolUse"].first();CTEST_COMPARE(post.messages[2].role,a::MessageRole::Tool);CTEST_CHECK(!post.messages[2].data.contains("iilocal.hook_pending"));
        QFile file(work.filePath("actual.txt"));CTEST_CHECK(file.open(QIODevice::ReadOnly));CTEST_COMPARE(file.readAll(),"EXECUTED_ONCE");
        const auto ended=engine.endSession(session.id);CTEST_CHECK(ended["ended"].toBool());CTEST_COMPARE(observed["SessionEnd"].first().messages.size(),5);
        for(auto it=events.begin();it!=events.end();++it)CTEST_CHECK_MESSAGE(!observed[it.key()].isEmpty(),qPrintable(it.key()));
    }
    void taskTransactionsAndSubagentHooksUseTheirHostModel() {
        QTemporaryDir root;const auto work=root.filePath("work");CTEST_CHECK(QDir().mkpath(work));
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        QJsonObject events;for(const QString& event:{"TaskCreated","TaskCompleted","SubagentStart","SubagentStop"})
            events[event]=settings(event,prompt("$ARGUMENTS"))["hooks"].toObject()[event];
        a::CommandHookOptions limits;limits.workingDirectory=work;a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=root.filePath("parents");
        options.taskToolsEnabled=true;options.skills.enabled=false;options.projectContext.enabled=false;
        options.hooks={a::CommandHooks({{"hooks",events}},limits).callback()};
        a::SubagentOptions children;children.workingDirectory=work;children.stateDirectory=root.filePath("children");
        a::Subagents::attach(options,std::make_shared<a::Subagents>(model,registry,policy,options,children));
        QHash<QString,a::ModelRequest> observed;
        model->next=[&](const a::ModelRequest& request,const auto&,const auto&){
            if(request.responseSchema.isEmpty())return a::ModelReply{"CHILD_FINISHED"};
            const auto body=QJsonDocument::fromJson(request.messages.last().text.toUtf8()).object();observed[body["hook_event_name"].toString()]=request;
            return a::ModelReply{body["task_subject"]=="REJECT_TASK"?"{\"ok\":false,\"reason\":\"REJECT_TASK\"}":"{\"ok\":true}"};
        };
        a::Engine engine(model,registry,policy,options);const auto parent=engine.createSession("model://fixture",work);
        const auto task=engine.runTaskTool(parent.id,"TaskCreate",{{"subject","ACCEPT_TASK"},{"description","test"}});CTEST_CHECK_MESSAGE(!task.isError,qPrintable(task.text));
        const auto updated=engine.runTaskTool(parent.id,"TaskUpdate",{{"taskId",task.data["task"].toObject()["id"]},{"status","completed"}});CTEST_CHECK_MESSAGE(!updated.isError,qPrintable(updated.text));
        CTEST_THROWS(Error,engine.runTaskTool(parent.id,"TaskCreate",{{"subject","REJECT_TASK"},{"description","test"}}));
        const auto listed=engine.runTaskTool(parent.id,"TaskList");CTEST_CHECK(!QJsonDocument(listed.data).toJson().contains("REJECT_TASK"));
        const auto child=engine.runSubagentTool(parent.id,"Agent",{{"prompt","Child request"}});CTEST_CHECK_MESSAGE(!child.isError,qPrintable(child.text));
        CTEST_COMPARE(child.data["result"].toObject()["text"],"CHILD_FINISHED");
        for(auto it=events.begin();it!=events.end();++it){CTEST_CHECK_MESSAGE(observed.contains(it.key()),qPrintable(it.key()));CTEST_COMPARE(observed[it.key()].model,"model://fixture");}
        const auto stop=observed["SubagentStop"];CTEST_CHECK(stop.messages.size()>=3);CTEST_COMPARE(stop.messages[stop.messages.size()-2].text,"CHILD_FINISHED");
        CTEST_COMPARE(engine.session(parent.id).messages.size(),0);
    }
    void toolAndPermissionHooksRespectHostPolicy() {
        QTemporaryDir work;auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work.path());
        a::CommandHookOptions limits;limits.workingDirectory=work.path();a::ToolRunnerOptions options;options.hookModel=model;options.hookModelName="model://fixture";
        options.hooks={a::CommandHooks(settings("PreToolUse",prompt()),limits).callback()};
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::AcceptEdits,QList<a::PermissionRule>{{"Write",a::PermissionBehavior::Deny}});
        const a::ToolCall call{"id","Write",{{"path","forbidden.txt"},{"content","x"}}};const a::ToolContext context{"s","r",work.path()};
        CTEST_CHECK(a::ToolRunner(registry,policy,options).run(call,context).isError);CTEST_COMPARE(model->requests.size(),1);CTEST_CHECK(!QFileInfo::exists(work.filePath("forbidden.txt")));
        model->next=[](const auto&,const auto&,const auto&){return a::ModelReply{"{\"ok\":false,\"reason\":\"PROMPT_DENY\"}"};};
        options.hooks={a::CommandHooks(settings("PermissionRequest",prompt()),limits).callback()};
        for(bool broker:{false,true}) {
            options.permissionRequests=broker?std::make_shared<a::PermissionRequests>():nullptr;
            try{(void)a::ToolRunner(registry,std::make_shared<a::RulePolicy>(),options).run(call,context);agent_ctest::fail(__FILE__, __LINE__, "Prompt denial did not interrupt the request");}
            catch(const Error& error){CTEST_COMPARE(error.code(),ErrorCode::Cancelled);}
            CTEST_CHECK(!QFileInfo::exists(work.filePath("forbidden.txt")));
        }
    }
    void promptUsesHistoryAndSchemaWithoutExecutingPendingTools() {
        QTemporaryDir work;auto model=std::make_shared<Model>();auto value=input(model,work.path());
        a::CommandHookOptions options;options.workingDirectory=work.path();
        const auto result=a::CommandHooks(settings("PreToolUse",prompt()),options).callback()(value,{});
        CTEST_COMPARE(diagnostic(result)["outcome"],"success");CTEST_CHECK(!result.block&&!result.stop);CTEST_COMPARE(model->requests.size(),1);
        const auto request=model->requests.first();CTEST_COMPARE(request.model,"model://fixture");
        CTEST_COMPARE(request.responseSchema["properties"].toObject()["ok"].toObject()["type"],"boolean");
        CTEST_CHECK(request.enableThinking.has_value()&&!*request.enableThinking);CTEST_CHECK(request.systemPromptOnly);CTEST_COMPARE(request.toolChoice,"none");
        CTEST_COMPARE(request.messages.first().text,"HISTORY");
        CTEST_COMPARE(request.messages[2].role,a::MessageRole::Tool);CTEST_COMPARE(request.messages[2].toolCallId,"call");
        CTEST_CHECK(request.messages.last().text.contains("MODEL_DATA"));CTEST_CHECK(request.messages.last().text.contains("PreToolUse"));
        CTEST_COMPARE(value.modelContext->session->messages.size(),2);CTEST_CHECK(!QFileInfo::exists(work.filePath("file.txt")));
    }
    void falseBlocksContinuationAndInvalidOutputIsNotAValidDecision() {
        QTemporaryDir work;auto model=std::make_shared<Model>();auto value=input(model,work.path());
        a::CommandHookOptions options;options.workingDirectory=work.path();
        model->next=[](const auto&,const auto&,const auto&){return a::ModelReply{"{\"ok\":false,\"reason\":\"NOT_VERIFIED\"}"};};
        auto result=a::CommandHooks(settings("PreToolUse",prompt()),options).callback()(value,{});
        CTEST_CHECK(result.block&&result.stop);CTEST_COMPARE(result.stopReason,"NOT_VERIFIED");
        for(const QString& text:{"not JSON","[]","{\"ok\":\"false\"}","{\"ok\":false,\"reason\":42}","{\"ok\":true,\"permissionDecision\":\"allow\"}"}) {
            model->next=[text](const auto&,const auto&,const auto&){return a::ModelReply{text};};
            result=a::CommandHooks(settings("PreToolUse",prompt()),options).callback()(value,{});
            CTEST_COMPARE(diagnostic(result)["outcome"],"non_blocking_error");CTEST_CHECK(!result.block&&!result.stop&&!result.permission);
        }
    }
    void timeoutCancelsOnlyEvaluatorAndExternalCancellationPropagates() {
        QTemporaryDir work;auto model=std::make_shared<Model>();auto value=input(model,work.path());
        a::CommandHookOptions options;options.workingDirectory=work.path();
        model->next=[](const auto&,const CancellationToken& token,const auto&){while(!token.isCancelled())std::this_thread::sleep_for(2ms);token.throwIfCancelled();return a::ModelReply{};};
        auto hook=prompt();hook["timeout"]=0.04;CancellationToken parent;
        const auto result=a::CommandHooks(settings("PreToolUse",hook),options).callback()(value,parent);
        CTEST_CHECK(!parent.isCancelled());CTEST_COMPARE(diagnostic(result)["outcome"],"cancelled");CTEST_COMPARE(diagnostic(result)["error_code"],"timeout");
        hook["timeout"]=5;auto callback=a::CommandHooks(settings("PreToolUse",hook),options).callback();
        auto pending=std::async(std::launch::async,[&]{return callback(value,parent);});
        std::this_thread::sleep_for(20ms);parent.cancel();CTEST_CHECK(pending.wait_for(500ms)==std::future_status::ready);
        try{(void)pending.get();agent_ctest::fail(__FILE__, __LINE__, "Parent cancellation was ignored");}catch(const Error& error){CTEST_COMPARE(error.code(),ErrorCode::Cancelled);}
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner;
    ModelHooksTests tests;
    runner.run("configurationAndMissingModelAreExplicit", [&] { tests.configurationAndMissingModelAreExplicit(); });
    runner.run("argumentsAreSubstitutedOnceAndAfterToolUsesActualObservation", [&] { tests.argumentsAreSubstitutedOnceAndAfterToolUsesActualObservation(); });
    runner.run("limitsRejectContextAndOutputWithoutApprovingAnything", [&] { tests.limitsRejectContextAndOutputWithoutApprovingAnything(); });
    runner.run("deduplicationOnceAndSharedConcurrencyLimits", [&] { tests.deduplicationOnceAndSharedConcurrencyLimits(); });
    runner.run("engineLifecycleGetsSnapshotAndStopFalseCancelsWithoutRecursion", [&] { tests.engineLifecycleGetsSnapshotAndStopFalseCancelsWithoutRecursion(); });
    runner.run("taskTransactionsAndSubagentHooksUseTheirHostModel", [&] { tests.taskTransactionsAndSubagentHooksUseTheirHostModel(); });
    runner.run("toolAndPermissionHooksRespectHostPolicy", [&] { tests.toolAndPermissionHooksRespectHostPolicy(); });
    runner.run("promptUsesHistoryAndSchemaWithoutExecutingPendingTools", [&] { tests.promptUsesHistoryAndSchemaWithoutExecutingPendingTools(); });
    runner.run("falseBlocksContinuationAndInvalidOutputIsNotAValidDecision", [&] { tests.falseBlocksContinuationAndInvalidOutputIsNotAValidDecision(); });
    runner.run("timeoutCancelsOnlyEvaluatorAndExternalCancellationPropagates", [&] { tests.timeoutCancelsOnlyEvaluatorAndExternalCancellationPropagates(); });
    return runner.result();
}

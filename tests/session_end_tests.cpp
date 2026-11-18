#include "iiMultimodalAgent/agent/Engine.h"
#include "iiMultimodalAgent/agent/CommandHooks.h"
#include "iiMultimodalAgent/agent/Api.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QCoreApplication>
#include <atomic>
#include <thread>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
using namespace std::chrono_literals;
namespace a=iiMultimodalAgent::agent;
namespace {
class Model final:public a::Model {
public:
    std::atomic_bool waiting=false;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const TextCallback&) override {
        if(request.messages.last().text=="wait") {
            waiting=true;while(!token.isCancelled())std::this_thread::sleep_for(1ms);token.throwIfCancelled();
        }
        return {"ANSWER",{}};
    }
};
QJsonObject config(const QString& command,const QString& matcher="*") {
    return {{"hooks",QJsonObject{{"SessionEnd",QJsonArray{QJsonObject{{"matcher",matcher},
        {"hooks",QJsonArray{QJsonObject{{"type","command"},{"command",command}}}}}}}}}};
}
struct Host {
    QTemporaryDir root;
    QString workspace=root.filePath("work");
    std::shared_ptr<Model> model=std::make_shared<Model>();
    a::EngineOptions options{.decision={.enabled=false}};
    Host(){QDir().mkpath(workspace);options.sessionsDirectory=root.filePath("sessions");options.compaction.automatic=false;}
    std::unique_ptr<a::Engine> engine(){return std::make_unique<a::Engine>(model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),options);}
};
}
class SessionEndTests final:public QObject {
public:
    void endIsOncePerActivationAndAllowsReadOnlyReentryAndResume() {
        Host host;a::Engine* current=nullptr;QStringList sources;int ends=0;bool couldRead=false,admissionBlocked=false;
        host.options.hooks.append([&](const a::HookInput& input,const CancellationToken&) {
            a::HookResult result;
            if(input.kind==a::HookKind::SessionStart)sources.append(input.context["source"].toString());
            if(input.kind==a::HookKind::SessionEnd) {
                ++ends;couldRead=current->session(input.sessionId).messages.last().text=="ANSWER";
                admissionBlocked=current->run({input.sessionId,"cannot race end"}).result.get().errorCode==ErrorCode::ModelInUse;
                result.block=true;result.stop=true;result.feedback="END_MUST_NOT_ENTER_HISTORY";result.initialUserMessage="NO_NEW_INPUT";
            }
            return result;
        });
        auto engine=host.engine();current=engine.get();const auto id=engine->createSession("local",host.workspace).id;
        CTEST_CHECK(!engine->endSession(id)["ended"].toBool());CTEST_COMPARE(ends,0);
        CTEST_COMPARE(engine->run({id,"hello"}).result.get().status,a::RunStatus::Completed);
        engine->enqueueInput(id,{{"text","PRESERVED"},{"priority","later"}});
        const auto report=engine->endSession(id,"logout");CTEST_COMPARE(report["ended"],true);CTEST_CHECK(couldRead&&admissionBlocked);
        CTEST_COMPARE(engine->session(id).messages.size(),2);CTEST_COMPARE(engine->queuedInputs(id)["count"],1);
        CTEST_COMPARE(ends,1);CTEST_COMPARE(report["diagnostics"].toArray().first().toObject()["outcome"],"ignored_control");
        CTEST_CHECK(!engine->endSession(id)["ended"].toBool());CTEST_COMPARE(ends,1);
        CTEST_COMPARE(engine->run({id,"again"}).result.get().status,a::RunStatus::Completed);
        CTEST_COMPARE(sources,(QStringList{"startup","resume"}));
        engine->close();CTEST_COMPARE(ends,2);CTEST_CHECK(engine->close().isEmpty());
        CTEST_COMPARE(engine->run({id,"closed"}).result.get().errorCode,ErrorCode::ShuttingDown);
        CTEST_THROWS(Error,engine->createSession("local",host.workspace));
        CTEST_THROWS(Error,engine->forkSession(id));
    }
    void runningAndQueuedCancellationIsScopedAndConcurrentEndDoesNotDuplicate() {
        Host host;host.options.maxConcurrentRuns=1;std::atomic_int ends=0;
        host.options.hooks.append([&](const a::HookInput& input,const CancellationToken&) {
            if(input.kind==a::HookKind::SessionEnd){++ends;std::this_thread::sleep_for(50ms);}return a::HookResult{};
        });
        auto engine=host.engine();const auto first=engine->createSession("local",host.workspace).id;
        const auto second=engine->createSession("local",host.workspace).id;
        auto running=engine->run({first,"wait"});CTEST_WAIT_DEFAULT(host.model->waiting.load());
        auto queued=engine->run({second,"queued"});
        const auto unstarted=engine->endSession(second);CTEST_CHECK(!unstarted["ended"].toBool());
        CTEST_COMPARE(running.result.wait_for(0ms),std::future_status::timeout);
        running.cancel();CTEST_COMPARE(running.result.get().status,a::RunStatus::Cancelled);
        CTEST_COMPARE(queued.result.get().status,a::RunStatus::Cancelled);
        auto one=std::async(std::launch::async,[&]{return engine->endSession(first,"clear");});
        auto two=std::async(std::launch::async,[&]{return engine->endSession(first,"other");});
        const auto a=one.get(),b=two.get();CTEST_COMPARE(int(a["ended"].toBool())+int(b["ended"].toBool()),1);CTEST_COMPARE(ends.load(),1);
        engine->close();CTEST_COMPARE(ends.load(),1);
    }
    void activeRunEndsBeforeTheHookAndOtherSessionsRemainUsable() {
        Host host;int ends=0;host.options.hooks.append([&](const a::HookInput& input,const CancellationToken& token){
            if(input.kind==a::HookKind::SessionEnd){token.throwIfCancelled();++ends;}return a::HookResult{};});
        auto engine=host.engine();const auto id=engine->createSession("local",host.workspace).id;
        auto run=engine->run({id,"wait"});CTEST_WAIT_DEFAULT(host.model->waiting.load());
        const auto result=engine->endSession(id,"resume");CTEST_COMPARE(result["ended"],true);
        CTEST_COMPARE(run.result.get().status,a::RunStatus::Cancelled);CTEST_COMPARE(ends,1);
        const auto other=engine->createSession("local",host.workspace).id;
        CTEST_COMPARE(engine->run({other,"unaffected"}).result.get().status,a::RunStatus::Completed);engine->close();
    }
    void exceptionsCannotVetoEndAndChildEnginesDoNotEmitMainEnd() {
        Host host;host.options.sessionEndTimeoutMs=1500;int afterThrow=0;
        host.options.hooks.append([](const a::HookInput& input,const CancellationToken&){
            if(input.kind==a::HookKind::SessionEnd)throw std::runtime_error("EXPECTED_END_FAILURE");return a::HookResult{};});
        host.options.hooks.append([&](const a::HookInput& input,const CancellationToken&){
            if(input.kind==a::HookKind::SessionEnd)++afterThrow;
            return a::HookResult{};});
        auto engine=host.engine();const auto id=engine->createSession("local",host.workspace).id;
        engine->run({id,"hello"}).result.get();const auto begin=std::chrono::steady_clock::now();
        auto result=engine->endSession(id);CTEST_CHECK(std::chrono::steady_clock::now()-begin<2s);CTEST_COMPARE(result["timed_out"],false);
        CTEST_CHECK(QJsonDocument(result).toJson().contains("EXPECTED_END_FAILURE"));CTEST_COMPARE(afterThrow,1);
        engine->close();host.options.sessionStartHooks=false;auto child=host.engine();
        child->run({id,"child"}).result.get();child->close();CTEST_COMPARE(afterThrow,1);
    }
    void shortBudgetStopsLaterHooksWithoutRequiringCallbackAdmission() {
        Host host;host.options.sessionEndTimeoutMs=40;int late=0;
        host.options.hooks.append([](const a::HookInput& input,const CancellationToken& token){
            if(input.kind==a::HookKind::SessionEnd){while(!token.isCancelled())std::this_thread::sleep_for(1ms);token.throwIfCancelled();}
            return a::HookResult{};});
        host.options.hooks.append([&](const a::HookInput& input,const CancellationToken&){if(input.kind==a::HookKind::SessionEnd)++late;return a::HookResult{};});
        auto engine=host.engine();const auto id=engine->createSession("local",host.workspace).id;engine->run({id,"hello"}).result.get();
        const auto begin=std::chrono::steady_clock::now();const auto result=engine->endSession(id);
        CTEST_CHECK(std::chrono::steady_clock::now()-begin<1s);CTEST_COMPARE(result["timed_out"],true);CTEST_COMPARE(result["ended"],true);CTEST_COMPARE(late,0);
        // The shared budget includes context preparation; it can expire before
        // the first callback under sanitizers or load. Admission is not promised.
        CTEST_COMPARE(engine->endSession(id)["ended"],false);
    }
    void commandReasonMatchersAndCommonBudgetStopQueuedProcesses() {
        try {
        Host host;host.options.sessionEndTimeoutMs=180;a::CommandHookOptions options;options.workingDirectory=host.workspace;options.maxConcurrentProcesses=1;
        auto settings=config("cat > matched.json; sleep 5; touch TOO_LATE","logout|clear");
        auto groups=settings["hooks"].toObject()["SessionEnd"].toArray();
        groups.append(QJsonObject{{"hooks",QJsonArray{QJsonObject{{"type","command"},{"command","sleep 5; touch ALSO_TOO_LATE"}}}}});
        settings={{"hooks",QJsonObject{{"SessionEnd",groups}}}};a::CommandHooks hooks(settings,options);host.options.hooks={hooks.callback()};
        auto engine=host.engine();const auto id=engine->createSession("local",host.workspace).id;engine->run({id,"hello"}).result.get();
        const auto begin=std::chrono::steady_clock::now();const auto report=engine->endSession(id,"logout");
        CTEST_CHECK(std::chrono::steady_clock::now()-begin<2s);CTEST_CHECK(report["timed_out"].toBool());
        CTEST_CHECK(!QFileInfo::exists(host.workspace+"/TOO_LATE"));CTEST_CHECK(!QFileInfo::exists(host.workspace+"/ALSO_TOO_LATE"));
        // Matcher-only invocation is deterministic, independent of pool order.
        a::CommandHooks matcher(config("cat > reason.json","logout|clear"),options);
        a::HookInput input{a::HookKind::SessionEnd,id,{}, {},{},{},{{"reason","other"}}};
        CTEST_CHECK(matcher.callback()(input,{}).diagnostics.isEmpty());input.context["reason"]="clear";matcher.callback()(input,{});
        QFile f(host.workspace+"/reason.json");CTEST_CHECK(f.open(QIODevice::ReadOnly));CTEST_COMPARE(QJsonDocument::fromJson(f.readAll()).object()["reason"],"clear");
        } catch (const std::exception &error) { agent_ctest::fail(__FILE__, __LINE__, error.what()); }
    }
    void invalidOrPreCancelledRequestsLeaveTheSessionOpen() {
        Host host;auto engine=host.engine();const auto id=engine->createSession("local",host.workspace).id;
        engine->run({id,"hello"}).result.get();CTEST_THROWS(Error,engine->endSession(id,"invented"));
        CancellationToken cancelled;cancelled.cancel();CTEST_THROWS(Error,engine->endSession(id,"other",cancelled));
        CTEST_COMPARE(engine->endSession(id)["ended"],true);
        host.options.sessionEndTimeoutMs=0;CTEST_THROWS(Error,host.engine());
    }
    void destructorRunsARealCommandAfterTheTranscriptIsStored() {
        Host host;a::CommandHookOptions options;options.workingDirectory=host.workspace;
        std::unique_ptr<a::CommandHooks> hooks;
        try {hooks=std::make_unique<a::CommandHooks>(config("cat >> end.jsonl; printf 'END_ERROR' >&2; exit 2"),options);}
        catch(const Error& e){agent_ctest::fail(__FILE__, __LINE__, e.what());}
        host.options.hooks.append(hooks->callback());QString id;
        {auto engine=host.engine();id=engine->createSession("model://test",host.workspace).id;
            CTEST_COMPARE(engine->run({id,"hello"}).result.get().status,a::RunStatus::Completed);
            CTEST_CHECK(!QFileInfo::exists(host.workspace+"/end.jsonl"));}
        QFile file(host.workspace+"/end.jsonl");CTEST_CHECK(file.open(QIODevice::ReadOnly));
        const auto body=QJsonDocument::fromJson(file.readAll()).object();
        CTEST_COMPARE(body["hook_event_name"],"SessionEnd");CTEST_COMPARE(body["reason"],"other");CTEST_COMPARE(body["session_id"],id);
        QFile transcript(body["transcript_path"].toString());CTEST_CHECK(transcript.open(QIODevice::ReadOnly));
        CTEST_CHECK(transcript.readAll().contains("ANSWER"));
    }
    void apiExposesAnAuthenticatedSessionEnd() {
        Host host;a::ApiOptions options{.engine={.decision={.enabled=false}}};options.workingDirectory=host.workspace;options.stateDirectory=host.root.filePath("private");
        const QString token(48,'a');options.clientTokens={{"client",token}};
        a::Api api(host.model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),options);
        auto call=[&](const QString& method,const QJsonObject& params){return api.dispatch(method,params,token).result.get().toObject();};
        const auto id=call("agent.sessions.create",{{"model","model://test"}}).value("session_id");
        CTEST_COMPARE(call("agent.run",{{"session_id",id},{"prompt","hello"}})["status"],"completed");
        QJsonObject ended;
        try {ended=call("agent.sessions.end",{{"session_id",id},{"reason","logout"}});}
        catch(const Error& e){agent_ctest::fail(__FILE__, __LINE__, e.what());}
        CTEST_COMPARE(ended["ended"],true);CTEST_COMPARE(ended["reason"],"logout");
    }
    void apiEndCancelsRunningAndQueuedRequestsBeforeResumption() {
        Host host;a::ApiOptions options{.engine={.decision={.enabled=false}}};options.workingDirectory=host.workspace;options.stateDirectory=host.root.filePath("private");
        options.maxConcurrentRequests=1;const QString token(48,'a'),other(48,'b');options.clientTokens={{"client",token},{"other",other}};
        QStringList sources;int ends=0;options.engine.hooks.append([&](const a::HookInput& input,const CancellationToken&){
            if(input.kind==a::HookKind::SessionStart)sources.append(input.context["source"].toString());
            if(input.kind==a::HookKind::SessionEnd)++ends;return a::HookResult{};});
        a::Api api(host.model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),options);
        auto call=[&](const QString& method,const QJsonObject& params){return api.dispatch(method,params,token).result.get().toObject();};
        const auto id=call("agent.sessions.create",{{"model","local"}}).value("session_id");
        CTEST_THROWS(Error,api.dispatch("agent.sessions.end",{{"session_id",id}},other).result.get());
        auto run=api.dispatch("agent.run",{{"session_id",id},{"prompt","wait"}},token);CTEST_WAIT_DEFAULT(host.model->waiting.load());
        auto queued=api.dispatch("agent.run",{{"session_id",id},{"prompt","must not resume"}},token);
        CTEST_COMPARE(call("agent.sessions.end",{{"session_id",id},{"reason","logout"}})["ended"],true);
        CTEST_COMPARE(run.result.get().toObject()["status"],"cancelled");
        CTEST_THROWS(Error,queued.result.get());CTEST_COMPARE(ends,1);CTEST_COMPARE(sources,(QStringList{"startup"}));
        CTEST_COMPARE(call("agent.run",{{"session_id",id},{"prompt","explicit resume"}})["status"],"completed");
        CTEST_COMPARE(sources,(QStringList{"startup","resume"}));api.close();CTEST_COMPARE(ends,2);api.close();CTEST_COMPARE(ends,2);
    }
    void nativeOperationsAreCancelledWithoutCancellingTheirCaller() {
        Host host;host.options.taskToolsEnabled=true;std::atomic_bool waiting=false;
        host.options.hooks.append([&](const a::HookInput& input,const CancellationToken& token){
            if(input.kind==a::HookKind::TaskCreated){waiting=true;while(!token.isCancelled())std::this_thread::sleep_for(1ms);token.throwIfCancelled();}
            return a::HookResult{};});
        auto engine=host.engine();const auto id=engine->createSession("local",host.workspace).id;CancellationToken caller;
        auto native=std::async(std::launch::async,[&]{return engine->runTaskTool(id,"TaskCreate",{{"subject","must roll back"},{"description","cancel in hook"}},caller);});
        CTEST_WAIT_DEFAULT(waiting.load());const auto result=engine->endSession(id);CTEST_COMPARE(result["ended"],false);
        CTEST_THROWS(Error,native.get());CTEST_CHECK(!caller.isCancelled());
        CTEST_CHECK(engine->runTaskTool(id,"TaskList").data["tasks"].toArray().isEmpty());engine->close();
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) { std::cerr << "Expected a case name or --offline" << std::endl; return 1; }
    const std::string_view selection = argc == 2 ? argv[1] : "";
    const bool offline = selection == "--offline";
    agent_ctest::CTestRunner runner(offline ? std::string_view{} : selection);
    SessionEndTests tests;
    runner.run("endIsOncePerActivationAndAllowsReadOnlyReentryAndResume", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.endIsOncePerActivationAndAllowsReadOnlyReentryAndResume(); });
    runner.run("runningAndQueuedCancellationIsScopedAndConcurrentEndDoesNotDuplicate", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.runningAndQueuedCancellationIsScopedAndConcurrentEndDoesNotDuplicate(); });
    runner.run("activeRunEndsBeforeTheHookAndOtherSessionsRemainUsable", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.activeRunEndsBeforeTheHookAndOtherSessionsRemainUsable(); });
    runner.run("exceptionsCannotVetoEndAndChildEnginesDoNotEmitMainEnd", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.exceptionsCannotVetoEndAndChildEnginesDoNotEmitMainEnd(); });
    runner.run("shortBudgetStopsLaterHooksWithoutRequiringCallbackAdmission", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.shortBudgetStopsLaterHooksWithoutRequiringCallbackAdmission(); });
    if (!offline)
        runner.run("commandReasonMatchersAndCommonBudgetStopQueuedProcesses", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.commandReasonMatchersAndCommonBudgetStopQueuedProcesses(); });
    runner.run("invalidOrPreCancelledRequestsLeaveTheSessionOpen", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.invalidOrPreCancelledRequestsLeaveTheSessionOpen(); });
    if (!offline)
        runner.run("destructorRunsARealCommandAfterTheTranscriptIsStored", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.destructorRunsARealCommandAfterTheTranscriptIsStored(); });
    runner.run("apiExposesAnAuthenticatedSessionEnd", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.apiExposesAnAuthenticatedSessionEnd(); });
    runner.run("apiEndCancelsRunningAndQueuedRequestsBeforeResumption", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.apiEndCancelsRunningAndQueuedRequestsBeforeResumption(); });
    runner.run("nativeOperationsAreCancelledWithoutCancellingTheirCaller", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.nativeOperationsAreCancelledWithoutCancellingTheirCaller(); });
    return runner.result();
}

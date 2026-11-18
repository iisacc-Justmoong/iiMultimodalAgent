#include "iiMultimodalAgent/agent/Engine.h"
#include "iiMultimodalAgent/agent/Api.h"
#include "iiMultimodalAgent/agent/ShellTasks.h"
#include "iiMultimodalAgent/agent/Subagents.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include "QtEventWait.h"
#include <QtCore/QCoreApplication>
#include <atomic>
#include <thread>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
using namespace std::chrono_literals;
namespace {
class Model final:public a::Model {
public:
    std::atomic_bool foreground=false,child=false,releaseChild=false;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const TextCallback&) override {
        const auto prompt=request.messages.last().text;
        if(prompt=="foreground") {foreground=true;while(!token.isCancelled())std::this_thread::sleep_for(1ms);token.throwIfCancelled();}
        if(prompt=="child") {child=true;while(!releaseChild&&!token.isCancelled())std::this_thread::sleep_for(1ms);token.throwIfCancelled();}
        if(prompt=="fresh")for(const auto& message:request.messages)if(message.text.contains("OLD_SECRET"))throw std::runtime_error("Old context leaked after clear");
        return {"ANSWER"};
    }
};
struct Host {
    QTemporaryDir root;QString workspace=root.filePath("work");
    std::shared_ptr<Model> model=std::make_shared<Model>();
    std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    a::EngineOptions options{.decision={.enabled=false}};
    Host(){QDir().mkpath(workspace);options.sessionsDirectory=root.filePath("sessions");options.compaction.automatic=false;options.taskToolsEnabled=true;}
};
}
class SessionClearTests final {
public:
    void failuresReturnTheNewIdentityAndCloseWaitsForAnAdmittedClear() {
        Host h;std::atomic_bool entering=false,release=false;bool failStart=true;
        h.options.hooks.append([&](const a::HookInput& input,const CancellationToken&) {
            if(input.kind==a::HookKind::SessionStart&&input.context["source"]=="clear"&&failStart)throw std::runtime_error("EXPECTED_START_FAILURE");
            return a::HookResult{};
        });
        a::Engine engine(h.model,h.registry,h.policy,h.options);const auto old=engine.createSession("fixture",h.workspace).id;
        CancellationToken cancelled;cancelled.cancel();CTEST_THROWS(Error,engine.clearSession(old,cancelled));CTEST_COMPARE(engine.sessions().size(),1);
        const auto failed=engine.clearSession(old);CTEST_CHECK(!failed["complete"].toBool());const auto next=failed["session_id"].toString();CTEST_CHECK(!next.isEmpty());
        CTEST_CHECK(QJsonDocument(failed).toJson().contains("EXPECTED_START_FAILURE"));failStart=false;
        CTEST_COMPARE(engine.run({next,"recover"}).result.get().status,a::RunStatus::Completed);
        a::Tool barrier;barrier.definition={"owner","Trusted transfer",{{"type","object"}}};barrier.execute=[](const auto&,const auto&){return a::ToolResult{};};
        barrier.transferSession=[&](const auto&,const auto&,const auto&){entering=true;const auto until=std::chrono::steady_clock::now()+2s;
            while(!release&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(1ms);return QJsonArray{};};h.registry->add(barrier);
        auto clear=std::async(std::launch::async,[&]{return engine.clearSession(next);});CTEST_WAIT_DEFAULT(entering.load());
        auto close=std::async(std::launch::async,[&]{return engine.close();});CTEST_COMPARE(close.wait_for(30ms),std::future_status::timeout);
        release=true;const auto completed=clear.get();CTEST_CHECK(completed["complete"].toBool());CTEST_CHECK(!close.get().isEmpty());
        CTEST_COMPARE(engine.run({completed["session_id"].toString(),"closed"}).result.get().errorCode,ErrorCode::ShuttingDown);
    }
    void clearStartsANewConversationImmediatelyAndRetainsOldHistory() {
        Host h;QStringList lifecycle;h.options.hooks.append([&](const a::HookInput& input,const CancellationToken&) {
            a::HookResult result;
            if(input.kind==a::HookKind::SessionStart) {
                const auto source=input.context["source"].toString();lifecycle.append("start:"+source);
                if(source=="clear"){result.feedback="CLEAR_CONTEXT";result.initialUserMessage="INITIAL_AFTER_CLEAR";}
            }
            if(input.kind==a::HookKind::SessionEnd)lifecycle.append("end:"+input.context["reason"].toString());
            return result;
        });
        a::Engine engine(h.model,h.registry,h.policy,h.options);
        const auto old=engine.createSession("fixture",h.workspace,"HOST_SYSTEM").id;
        CTEST_COMPARE(engine.run({old,"OLD_SECRET"}).result.get().status,a::RunStatus::Completed);
        engine.enqueueInput(old,{{"text","OLD_PENDING"}});
        engine.runTaskTool(old,"TaskCreate",{{"subject","old task"},{"description","retained only in old session"}});
        const auto report=engine.clearSession(old);const auto next=report["session_id"].toString();
        CTEST_CHECK_MESSAGE(report["complete"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(report).toJson())));CTEST_CHECK(next!=old&&!next.isEmpty());
        CTEST_COMPARE(report["previous_session_id"],old);CTEST_COMPARE(lifecycle,(QStringList{"start:startup","end:clear","start:clear"}));
        const auto session=engine.session(next);CTEST_COMPARE(session.parentSessionId,old);CTEST_COMPARE(session.systemPrompt,"HOST_SYSTEM");
        CTEST_COMPARE(session.messages.size(),1);CTEST_COMPARE(session.messages.first().text,"CLEAR_CONTEXT");CTEST_CHECK(session.compactions.isEmpty());
        CTEST_COMPARE(engine.session(old).messages.size(),2);CTEST_COMPARE(engine.queuedInputs(old)["count"],1);
        CTEST_COMPARE(engine.queuedInputs(next)["inputs"].toArray().first().toObject()["text"],"INITIAL_AFTER_CLEAR");
        CTEST_CHECK(engine.runTaskTool(next,"TaskList").data["tasks"].toArray().isEmpty());
        CTEST_COMPARE(engine.run({next,"fresh"}).result.get().status,a::RunStatus::Completed);CTEST_COMPARE(lifecycle.size(),3);
        CTEST_COMPARE(a::SessionStore(h.options.sessionsDirectory).metadata(next).parentSessionId,old);
        engine.close();CTEST_COMPARE(lifecycle.last(),"end:other");
    }
    void clearCancelsForegroundButKeepsBackgroundChildrenAndShells() {
#if !defined(Q_OS_MACOS) && !defined(Q_OS_LINUX)
        agent_ctest::skip("Requires a desktop POSIX shell");
#endif
        Host h;auto shells=std::make_shared<a::ShellTasks>(h.workspace,h.root.filePath("shells"));a::registerShellTaskControls(*h.registry,shells);
        a::registerWorkspaceTools(*h.registry,h.workspace);
        a::SubagentOptions children;children.workingDirectory=h.workspace;children.stateDirectory=h.root.filePath("children");
        auto agents=std::make_shared<a::Subagents>(h.model,h.registry,h.policy,h.options,children);a::Subagents::attach(h.options,agents);
        a::Engine engine(h.model,h.registry,h.policy,h.options);const auto old=engine.createSession("fixture",h.workspace).id;
        const auto child=engine.runSubagentTool(old,"Agent",{{"prompt","child"},{"run_in_background",true}}).data["agentId"].toString();
        CTEST_WAIT_DEFAULT(h.model->child.load());
        const auto shellId=shells->start({old,{},h.workspace},"while [ ! -f release ]; do sleep .02; done; printf SHELL_FINISHED")["task_id"].toString();
        auto run=engine.run({old,"foreground"});CTEST_WAIT_DEFAULT(h.model->foreground.load());
        const auto report=engine.clearSession(old);CTEST_CHECK(report["complete"].toBool());const auto next=report["session_id"].toString();
        CTEST_COMPARE(run.result.get().status,a::RunStatus::Cancelled);CTEST_COMPARE(agents->list(next).size(),1);CTEST_CHECK(agents->list(old).isEmpty());
        CTEST_THROWS(Error,agents->output(old,child));CTEST_THROWS(Error,shells->output(old,shellId));
        h.model->releaseChild=true;CTEST_COMPARE(agents->output(next,child,true)["status"],"completed");
        QFile release(h.workspace+"/release");CTEST_CHECK(release.open(QIODevice::WriteOnly));release.close();
        CTEST_COMPARE(shells->output(next,shellId,true,3000)["task"].toObject()["output"],"SHELL_FINISHED");
        CTEST_COMPARE(engine.queuedInputs(next)["count"],1);CTEST_COMPARE(engine.queuedInputs(old)["count"],0);engine.close();agents->close();
    }
    void apiClearIsScopedAndChecksSessionCapacityBeforeEnding() {
        Host h;a::ApiOptions options{.engine={.decision={.enabled=false}}};options.workingDirectory=h.workspace;options.stateDirectory=h.root.filePath("private");options.maxSessionsPerClient=2;
        const QString token(48,'a'),other(48,'b');options.clientTokens={{"client",token},{"other",other}};
        a::Api api(h.model,h.registry,h.policy,options);
        auto call=[&](const QString& method,const QJsonObject& params){return api.dispatch(method,params,token).result.get().toObject();};
        const auto id=call("agent.sessions.create",{{"model","fixture"}}).value("session_id");
        CTEST_THROWS(Error,api.dispatch("agent.sessions.clear",{{"session_id",id}},other).result.get());
        const auto clear=call("agent.sessions.clear",{{"session_id",id}});CTEST_CHECK(clear["complete"].toBool());CTEST_CHECK(clear["session_id"]!=id);
        CTEST_COMPARE(call("agent.sessions.get",{{"session_id",clear["session_id"]}})["parent_session_id"],id);
        CTEST_THROWS(Error,call("agent.sessions.clear",{{"session_id",clear["session_id"]}}));
        CTEST_COMPARE(call("agent.run",{{"session_id",clear["session_id"]},{"prompt","still active"}})["status"],"completed");
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) {
        std::cerr << "Expected a case name or --offline" << std::endl;
        return 1;
    }
    const std::string_view selection = argc == 2 ? argv[1] : "";
    const bool offline = selection == "--offline";
    agent_ctest::CTestRunner runner(offline ? std::string_view{} : selection);
    SessionClearTests tests;
    runner.run("failuresReturnTheNewIdentityAndCloseWaitsForAnAdmittedClear", [&] { tests.failuresReturnTheNewIdentityAndCloseWaitsForAnAdmittedClear(); });
    runner.run("clearStartsANewConversationImmediatelyAndRetainsOldHistory", [&] { tests.clearStartsANewConversationImmediatelyAndRetainsOldHistory(); });
    if (!offline)
        runner.run("clearCancelsForegroundButKeepsBackgroundChildrenAndShells", [&] { tests.clearCancelsForegroundButKeepsBackgroundChildrenAndShells(); });
    runner.run("apiClearIsScopedAndChecksSessionCapacityBeforeEnding", [&] { tests.apiClearIsScopedAndChecksSessionCapacityBeforeEnding(); });
    return runner.result();
}

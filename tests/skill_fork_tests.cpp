#include "iiMultimodalAgent/agent/Subagents.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include "QtEventWait.h"
#include <QCoreApplication>
#include <atomic>
#include <chrono>
#include <thread>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a = iiMultimodalAgent::agent;
using namespace std::chrono_literals;
namespace {
void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
class Model final : public a::Model {
public:
    std::function<a::ModelReply(const a::ModelRequest&, const CancellationToken&)> next;
    a::ModelReply generate(const a::ModelRequest& r, const CancellationToken& t, const TextCallback&) override { return next(r,t); }
};
struct Host {
    QTemporaryDir root;
    QString workspace = root.filePath("workspace");
    std::shared_ptr<Model> model = std::make_shared<Model>();
    std::shared_ptr<a::ToolRegistry> registry = std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::PermissionPolicy> policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    a::EngineOptions eo{.decision={.enabled=false}};
    a::SubagentOptions so;
    std::shared_ptr<a::Subagents> agents;
    Host() {
        QDir().mkpath(workspace); eo.sessionsDirectory = root.filePath("parents");
        eo.projectContext.enabled=false; eo.compaction.automatic=false;
        so.workingDirectory=workspace; so.stateDirectory=root.filePath("children");
        a::SubagentDefinition p; p.name="reader"; p.description="Reader";
        p.systemPrompt="READER_SYSTEM"; p.tools={"Read", "Skill"}; p.background=true;
        so.definitions={p}; so.modelAliases={{"reader-model","child-model"}};
    }
    void skill(QByteArray fields = "context: fork\nagent: reader\nmodel: reader-model\n") {
        const auto path=workspace+"/.claude/skills/inspect/SKILL.md"; QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path); check(f.open(QIODevice::WriteOnly),"open skill");
        f.write("---\ndescription: Inspect evidence\n"+fields+"---\nPRIVATE_BODY $0 parent=${CLAUDE_SESSION_ID}\n");
    }
    std::unique_ptr<a::Engine> start(bool attached=true) {
        if(attached) { agents=std::make_shared<a::Subagents>(model,registry,policy,eo,so); a::Subagents::attach(eo,agents); }
        return std::make_unique<a::Engine>(model,registry,policy,eo);
    }
};
}
class SkillForkTests final {
public:
    void fallbackModelAuthorizationAndParentPolicyRemainEnforced() {
        Host h; h.skill("context: fork\nagent: absent\nmodel: inherit\n"); int reads=0;
        h.policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,
            QList<a::PermissionRule>{{"Read",a::PermissionBehavior::Deny},{"Skill",a::PermissionBehavior::Deny}});
        h.registry->add({{"Read","Read file",{{"type","object"}}, {},true},[&](const auto&,const auto&){++reads;return a::ToolResult{"BAD"};}});
        h.model->next=[](const a::ModelRequest& r,const auto&){
            check(r.model=="parent","inherit model lost");
            if(r.messages.last().role!=a::MessageRole::Tool)return a::ModelReply{{},{{"read","Read",{}}}};
            check(r.messages.last().isError,"parent denial bypassed");return a::ModelReply{"DENIED_AS_EXPECTED",{}};
        };
        auto engine=h.start();const auto s=engine->createSession("parent",h.workspace);
        a::RunRequest r{s.id};r.skill="inspect";const auto result=engine->run(r).result.get();
        CTEST_CHECK_MESSAGE(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage)); CTEST_COMPARE(reads,0);
        CTEST_COMPARE(h.agents->list(s.id)[0].toObject()["agent_type"],"reader");
        h.skill("context: fork\nagent: reader\nmodel: unauthorized\n");
        CTEST_COMPARE(engine->run(r).result.get().errorCode,ErrorCode::InvalidArgument); CTEST_COMPARE(h.agents->list(s.id).size(),1);
        h.skill("context: fork\nuser-invocable: false\n");
        CTEST_COMPARE(engine->run(r).result.get().errorCode,ErrorCode::InvalidArgument); CTEST_COMPARE(h.agents->list(s.id).size(),1);
    }
    void directForkLeavesQueuedInputPendingAndHonorsStopVeto() {
        Host h; h.skill(); int stops=0; QString parentId; std::unique_ptr<a::Engine> engine;
        h.eo.hooks.append([&](const a::HookInput& i,const auto&){
            if(i.kind==a::HookKind::SubagentStop) return a::HookResult{++stops==1,"TRY_AGAIN"};
            return a::HookResult{};
        });
        h.model->next=[&](const a::ModelRequest& r,const auto&){
            if(!stops) engine->enqueueInput(parentId,{{"text","QUEUED_PARENT"},{"priority","now"}});
            for(const auto& m:r.messages)check(!m.text.contains("QUEUED_PARENT"),"queued parent input leaked");
            return a::ModelReply{stops?"AFTER_VETO":"BEFORE_VETO",{}};
        };
        engine=h.start(); parentId=engine->createSession("parent",h.workspace).id;
        a::RunRequest r{parentId};r.skill="inspect";const auto result=engine->run(r).result.get();
        CTEST_CHECK_MESSAGE(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage)); CTEST_COMPARE(result.text,"AFTER_VETO");
        CTEST_COMPARE(stops,2); CTEST_COMPARE(result.turns,2); CTEST_COMPARE(engine->queuedInputs(parentId)["count"],1);
    }
    void observerFailureJoinsChildAndDoesNotReplayOnReopen() {
        Host h;h.skill();int calls=0;h.model->next=[&](const auto&,const auto&){++calls;return a::ModelReply{"answer",{}};};
        auto engine=h.start();const auto s=engine->createSession("parent",h.workspace);a::RunRequest r{s.id};r.skill="inspect";
        const auto result=engine->run(r,[](const a::Event& e){if(e.kind==a::EventKind::ToolProgress)throw std::runtime_error("observer");}).result.get();
        CTEST_COMPARE(result.status,a::RunStatus::Failed); CTEST_COMPARE(result.errorCode,ErrorCode::ConsumerFailure); CTEST_COMPARE(calls,0);
        const auto jobs=h.agents->list(s.id);CTEST_COMPARE(jobs.size(),1);CTEST_CHECK(jobs[0].toObject()["finished"].toBool());
        engine.reset();engine=std::make_unique<a::Engine>(h.model,h.registry,h.policy,h.eo);
        CTEST_COMPARE(engine->run({s.id,"continue"}).result.get().status,a::RunStatus::Completed);CTEST_COMPARE(calls,1);CTEST_COMPARE(h.agents->list(s.id).size(),1);
    }
    void catalogRequiresAnExecutorAndKeepsInlineModelUnsupported() {
        Host h; h.skill(); auto catalog=a::discoverSkills(h.workspace);
        CTEST_COMPARE(catalog.skills[0].executionContext,"fork"); CTEST_COMPARE(catalog.skills[0].agent,"reader");
        CTEST_COMPARE(catalog.skills[0].model,"reader-model"); CTEST_CHECK(catalog.skills[0].unsupportedFeatures.isEmpty());
        auto engine=h.start(false); const auto s=engine->createSession("parent",h.workspace);
        CTEST_CHECK(engine->skills(s.id).message().text.isEmpty());
        CTEST_CHECK(engine->skills(s.id).skills[0].unsupportedFeatures.contains("fork-executor-unavailable"));
        a::RunRequest request{s.id}; request.skill="inspect";
        CTEST_COMPARE(engine->run(request).result.get().errorCode,ErrorCode::RuntimeUnavailable);
        CTEST_CHECK(engine->session(s.id).messages.isEmpty());
        h.skill("model: reader-model\n"); CTEST_CHECK(!a::discoverSkills(h.workspace).skills[0].unsupportedFeatures.isEmpty());
    }
    void directInvocationIsIsolatedForegroundAndPreservesParentIdentity() {
        Host h; h.skill(); int calls=0, starts=0, stops=0; QString parentId;
        h.eo.hooks.append([&](const a::HookInput& i,const auto&){
            starts+=i.kind==a::HookKind::SubagentStart; stops+=i.kind==a::HookKind::SubagentStop; return a::HookResult{};
        });
        h.model->next=[&](const a::ModelRequest& r,const auto&){
            ++calls; check(r.contextId!=parentId,"parent model queried"); check(r.model=="child-model","alias not resolved");
            check(r.systemPrompt=="READER_SYSTEM","wrong child system"); check(r.generation.temperature==0.13,"generation lost");
            for(const auto& m:r.messages) check(!m.text.contains("PARENT_SECRET"),"parent history leaked");
            check(r.messages.last().text.contains("PRIVATE_BODY two words parent="+parentId),"wrong expansion context");
            check(r.messages.last().text.contains("ADDITIONAL"),"additional input lost");
            check(r.messages.last().metadata["iilocal.skill"].toObject()["invocation"]=="user","snapshot lost");
            for(const auto& m:r.messages) check(!m.metadata.contains("iilocal.skill_catalog"),"recursive fork advertised");
            a::ModelReply reply{"CHILD_RESULT",{}}; reply.usage.promptTokens=17; reply.usage.generatedTokens=3; return reply;
        };
        auto engine=h.start(); const auto s=engine->createSession("parent",h.workspace,"PARENT_SECRET"); parentId=s.id;
        { a::SessionStore store(h.eo.sessionsDirectory); auto lease=store.acquire(s.id); lease->append({{},a::MessageRole::User,"PARENT_SECRET"}); }
        a::RunRequest request{s.id,"ADDITIONAL"}; request.skill="inspect"; request.skillArguments="'two words'"; request.generation.temperature=0.13;
        const auto handle=engine->run(request); const auto result=handle.result.get();
        CTEST_CHECK_MESSAGE(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage)); CTEST_COMPARE(result.text,"CHILD_RESULT");
        CTEST_COMPARE(result.sessionId,s.id); CTEST_COMPARE(result.runId,handle.runId); CTEST_COMPARE(result.turns,1);
        CTEST_COMPARE(result.usage.promptTokens,17); CTEST_COMPARE(result.usage.generatedTokens,3); CTEST_COMPARE(calls,1); CTEST_COMPARE(starts,1); CTEST_COMPARE(stops,1);
        const auto jobs=h.agents->list(s.id); CTEST_COMPARE(jobs.size(),1);
        const auto child=h.agents->output(s.id,jobs[0].toObject()["agentId"].toString());
        CTEST_COMPARE(child["background"],false); CTEST_COMPARE(child["fork_context"],false); CTEST_CHECK(!child.contains("notification_id"));
        const auto saved=engine->session(s.id); CTEST_COMPARE(saved.messages.size(),3);
        for(const auto& m:saved.messages) CTEST_CHECK(!m.text.contains("PRIVATE_BODY"));
        CTEST_COMPARE(saved.messages.last().metadata["iilocal.skill_fork"].toObject()["agentId"],child["agentId"]);
    }
    void nativeToolReturnsResultWithoutInjectingInstructionsOrReplaying() {
        Host h; h.skill(); QString parentId; int childCalls=0,parentCalls=0;
        h.model->next=[&](const a::ModelRequest& r,const auto&){
            if(r.contextId!=parentId) { ++childCalls; return a::ModelReply{"NATIVE_CHILD",{}}; }
            ++parentCalls;
            for(const auto& m:r.messages) check(!m.text.contains("PRIVATE_BODY"),"inline injection");
            if(r.messages.last().text=="invoke") return a::ModelReply{{},{{"skill-1","Skill",{{"skill","inspect"},{"args","evidence"}}}}};
            if(r.messages.last().role==a::MessageRole::Tool) {
                const auto& m=r.messages.last(); check(!m.isError,"fork failed"); check(m.data["status"]=="forked","wrong result status");
                check(m.data["result"]=="NATIVE_CHILD","missing actual child result");
                check(!m.metadata.contains("iilocal.skill_result"),"inline recovery marker");
            }
            return a::ModelReply{"PARENT_RESULT",{}};
        };
        auto engine=h.start(); const auto s=engine->createSession("parent",h.workspace); parentId=s.id;
        const auto result=engine->run({s.id,"invoke"}).result.get(); CTEST_CHECK_MESSAGE(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage));
        CTEST_COMPARE(result.text,"PARENT_RESULT"); CTEST_COMPARE(childCalls,1); CTEST_COMPARE(parentCalls,2); CTEST_COMPARE(engine->session(s.id).messages.size(),4);
        engine.reset(); h.skill("context: fork\nagent: reader\n"); engine=std::make_unique<a::Engine>(h.model,h.registry,h.policy,h.eo);
        CTEST_COMPARE(engine->run({s.id,"continue"}).result.get().status,a::RunStatus::Completed); CTEST_COMPARE(childCalls,1);
        CTEST_CHECK(a::pendingToolCalls(engine->session(s.id).messages).isEmpty());
    }
    void childFailureAndTurnLimitAreNotReportedCompleted() {
        for(bool limited:{false,true}) {
            Host h; h.skill(); h.model->next=[limited](const auto&,const auto&)->a::ModelReply{
                if(!limited) throw Error(ErrorCode::RuntimeFailure,"CHILD_FAILED");
                return {{},{{"missing","Read",{}}}};
            };
            auto engine=h.start(); const auto s=engine->createSession("parent",h.workspace);
            a::RunRequest r{s.id}; r.skill="inspect"; r.maxTurns=1; const auto result=engine->run(r).result.get();
            CTEST_COMPARE(result.status,limited?a::RunStatus::TurnLimit:a::RunStatus::Failed);
            if(!limited) { CTEST_COMPARE(result.errorCode,ErrorCode::RuntimeFailure); CTEST_CHECK(result.errorMessage.contains("CHILD_FAILED")); }
            CTEST_COMPARE(h.agents->list(s.id).size(),1);
        }
    }
    void cancellationAndDeadlineJoinTheChild() {
        for(bool deadline:{false,true}) {
            Host h; h.skill(); std::atomic<bool> entered=false;
            if(deadline) h.so.maxRuntimeMs=50;
            h.model->next=[&](const auto&,const auto& token)->a::ModelReply{
                entered=true; while(true){ token.throwIfCancelled(); std::this_thread::sleep_for(2ms); }
            };
            auto engine=h.start(); const auto s=engine->createSession("parent",h.workspace);
            a::RunRequest r{s.id}; r.skill="inspect"; const auto handle=engine->run(r);
            if(!deadline) { CTEST_WAIT_VERIFY(entered.load(),2000); handle.cancel(); }
            CTEST_CHECK(handle.result.wait_for(3s)==std::future_status::ready); const auto result=handle.result.get();
            CTEST_COMPARE(result.status,deadline?a::RunStatus::Failed:a::RunStatus::Cancelled);
            CTEST_COMPARE(result.errorCode,deadline?ErrorCode::Timeout:ErrorCode::Cancelled);
            const auto jobs=h.agents->list(s.id); CTEST_COMPARE(jobs.size(),1); CTEST_CHECK(jobs[0].toObject()["finished"].toBool());
        }
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner;
    SkillForkTests tests;
    runner.run("fallbackModelAuthorizationAndParentPolicyRemainEnforced", [&] { tests.fallbackModelAuthorizationAndParentPolicyRemainEnforced(); });
    runner.run("directForkLeavesQueuedInputPendingAndHonorsStopVeto", [&] { tests.directForkLeavesQueuedInputPendingAndHonorsStopVeto(); });
    runner.run("observerFailureJoinsChildAndDoesNotReplayOnReopen", [&] { tests.observerFailureJoinsChildAndDoesNotReplayOnReopen(); });
    runner.run("catalogRequiresAnExecutorAndKeepsInlineModelUnsupported", [&] { tests.catalogRequiresAnExecutorAndKeepsInlineModelUnsupported(); });
    runner.run("directInvocationIsIsolatedForegroundAndPreservesParentIdentity", [&] { tests.directInvocationIsIsolatedForegroundAndPreservesParentIdentity(); });
    runner.run("nativeToolReturnsResultWithoutInjectingInstructionsOrReplaying", [&] { tests.nativeToolReturnsResultWithoutInjectingInstructionsOrReplaying(); });
    runner.run("childFailureAndTurnLimitAreNotReportedCompleted", [&] { tests.childFailureAndTurnLimitAreNotReportedCompleted(); });
    runner.run("cancellationAndDeadlineJoinTheChild", [&] { tests.cancellationAndDeadlineJoinTheChild(); });
    return runner.result();
}

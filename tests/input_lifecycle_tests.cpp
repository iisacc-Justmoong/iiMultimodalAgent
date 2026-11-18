#include "iiMultimodalAgent/agent/Engine.h"
#include "iiMultimodalAgent/agent/CommandHooks.h"
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
namespace {
class Model final:public a::Model {
public:
    QList<a::ModelRequest> requests;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken&,const TextCallback&) override {
        requests.append(request);return {request.summarizing?"Clean summary":"ANSWER",{}};
    }
    std::optional<ContextBudget> measure(const a::ModelRequest& request,const CancellationToken&) override {
        ContextBudget budget;budget.contextTokens=4096;budget.inputTokens=32;
        for(const auto& message:request.messages)budget.inputTokens+=message.text.size()/4+4;
        return budget;
    }
};
struct Host {
    QTemporaryDir root;
    QString workspace=root.filePath("work");
    std::shared_ptr<Model> model=std::make_shared<Model>();
    a::EngineOptions options{.decision={.enabled=false}};
    Host(){QDir().mkpath(workspace);options.sessionsDirectory=root.filePath("sessions");options.compaction.automatic=false;}
    std::unique_ptr<a::Engine> engine(){return std::make_unique<a::Engine>(model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(),options);}
};
QString modelText(const a::ModelRequest& request) {
    QString text;for(const auto& message:request.messages)text+=message.text+'\n';return text;
}
}
class InputLifecycleTests final {
public:
    void sessionInitialInputHonorsTheSmallerHostInputLimit() {
        Host host;host.options.maxInputCharacters=32;
        host.options.hooks.append([](const a::HookInput& input,const CancellationToken&) {
            a::HookResult result;if(input.kind==a::HookKind::SessionStart)result.initialUserMessage=QString(33,'x');return result;
        });
        auto engine=host.engine();const auto id=engine->createSession("model://test",host.workspace).id;
        const auto result=engine->run({id,"small"}).result.get();
        CTEST_COMPARE(result.errorCode,ErrorCode::ResourceLimit);CTEST_COMPARE(result.status,a::RunStatus::Failed);
        CTEST_CHECK(host.model->requests.isEmpty());CTEST_COMPARE(engine->queuedInputs(id)["count"].toInt(),0);
    }
    void directBlockDoesNotReachModelsAndStopPreservesThePrompt() {
        Host host;QStringList submitted;
        host.options.hooks.append([&](const a::HookInput& input,const CancellationToken&) {
            a::HookResult result;if(input.kind!=a::HookKind::UserPromptSubmit)return result;
            submitted.append(input.text);
            if(input.text=="FORBIDDEN_INPUT"){result.block=true;result.feedback="PROMPT_REJECTED";}
            else if(input.text=="STOPPED_INPUT"){result.stop=true;result.stopReason="PROMPT_STOPPED";}
            else result.feedback="HOOK_CONTEXT";
            return result;
        });
        auto engine=host.engine();const auto id=engine->createSession("model://test",host.workspace).id;
        a::RunRequest forbidden{id,"FORBIDDEN_INPUT"};forbidden.contextPaths={"rejected-scope.txt"};
        auto rejected=engine->run(forbidden).result.get();
        CTEST_COMPARE(rejected.status,a::RunStatus::Failed);CTEST_COMPARE(rejected.errorCode,ErrorCode::InvalidArgument);CTEST_CHECK(rejected.errorMessage.contains("PROMPT_REJECTED"));
        CTEST_CHECK(host.model->requests.isEmpty());CTEST_COMPARE(engine->session(id).messages.size(),1);
        CTEST_CHECK(a::projectContextPaths(engine->session(id).messages).isEmpty());
        CTEST_COMPARE(engine->run({id,"safe input"}).result.get().status,a::RunStatus::Completed);
        CTEST_CHECK(modelText(host.model->requests.last()).contains("HOOK_CONTEXT"));CTEST_CHECK(!modelText(host.model->requests.last()).contains("FORBIDDEN_INPUT"));
        const auto stopped=engine->run({id,"STOPPED_INPUT"}).result.get();CTEST_COMPARE(stopped.status,a::RunStatus::Cancelled);CTEST_CHECK(stopped.errorMessage.contains("PROMPT_STOPPED"));
        CTEST_COMPARE(host.model->requests.size(),1);
        CTEST_COMPARE(engine->run({id,"continue"}).result.get().status,a::RunStatus::Completed);
        CTEST_CHECK(modelText(host.model->requests.last()).contains("STOPPED_INPUT"));
        CTEST_COMPARE(submitted,(QStringList{"FORBIDDEN_INPUT","safe input","STOPPED_INPUT","continue"}));
    }
    void queuedDispositionSurvivesAnUnacknowledgedTranscriptCommit() {
        for(const auto disposition:{"accepted","blocked","stopped"}) {
            Host host;int submitted=0;
            host.options.hooks.append([&](const a::HookInput& input,const CancellationToken&) {
                submitted+=input.kind==a::HookKind::UserPromptSubmit;return a::HookResult{};
            });
            auto engine=host.engine();const auto id=engine->createSession("model://test",host.workspace).id;
            const auto input=engine->enqueueInput(id,{{"text","COMMITTED_INPUT"}})["input"].toObject();
            {
                a::SessionStore store(host.options.sessionsDirectory);auto lease=store.acquire(id);
                a::Message message{input["id"].toString(),a::MessageRole::User,input["text"].toString()};
                message.metadata={{"iilocal.input",input},{"iilocal.user_prompt_hook",QJsonObject{{"version",1},
                    {"disposition",disposition},{"reason","SAVED_REASON"},{"context","SAVED_CONTEXT"}}}};
                lease->append(message);
            }
            engine.reset();engine=host.engine();const auto result=engine->runQueued({id,{}}).result.get();
            const bool accepted=QString(disposition)=="accepted";
            CTEST_COMPARE(result.status,accepted?a::RunStatus::Completed:QString(disposition)=="blocked"?a::RunStatus::Failed:a::RunStatus::Cancelled);
            CTEST_COMPARE(submitted,0);CTEST_COMPARE(engine->queuedInputs(id)["count"].toInt(),0);
            CTEST_COMPARE(host.model->requests.size(),accepted?1:0);
            if(accepted)CTEST_CHECK(modelText(host.model->requests.last()).contains("SAVED_CONTEXT"));
            else CTEST_CHECK(result.errorMessage.contains("SAVED_REASON"));
        }
    }
    void urgentInputCancelsPreparationAndRetriesThePendingPrompt() {
        Host host;std::atomic_bool preparing=false;int originalCalls=0;
        host.options.hooks.append([&](const a::HookInput& input,const CancellationToken& token) {
            if(input.kind==a::HookKind::UserPromptSubmit&&input.text=="original"&&++originalCalls==1) {
                preparing=true;
                while(!token.isCancelled())std::this_thread::sleep_for(std::chrono::milliseconds(1));
                token.throwIfCancelled();
            }
            return a::HookResult{};
        });
        auto engine=host.engine();const auto id=engine->createSession("model://test",host.workspace).id;
        engine->enqueueInput(id,{{"text","original"}});auto run=engine->runQueued({id,{}});
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(!preparing&&std::chrono::steady_clock::now()<deadline)agent_ctest::processFor(1);
        if(!preparing){run.cancel();agent_ctest::fail(__FILE__, __LINE__, "Prompt preparation did not start");}
        engine->enqueueInput(id,{{"text","urgent"},{"priority","now"}});
        const auto ready=run.result.wait_for(std::chrono::seconds(3));if(ready!=std::future_status::ready)run.cancel();
        CTEST_CHECK(ready==std::future_status::ready);const auto result=run.result.get();
        CTEST_CHECK_MESSAGE(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage));
        CTEST_COMPARE(originalCalls,2);CTEST_COMPARE(host.model->requests.size(),1);CTEST_COMPARE(engine->queuedInputs(id)["count"].toInt(),0);
        const auto messages=engine->session(id).messages;CTEST_COMPARE(messages[0].text,"urgent");CTEST_COMPARE(messages[1].text,"original");
    }
    void directSkillsSubmitRawInputAndChildLifecycleIsDistinct() {
        for(const bool fork:{false,true}) {
            Host host;const auto path=host.workspace+"/.claude/skills/inspect/SKILL.md";
            CTEST_CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));QFile file(path);CTEST_CHECK(file.open(QIODevice::WriteOnly));
            file.write("---\ndescription: Inspect\n");if(fork)file.write("context: fork\nagent: general-purpose\n");
            file.write("---\nEXPANDED_BODY $ARGUMENTS\n");file.close();
            QStringList prompts;int starts=0,childStarts=0;
            host.options.hooks.append([&](const a::HookInput& input,const CancellationToken&) {
                starts+=input.kind==a::HookKind::SessionStart;childStarts+=input.kind==a::HookKind::SubagentStart;
                a::HookResult result;if(input.kind==a::HookKind::UserPromptSubmit) {
                    prompts.append(input.text);result.block=input.text=="/inspect denied";result.feedback=result.block?"REJECTED_SKILL":"SKILL_HOOK_CONTEXT";
                }return result;
            });
            auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>();
            a::SubagentOptions options;options.workingDirectory=host.workspace;options.stateDirectory=host.root.filePath("children");
            auto children=std::make_shared<a::Subagents>(host.model,registry,policy,host.options,options);a::Subagents::attach(host.options,children);
            a::Engine engine(host.model,registry,policy,host.options);const auto id=engine.createSession("model://test",host.workspace).id;
            a::RunRequest request{id};request.skill=fork?"/inspect":"inspect";request.skillArguments="denied";
            CTEST_COMPARE(engine.run(request).result.get().status,a::RunStatus::Failed);CTEST_CHECK(host.model->requests.isEmpty());CTEST_COMPARE(childStarts,0);
            request.skillArguments="allowed";request.prompt="extra request";
            const auto result=engine.run(request).result.get();CTEST_CHECK_MESSAGE(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage));
            CTEST_COMPARE(prompts,(QStringList{"/inspect denied","/inspect allowed\n\nextra request"}));
            CTEST_COMPARE(starts,1);CTEST_COMPARE(childStarts,fork?1:0);CTEST_COMPARE(host.model->requests.size(),1);
            const auto context=modelText(host.model->requests.last());CTEST_CHECK(context.contains("EXPANDED_BODY allowed"));
            CTEST_COMPARE(context.count("SKILL_HOOK_CONTEXT"),1);CTEST_CHECK(!context.contains("EXPANDED_BODY denied"));
            CTEST_COMPARE(engine.session(id).messages[0].metadata["iilocal.user_prompt_hook"].toObject()["submitted_prompt"],"/inspect denied");
        }
    }
    void queuedHooksCanPublishAndBlockedInputIsAcknowledgedOnce() {
        Host host;std::unique_ptr<a::Engine> engine;QStringList submitted;
        host.options.hooks.append([&](const a::HookInput& input,const CancellationToken&) {
            a::HookResult result;if(input.kind!=a::HookKind::UserPromptSubmit)return result;
            submitted.append(input.text);
            if(input.text=="blocked queue") {
                if(engine->queuedInputs(input.sessionId)["count"].toInt()!=1)throw std::runtime_error("Queue was not inspectable");
                engine->enqueueInput(input.sessionId,{{"text","published from hook"}});
                result.block=true;result.feedback="BLOCK_QUEUE";
            }
            return result;
        });
        engine=host.engine();const auto id=engine->createSession("model://test",host.workspace).id;
        engine->enqueueInput(id,{{"text","blocked queue"}});
        const auto blocked=engine->runQueued({id,{}}).result.get();CTEST_COMPARE(blocked.status,a::RunStatus::Failed);CTEST_CHECK(blocked.errorMessage.contains("BLOCK_QUEUE"));
        CTEST_CHECK(host.model->requests.isEmpty());CTEST_COMPARE(engine->queuedInputs(id)["count"].toInt(),1);
        CTEST_COMPARE(engine->runQueued({id,{}}).result.get().status,a::RunStatus::Completed);
        CTEST_COMPARE(submitted,(QStringList{"blocked queue","published from hook"}));CTEST_CHECK(!modelText(host.model->requests.last()).contains("blocked queue"));
        engine->enqueueInput(id,{{"text","notification data"},{"kind","notification"}});
        CTEST_COMPARE(engine->runQueued({id,{}}).result.get().status,a::RunStatus::Completed);CTEST_COMPARE(submitted.size(),2);
    }
    void sessionStartRunsOnActivationResumeAndCompactionWithoutVeto() {
        Host host;QStringList sources;int prompts=0;
        host.options.hooks.append([&](const a::HookInput& input,const CancellationToken&) {
            a::HookResult result;
            if(input.kind==a::HookKind::SessionStart) {
                sources.append(input.context["source"].toString());result.feedback="START_CONTEXT";result.block=true;result.stop=true;
                if(input.context["source"]=="startup")result.initialUserMessage="INITIAL_FROM_HOOK";
            }
            if(input.kind==a::HookKind::UserPromptSubmit)++prompts;
            return result;
        });
        auto engine=host.engine();const auto id=engine->createSession("model://test",host.workspace).id;
        CTEST_COMPARE(engine->run({id,"first input "+QString(1200,'a')}).result.get().status,a::RunStatus::Completed);
        CTEST_COMPARE(sources,(QStringList{"startup"}));CTEST_COMPARE(prompts,2);
        CTEST_CHECK(modelText(host.model->requests.last()).contains("START_CONTEXT"));CTEST_CHECK(modelText(host.model->requests.last()).contains("INITIAL_FROM_HOOK"));
        CTEST_COMPARE(engine->run({id,"second input "+QString(1200,'b')}).result.get().status,a::RunStatus::Completed);CTEST_COMPARE(sources.size(),1);
        engine.reset();engine=host.engine();CTEST_COMPARE(engine->run({id,"resume input"}).result.get().status,a::RunStatus::Completed);
        CTEST_COMPARE(sources,(QStringList{"startup","resume"}));
        a::CompactRequest compact;compact.sessionId=id;compact.generation.maxTokens=64;
        const auto result=engine->compact(compact).result.get();CTEST_CHECK_MESSAGE(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage));
        CTEST_COMPARE(sources,(QStringList{"startup","resume","compact"}));
    }
    void rejectedInputStaysOutOfCompactionAndForkViews() {
        Host host;host.options.compaction.keepRecentGroups=1;
        host.options.hooks.append([](const a::HookInput& input,const CancellationToken&) {
            return input.kind==a::HookKind::UserPromptSubmit&&input.text=="NEVER_TO_MODEL"?a::HookResult{true,"blocked"}:a::HookResult{};
        });
        auto engine=host.engine();const auto id=engine->createSession("model://test",host.workspace).id;
        for(int n=0;n<4;++n)CTEST_COMPARE(engine->run({id,"safe "+QString::number(n)+QString(1024,'a')}).result.get().status,a::RunStatus::Completed);
        CTEST_COMPARE(engine->run({id,"NEVER_TO_MODEL"}).result.get().status,a::RunStatus::Failed);
        a::CompactRequest compact;compact.sessionId=id;compact.generation.maxTokens=64;
        const auto result=engine->compact(compact).result.get();CTEST_CHECK_MESSAGE(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage));
        for(const auto& request:host.model->requests)CTEST_CHECK(!modelText(request).contains("NEVER_TO_MODEL"));
        const auto fork=engine->forkSession(id);CTEST_COMPARE(engine->run({fork.id,"fork continuation"}).result.get().status,a::RunStatus::Completed);
        CTEST_CHECK(!modelText(host.model->requests.last()).contains("NEVER_TO_MODEL"));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner;
    InputLifecycleTests tests;
    runner.run("sessionInitialInputHonorsTheSmallerHostInputLimit", [&] { tests.sessionInitialInputHonorsTheSmallerHostInputLimit(); });
    runner.run("directBlockDoesNotReachModelsAndStopPreservesThePrompt", [&] { tests.directBlockDoesNotReachModelsAndStopPreservesThePrompt(); });
    runner.run("queuedDispositionSurvivesAnUnacknowledgedTranscriptCommit", [&] { tests.queuedDispositionSurvivesAnUnacknowledgedTranscriptCommit(); });
    runner.run("urgentInputCancelsPreparationAndRetriesThePendingPrompt", [&] { tests.urgentInputCancelsPreparationAndRetriesThePendingPrompt(); });
    runner.run("directSkillsSubmitRawInputAndChildLifecycleIsDistinct", [&] { tests.directSkillsSubmitRawInputAndChildLifecycleIsDistinct(); });
    runner.run("queuedHooksCanPublishAndBlockedInputIsAcknowledgedOnce", [&] { tests.queuedHooksCanPublishAndBlockedInputIsAcknowledgedOnce(); });
    runner.run("sessionStartRunsOnActivationResumeAndCompactionWithoutVeto", [&] { tests.sessionStartRunsOnActivationResumeAndCompactionWithoutVeto(); });
    runner.run("rejectedInputStaysOutOfCompactionAndForkViews", [&] { tests.rejectedInputStaysOutOfCompactionAndForkViews(); });
    return runner.result();
}

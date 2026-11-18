#include "iiMultimodalAgent/agent/Subagents.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include "CTestSupport.h"
#include <QCoreApplication>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a = iiMultimodalAgent::agent;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
class Model final : public a::Model {
public:
    std::function<a::ModelReply(const a::ModelRequest&)> next;
    bool budget=false;
    std::optional<ContextBudget> measure(const a::ModelRequest& r,const CancellationToken&) override {
        if(!budget)return {};qint64 size=100;for(const auto& m:r.messages)size+=m.text.size()+10;return ContextBudget{size,4096};
    }
    a::ModelReply generate(const a::ModelRequest& r, const CancellationToken&, const TextCallback&) override {
        auto reply = next(r); for (auto& c : reply.toolCalls) c.id = QUuid::createUuid().toString(QUuid::WithoutBraces); return reply;
    }
};
struct Host {
    QTemporaryDir root;
    QString workspace = root.filePath("workspace");
    std::shared_ptr<Model> model = std::make_shared<Model>();
    std::shared_ptr<a::ToolRegistry> registry = std::make_shared<a::ToolRegistry>();
    a::EngineOptions options{.decision={.enabled=false}};
    int writes = 0;
    Host() {
        QDir().mkpath(workspace);
        workspace = QFileInfo(workspace).canonicalFilePath();
        options.sessionsDirectory = root.filePath("sessions"); options.compaction.automatic = false; options.projectContext.enabled = false;
        registry->add({{"Write", "Write evidence", {{"type", "object"}}}, [this](const auto&, const auto&) { ++writes; return a::ToolResult{"WRITTEN"}; }});
        model->next = [](const auto& r) -> a::ModelReply {
            if (r.messages.last().role == a::MessageRole::Tool) return {r.messages.last().isError ? "DENIED" : "ALLOWED", {}};
            if (r.messages.last().text == "invoke") return {{}, {{"skill", "Skill", {{"skill", "writer"}}}, {"write", "Write", {{"path", "out.txt"}}}}};
            if (r.messages.last().metadata.contains("iilocal.skill_parent")) return {"DONE", {}};
            return {{}, {{"write", "Write", {{"path", "out.txt"}}}}};
        };
    }
    void skill(QByteArray fields = "allowed-tools: [Write]\n", QByteArray body = "ORIGINAL") {
        const auto path = workspace + "/.claude/skills/writer/SKILL.md"; QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path); check(f.open(QIODevice::WriteOnly), "open skill"); f.write("---\ndescription: Write a file\n" + fields + "---\n" + body);
    }
};
}
class SkillPermissionTests final {
public:
    void directGrantExpiresAcrossRunsForksAndRestart() {
        Host h; h.skill(); auto policy = std::make_shared<a::RulePolicy>();
        QString session, fork;
        {
            a::Engine engine(h.model,h.registry,policy,h.options); session=engine.createSession("fixture",h.workspace).id;
            a::RunRequest r{session}; r.skill="writer";
            CTEST_COMPARE(engine.run(r).result.get().text,"ALLOWED"); CTEST_COMPARE(h.writes,1);
            fork=engine.forkSession(session).id;
            CTEST_COMPARE(engine.run({session,"again"}).result.get().text,"DENIED");
            CTEST_COMPARE(engine.run({fork,"again"}).result.get().text,"DENIED"); CTEST_COMPARE(h.writes,1);
        }
        a::Engine reopened(h.model,h.registry,policy,h.options);
        CTEST_COMPARE(reopened.run({session,"after restart"}).result.get().text,"DENIED"); CTEST_COMPARE(h.writes,1);
    }
    void nativeInvocationNeedsPermissionAndAppliesInSameBatch() {
        for (bool allow : {false,true}) {
            Host h; h.skill(); int asks=0; QJsonObject preview;
            h.options.permission=[&](const auto& call,const auto& decision,const auto& context){
                ++asks; check(context.allowedTools.isEmpty(),"proposed grant already active");
                if(call.name=="Skill") { check(decision.reason.contains("Write"),"no permission detail"); return allow; }
                return false;
            };
            a::Engine engine(h.model,h.registry,std::make_shared<a::RulePolicy>(),h.options);
            const auto id=engine.createSession("fixture",h.workspace).id;
            const auto result=engine.run({id,"invoke"},[&](const a::Event& e){if(e.kind==a::EventKind::PermissionRequested&&e.data["name"]=="Skill")preview=e.data;}).result.get();
            CTEST_COMPARE(result.status,a::RunStatus::Completed); CTEST_COMPARE(h.writes,allow?1:0); CTEST_COMPARE(asks,allow?1:2);
            CTEST_CHECK(preview["permission_preview"].toObject()["_meta"].toObject()["skill"].toObject()["allowed_tools"].toArray().contains("Write"));
        }
    }
    void approvedSnapshotSurvivesFileChangeAndHookRetargeting() {
        Host h; h.skill(); int asks=0;
        const auto originalNext=h.model->next;h.model->next=[originalNext](const auto& r){auto reply=originalNext(r);
            for(auto& call:reply.toolCalls)if(call.name=="Skill")call.arguments["skill"]="absent-before-hook";return reply;};
        h.options.hooks.append([](const a::HookInput& i,const auto&){a::HookResult r;
            if(i.kind==a::HookKind::BeforeTool&&i.call.name=="Skill")r.updatedArguments=QJsonObject{{"skill","writer"}};return r;});
        h.options.permission=[&](const auto& call,const auto& decision,const auto&){
            if(call.name!="Skill") return false; ++asks; check(call.arguments["skill"]=="writer","permission used original hook arguments");
            check(decision.reason.contains("Write"),"missing exact grant"); h.skill("allowed-tools: [Bash]\n","CHANGED"); return true;
        };
        a::Engine engine(h.model,h.registry,std::make_shared<a::RulePolicy>(),h.options);const auto id=engine.createSession("fixture",h.workspace).id;
        CTEST_COMPARE(engine.run({id,"invoke"}).result.get().status,a::RunStatus::Completed); CTEST_COMPARE(asks,1);CTEST_COMPARE(h.writes,1);
        bool found=false;
        for(const auto& m:engine.session(id).messages)if(m.metadata.contains("iilocal.skill_parent")){
            found=true; CTEST_CHECK(m.text.contains("ORIGINAL")); CTEST_CHECK(!m.text.contains("CHANGED"));
            CTEST_COMPARE(m.metadata["iilocal.skill"].toObject()["allowed_tools"].toArray(),QJsonArray{"Write"});
        }
        CTEST_CHECK(found);
    }
    void denyAskPlanAndPostHookVetoRemainAuthoritative() {
        for(int scenario=0;scenario<4;++scenario){
            Host h;h.skill();QList<a::PermissionRule> rules{{"Skill(writer)",a::PermissionBehavior::Allow}};
            if(scenario<2)rules.append({"Write",scenario==0?a::PermissionBehavior::Deny:a::PermissionBehavior::Ask});
            if(scenario==3)h.options.hooks.append([](const a::HookInput& i,const auto&){a::HookResult r;r.block=i.kind==a::HookKind::AfterTool&&i.call.name=="Skill";return r;});
            auto mode=scenario==2?a::PermissionMode::Plan:a::PermissionMode::Default;
            a::Engine engine(h.model,h.registry,std::make_shared<a::RulePolicy>(mode,rules),h.options);const auto id=engine.createSession("fixture",h.workspace).id;
            CTEST_COMPARE(engine.run({id,"invoke"}).result.get().status,a::RunStatus::Completed);CTEST_COMPARE(h.writes,0);
        }
    }
    void forkGrantIsChildOnlyAndResumeUsesCurrentCallerScope() {
        Host h;h.skill("context: fork\nagent: writer\nallowed-tools: [Write]\n");
        a::SubagentOptions sub;sub.stateDirectory=h.root.filePath("children");sub.workingDirectory=h.workspace;
        a::SubagentDefinition profile;profile.name="writer";profile.description="Writer";profile.tools={"Write","Skill"};sub.definitions={profile};
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{{"Skill(writer)",a::PermissionBehavior::Allow}});
        auto agents=std::make_shared<a::Subagents>(h.model,h.registry,policy,h.options,sub);a::Subagents::attach(h.options,agents);
        a::Engine engine(h.model,h.registry,policy,h.options);const auto id=engine.createSession("fixture",h.workspace).id;
        CTEST_COMPARE(engine.run({id,"invoke"}).result.get().status,a::RunStatus::Completed);CTEST_COMPARE(h.writes,1);
        auto jobs=agents->list(id);CTEST_COMPARE(jobs.size(),1);
        a::ToolContext context{id,"resume",h.workspace};context.sessionSnapshot=std::make_shared<a::Session>(engine.session(id));
        const auto resumed=agents->run(context,{{"resume",jobs[0].toObject()["agentId"]},{"prompt","again"}});
        CTEST_CHECK(!resumed.isError);CTEST_COMPARE(h.writes,1);CTEST_COMPARE(resumed.data["result"].toObject()["text"],"DENIED");
    }
    void metadataSyntaxAndUnsupportedArgumentsFailClosed() {
        Host h;h.skill("allowed-tools: 'Read, Bash(git status:*) Write(src/**)'\n");
        const auto info=a::discoverSkills(h.workspace).skills[0];CTEST_CHECK(info.unsupportedFeatures.isEmpty());
        CTEST_COMPARE(info.toJson()["allowed_tools"].toArray(),QJsonArray({"Read","Bash(git status:*)","Write(src/**)"}));
        h.skill("allowed-tools: 'Bash(unclosed'\n");CTEST_THROWS(Error,a::discoverSkills(h.workspace));
    }
    void queuedPromptsResetSkillGrantsButNotificationsDoNot() {
        for(bool notification:{false,true}) {
            Host h;h.skill();a::Engine engine(h.model,h.registry,std::make_shared<a::RulePolicy>(),h.options);
            const auto id=engine.createSession("fixture",h.workspace).id;bool sent=false;a::RunRequest request{id};request.skill="writer";
            const auto result=engine.run(request,[&](const a::Event& event){
                if(!sent&&event.kind==a::EventKind::ToolFinished){sent=true;engine.enqueueInput(id,{{"kind",notification?"notification":"prompt"},{"priority","next"},{"text","again"}});}
            }).result.get();
            CTEST_COMPARE(result.status,a::RunStatus::Completed);CTEST_COMPARE(h.writes,notification?2:1);
            CTEST_COMPARE(result.text,notification?"ALLOWED":"DENIED");
        }
    }
    void recoveredPromptDoesNotRestoreAuthority() {
        Host h;h.skill();auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{{"Skill(writer)",a::PermissionBehavior::Allow}});
        QString id;
        {
            a::Engine engine(h.model,h.registry,policy,h.options);id=engine.createSession("fixture",h.workspace).id;
            const auto result=engine.run({id,"invoke"},[](const a::Event& e){
                if(e.kind==a::EventKind::Message&&e.data["metadata"].toObject().contains("iilocal.skill_result"))throw std::runtime_error("observer stopped");
            }).result.get();
            CTEST_COMPARE(result.errorCode,ErrorCode::ConsumerFailure);CTEST_COMPARE(h.writes,0);
        }
        a::Engine reopened(h.model,h.registry,policy,h.options);
        CTEST_COMPARE(reopened.run({id,"continue"}).result.get().text,"DENIED");CTEST_COMPARE(h.writes,0);
        int injected=0;for(const auto& m:reopened.session(id).messages)injected+=m.metadata.contains("iilocal.skill_parent");CTEST_COMPARE(injected,1);
    }
    void ordinaryChildrenInheritCurrentScopeButPreloadsAndResumeDoNotGrant() {
        Host h;h.skill();auto policy=std::make_shared<a::RulePolicy>();
        a::SubagentOptions sub;sub.workingDirectory=h.workspace;sub.stateDirectory=h.root.filePath("children");
        a::SubagentDefinition p;p.name="writer";p.description="Writer";p.tools={"Write","Skill"};p.skills={"writer"};sub.definitions={p};
        auto agents=std::make_shared<a::Subagents>(h.model,h.registry,policy,h.options,sub);a::Subagents::attach(h.options,agents);
        a::Engine engine(h.model,h.registry,policy,h.options);const auto session=engine.createSession("fixture",h.workspace);
        a::ToolContext context{session.id,"launch",h.workspace};context.sessionSnapshot=std::make_shared<a::Session>(session);
        const QJsonObject args{{"subagent_type","writer"},{"prompt","write once"}};
        auto first=agents->run(context,args);CTEST_CHECK(!first.isError);CTEST_COMPARE(h.writes,0); // Preloaded body alone grants nothing.
        context.allowedTools={"Write"};auto background=args;background["run_in_background"]=true;
        auto second=agents->run(context,background);CTEST_CHECK(!second.isError);context.allowedTools.clear();
        const auto done=agents->output(session.id,second.data["agentId"].toString(),true,10000);
        CTEST_COMPARE(done["status"],"completed");CTEST_COMPARE(h.writes,1); // Accepted background scope was frozen.
        const auto resumed=agents->run(context,{{"resume",second.data["agentId"]},{"prompt","write again"}});
        CTEST_CHECK(!resumed.isError);CTEST_COMPARE(h.writes,1);CTEST_COMPARE(resumed.data["result"].toObject()["text"],"DENIED");
    }
    void automaticCompactionPreservesOnlyCurrentRunGrants() {
        Host h;h.skill();h.model->budget=true;h.options.compaction.automatic=true;h.options.compaction.keepRecentGroups=1;
        a::Engine engine(h.model,h.registry,std::make_shared<a::RulePolicy>(),h.options);const auto id=engine.createSession("fixture",h.workspace).id;
        {a::SessionStore store(h.options.sessionsDirectory);auto lease=store.acquire(id);
            lease->append({{},a::MessageRole::User,"old request"});lease->append({{},a::MessageRole::Assistant,{},{{"old-call","Read",{}}}});
            lease->append({{},a::MessageRole::Tool,QString(3000,'x'),{},"old-call"});lease->append({{},a::MessageRole::Assistant,"old result"});}
        a::RunRequest request{id};request.skill="writer";request.generation.maxTokens=256;
        const auto result=engine.run(request).result.get();CTEST_CHECK_MESSAGE(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage));
        CTEST_COMPARE(result.usage.compactions,1);CTEST_COMPARE(h.writes,1);
        CTEST_COMPARE(engine.run({id,"after compaction"}).result.get().text,"DENIED");CTEST_COMPARE(h.writes,1);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner;
    SkillPermissionTests tests;
    runner.run("directGrantExpiresAcrossRunsForksAndRestart", [&] { tests.directGrantExpiresAcrossRunsForksAndRestart(); });
    runner.run("nativeInvocationNeedsPermissionAndAppliesInSameBatch", [&] { tests.nativeInvocationNeedsPermissionAndAppliesInSameBatch(); });
    runner.run("approvedSnapshotSurvivesFileChangeAndHookRetargeting", [&] { tests.approvedSnapshotSurvivesFileChangeAndHookRetargeting(); });
    runner.run("denyAskPlanAndPostHookVetoRemainAuthoritative", [&] { tests.denyAskPlanAndPostHookVetoRemainAuthoritative(); });
    runner.run("forkGrantIsChildOnlyAndResumeUsesCurrentCallerScope", [&] { tests.forkGrantIsChildOnlyAndResumeUsesCurrentCallerScope(); });
    runner.run("metadataSyntaxAndUnsupportedArgumentsFailClosed", [&] { tests.metadataSyntaxAndUnsupportedArgumentsFailClosed(); });
    runner.run("queuedPromptsResetSkillGrantsButNotificationsDoNot", [&] { tests.queuedPromptsResetSkillGrantsButNotificationsDoNot(); });
    runner.run("recoveredPromptDoesNotRestoreAuthority", [&] { tests.recoveredPromptDoesNotRestoreAuthority(); });
    runner.run("ordinaryChildrenInheritCurrentScopeButPreloadsAndResumeDoNotGrant", [&] { tests.ordinaryChildrenInheritCurrentScopeButPreloadsAndResumeDoNotGrant(); });
    runner.run("automaticCompactionPreservesOnlyCurrentRunGrants", [&] { tests.automaticCompactionPreservesOnlyCurrentRunGrants(); });
    return runner.result();
}

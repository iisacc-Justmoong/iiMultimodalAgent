#include "native_link.h"
#include <iiMultimodalAgent/agent/CommandHooks.h>
#include <iiMultimodalAgent/agent/Engine.h>
#include <iiMultimodalAgent/agent/McpServer.h>
#include <iiMultimodalAgent/agent/PermissionSettings.h>
#include <iiMultimodalAgent/agent/Subagents.h>
#include <iiMultimodalAgent/agent/ShellTasks.h>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
class Model final:public a::Model {
public:
    std::function<a::ModelReply(const a::ModelRequest&,const CancellationToken&)> next;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const TextCallback&) override {
        token.throwIfCancelled();return next(request,token);
    }
};
a::Hook configured(const QString& work,const QString& event="Stop",double timeout=60,int input=1024*1024,int output=1024*1024) {
    a::CommandHookOptions limits;limits.workingDirectory=work;limits.maxInputBytes=input;limits.maxOutputBytes=output;
    return a::CommandHooks({{"hooks",QJsonObject{{event,QJsonArray{QJsonObject{{"hooks",QJsonArray{
        QJsonObject{{"type","agent"},{"prompt","Verify the host condition. $ARGUMENTS"},{"timeout",timeout}}}}}}}}}},limits).callback();
}
QString read(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};return QString::fromUtf8(f.readAll());}
void save(const QString& path,const QByteArray& bytes){QDir().mkpath(QFileInfo(path).absolutePath());QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("fixture write");}
QJsonObject grant(const QString& behavior,const QString& tool,const QString& pattern={}) {
    QJsonObject rule{{"toolName",tool}};if(!pattern.isEmpty())rule["ruleContent"]=pattern;
    return {{"type","addRules"},{"destination","session"},{"behavior",behavior},{"rules",QJsonArray{rule}}};
}
struct Fixture {
    QTemporaryDir root;QString work=root.filePath("work");
    std::shared_ptr<Model> model=std::make_shared<Model>();
    std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    a::EngineOptions options{.decision={.enabled=false}};
    Fixture(){QDir().mkpath(work);options.sessionsDirectory=root.filePath("state");options.skills.enabled=false;options.projectContext.enabled=false;
        a::registerWorkspaceTools(*registry,work,{},QStringList{options.sessionsDirectory});}
    bool clean() const {return QDir(root.filePath("state/hook-agents")).entryList(QDir::Dirs|QDir::NoDotAndDotDot).isEmpty();}
};
}
class AgentHooksTests final:public QObject {
public:
    void freshAgentReadsExactTranscriptAndCompletesWithoutLaterTools() {
        QTemporaryDir root;const auto work=root.filePath("work");CTEST_CHECK(QDir().mkpath(work));
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();
        a::registerWorkspaceTools(*registry,work,{},QStringList{root.filePath("state")});
        a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=root.filePath("state");options.skills.enabled=false;options.projectContext.enabled=false;
        options.hooks={configured(work)};QString transcript;int mainCalls=0,verifierCalls=0;bool isolated=false,observed=false;
        model->next=[&](const a::ModelRequest& request,const CancellationToken&) {
            if(!request.verificationAgent){++mainCalls;return a::ModelReply{"PARENT_FINISHED"};}
            ++verifierCalls;
            if(verifierCalls==1) {
                isolated=!QJsonDocument(a::toJson(request.messages.first())).toJson().contains("PARENT_PRIVATE");
                return a::ModelReply{{},{{"read","Read",{{"path",transcript},{"limit",20000}}}}};
            }
            observed=request.messages.last().text.contains("PARENT_PRIVATE")&&!request.messages.last().isError;
            return a::ModelReply{{},{{"write","Write",{{"path","proof.txt"},{"content","VERIFIED"}}},
                {"decision","StructuredOutput",{{"ok",true}}},{"after","Write",{{"path","after.txt"},{"content","MUST_NOT_RUN"}}}}};
        };
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"Write(proof.txt)",a::PermissionBehavior::Allow},{"Write(after.txt)",a::PermissionBehavior::Allow}});
        a::Engine engine(model,registry,policy,options);const auto session=engine.createSession("model://fixture",work);transcript=engine.transcriptPath(session.id);
        const auto result=engine.run({session.id,"PARENT_PRIVATE"}).result.get();
        CTEST_COMPARE(result.status,a::RunStatus::Completed);CTEST_COMPARE(mainCalls,1);CTEST_COMPARE(verifierCalls,2);CTEST_CHECK(isolated&&observed);
        CTEST_COMPARE(read(work+"/proof.txt"),"VERIFIED");CTEST_CHECK(!QFileInfo::exists(work+"/after.txt"));
        CTEST_COMPARE(engine.session(session.id).messages.size(),2);
        const QDir temporary(root.filePath("state/hook-agents"));CTEST_CHECK(temporary.entryList(QDir::Dirs|QDir::NoDotAndDotDot).isEmpty());
    }
    void failedConditionRequestsAnotherParentTurn() {
        QTemporaryDir root;auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();
        a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=root.filePath("state");options.skills.enabled=false;options.projectContext.enabled=false;
        options.hooks={configured(root.path())};int parent=0,verifier=0;bool feedback=false;
        model->next=[&](const a::ModelRequest& request,const CancellationToken&) {
            if(request.verificationAgent)return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",++verifier>1},{"reason","NEEDS_MORE"}}}}};
            ++parent;if(parent==2)feedback=request.messages.last().text.contains("NEEDS_MORE");return a::ModelReply{parent==1?"unfinished":"fixed"};
        };
        a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(),options);const auto session=engine.createSession("model://fixture",root.path());
        const auto result=engine.run({session.id,"Finish the work"}).result.get();CTEST_COMPARE(result.status,a::RunStatus::Completed);
        CTEST_COMPARE(result.text,"fixed");CTEST_COMPARE(parent,2);CTEST_COMPARE(verifier,2);CTEST_CHECK(feedback);
    }
    void assistantMessageLimit(int decisionAt) {
        QTemporaryDir root;auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();
        a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=root.filePath("state");options.skills.enabled=false;options.projectContext.enabled=false;
        options.hooks={configured(root.path())};int calls=0;QJsonObject diagnostic;
        model->next=[&](const a::ModelRequest& request,const CancellationToken&) {
            if(!request.verificationAgent)return a::ModelReply{"main"};
            if(++calls==decisionAt)return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
            return a::ModelReply{"I have not called the result tool yet"};
        };
        a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(),options);const auto session=engine.createSession("model://fixture",root.path());
        const auto result=engine.run({session.id,"Verify"},[&](const a::Event& event){if(event.kind==a::EventKind::Hook&&event.data["hook_type"]=="agent")diagnostic=event.data;}).result.get();
        CTEST_COMPARE(result.status,a::RunStatus::Completed);
        CTEST_COMPARE(calls,decisionAt);CTEST_COMPARE(diagnostic["outcome"],decisionAt==49?"success":"cancelled");
    }
    void taskVerifierCanInspectTheSameBoard() {
        QTemporaryDir root;auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();
        a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=root.filePath("state");options.skills.enabled=false;options.projectContext.enabled=false;
        options.taskToolsEnabled=true;options.taskToolsDeferred=false;options.hooks={configured(root.path(),"TaskCreated")};int turns=0;bool inspected=false;
        model->next=[&](const a::ModelRequest& request,const CancellationToken&) {
            if(++turns==1)return a::ModelReply{{},{{"list","TaskList",{}}}};
            inspected=!request.messages.last().isError&&request.messages.last().data["total"]==0;
            return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(),options);const auto session=engine.createSession("model://fixture",root.path());
        const auto task=engine.runTaskTool(session.id,"TaskCreate",{{"subject","TEST"},{"description","verify"}});
        CTEST_CHECK_MESSAGE(!task.isError,qPrintable(task.text));CTEST_CHECK(inspected);CTEST_COMPARE(turns,2);
        CTEST_COMPARE(engine.runTaskTool(session.id,"TaskList").data["total"].toInt(),1);
    }
    void mcpVerifierUsesEngineOwnerForTasksAndRuntimeGrants() {
        Fixture f;f.options.taskToolsEnabled=true;f.options.taskToolsDeferred=false;
        a::PermissionSettingsOptions settings;settings.workingDirectory=f.work;settings.fallbackMode=a::PermissionMode::Bypass;
        auto policy=std::make_shared<a::SettingsPermissionPolicy>(settings);
        auto engine=std::make_shared<a::Engine>(f.model,f.registry,policy,f.options);
        a::McpServerOptions server;server.workingDirectory=f.work;server.model="model://fixture";server.engine=engine;
        const auto hook=configured(f.work,"PreToolUse");
        server.tools.hooks={ [hook](const a::HookInput& input,const CancellationToken& token){
            return input.kind==a::HookKind::BeforeTool&&input.call.name=="TaskList"?hook(input,token):a::HookResult{};}};
        const auto bridge=a::mcpServerOptions(f.registry,policy,server);
        mcp::ServerRequestContext context;context.sessionId="connection-1";
        auto call=[&](QString name,QJsonObject args=QJsonObject{}){return bridge.handlers.at("tools/call")({{"name",name},{"arguments",args}},context);};
        CTEST_CHECK(!call("TaskCreate",{{"subject","OWNER_TASK"},{"description","parent board"}})["isError"].toBool());
        const auto owner=call("iiLocalLLM.agent.session")["structuredContent"].toObject()["session_id"].toString();
        CTEST_CHECK(!owner.isEmpty()&&owner!=context.sessionId);
        policy->applyUpdates({grant("allow","Write","/granted.txt")},{owner,{},f.work});
        int turns=0;bool correctBoard=false,correctGrant=false;QJsonArray grantResults;
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){
            if(++turns==1)return a::ModelReply{{},{{"list","TaskList",{}}}};
            if(turns==2){correctBoard=request.messages.last().text.contains("OWNER_TASK");
                return a::ModelReply{{},{{"yes","Write",{{"path","granted.txt"},{"content","GRANTED"}}},{"no","Write",{{"path","implicit.txt"},{"content","DENIED"}}}}};}
            for(const auto& message:request.messages)if(message.role==a::MessageRole::Tool)grantResults.append(a::toJson(message));
            correctGrant=!request.messages[request.messages.size()-2].isError&&request.messages.last().isError;
            return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        const auto result=call("TaskList");CTEST_CHECK_MESSAGE(!result["isError"].toBool(),qPrintable(QJsonDocument(result).toJson()));
        CTEST_COMPARE(turns,3);CTEST_CHECK(correctBoard);CTEST_CHECK_MESSAGE(correctGrant,qPrintable(QJsonDocument(grantResults).toJson(QJsonDocument::Compact)));
        CTEST_COMPARE(read(f.work+"/granted.txt"),"GRANTED");CTEST_CHECK(!QFileInfo::exists(f.work+"/implicit.txt"));
        CTEST_COMPARE(policy->describe({owner,{},f.work})["mode"],"bypassPermissions");
        CTEST_CHECK(f.clean());
    }
    void verifierUsesDontAskWithExplicitRules(int mode) {
        Fixture f;f.options.hooks={configured(f.work)};int turns=0,permissions=0;bool checked=false;
        f.options.permission=[&](const auto&,const auto&,const auto&){++permissions;return true;};
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode(mode),QList<a::PermissionRule>{
            {"Write(yes.txt)",a::PermissionBehavior::Allow},{"Write(guard.txt)",a::PermissionBehavior::Allow},
            {"Write(guard.txt)",a::PermissionBehavior::Deny},{"Write(ask.txt)",a::PermissionBehavior::Ask}});
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){
            if(!request.verificationAgent)return a::ModelReply{"main"};
            if(++turns==1){QList<a::ToolCall> calls;for(const auto& name:{"yes.txt","implicit.txt","guard.txt","ask.txt"})
                calls.append({name,"Write",{{"path",name},{"content","DATA"}}});return a::ModelReply{{},calls};}
            const auto n=request.messages.size();checked=!request.messages[n-4].isError&&request.messages[n-3].isError&&request.messages[n-2].isError&&request.messages[n-1].isError;
            return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        a::Engine engine(f.model,f.registry,policy,f.options);const auto session=engine.createSession("fixture",f.work);
        CTEST_COMPARE(engine.run({session.id,"verify"}).result.get().status,a::RunStatus::Completed);
        CTEST_COMPARE(turns,2);CTEST_CHECK(checked);CTEST_COMPARE(permissions,0);CTEST_COMPARE(read(f.work+"/yes.txt"),"DATA");
        for(const auto& path:{"implicit.txt","guard.txt","ask.txt"})CTEST_CHECK(!QFileInfo::exists(f.work+'/'+path));
        CTEST_COMPARE(policy->decide(f.registry->get("Write").definition,{{"path","implicit.txt"}},{session.id,{},f.work}).behavior,
            mode==int(a::PermissionMode::Default)?a::PermissionBehavior::Ask:a::PermissionBehavior::Allow);
        CTEST_CHECK(f.clean());
    }
    void transcriptGrantDoesNotExposeAdjacentStateOrWrites() {
        Fixture f;f.options.hooks={configured(f.work)};QString transcript;int turns=0;bool checked=false;
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"Write",a::PermissionBehavior::Allow}});
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){
            if(!request.verificationAgent)return a::ModelReply{"main"};
            if(++turns==1)return a::ModelReply{{},{{"sibling","Read",{{"path",QFileInfo(transcript).dir().filePath("neighbor.txt")}}},
                {"alias","Read",{{"path","alias.txt"}}},{"write","Write",{{"path",transcript},{"content","CORRUPT"}}}}};
            const auto n=request.messages.size();checked=request.messages[n-3].isError&&request.messages[n-2].isError&&request.messages[n-1].isError;
            return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        a::Engine engine(f.model,f.registry,policy,f.options);const auto session=engine.createSession("fixture",f.work);transcript=engine.transcriptPath(session.id);
        save(QFileInfo(transcript).dir().filePath("neighbor.txt"),"PRIVATE_NEIGHBOR");CTEST_CHECK(createNativeTestLink(transcript,f.work+"/alias.txt"));
        CTEST_COMPARE(engine.run({session.id,"PARENT_PRIVATE"}).result.get().status,a::RunStatus::Completed);
        CTEST_COMPARE(turns,2);CTEST_CHECK(checked);CTEST_CHECK(read(transcript).contains("PARENT_PRIVATE"));CTEST_CHECK(!read(transcript).contains("CORRUPT"));
        CTEST_CHECK(f.clean());
    }
    void explicitTranscriptDenyOverridesTheExactReadGrant() {
        Fixture f;f.options.hooks={configured(f.work)};QString transcript;int turns=0;bool denied=false;
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"Read",a::PermissionBehavior::Deny}});
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){
            if(!request.verificationAgent)return a::ModelReply{"main"};
            if(++turns==1)return a::ModelReply{{},{{"read","Read",{{"path",transcript}}}}};
            denied=request.messages.last().isError;return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        a::Engine engine(f.model,f.registry,policy,f.options);const auto session=engine.createSession("fixture",f.work);transcript=engine.transcriptPath(session.id);
        CTEST_COMPARE(engine.run({session.id,"verify"}).result.get().status,a::RunStatus::Completed);CTEST_CHECK(denied);CTEST_CHECK(f.clean());
    }
    void invalidResultAndForbiddenControlsCannotComplete() {
        Fixture f;f.options.hooks={configured(f.work)};int turns=0,executed=0;bool hidden=true,denied=false;
        const QStringList forbidden{"Agent","AgentOutput","TaskOutput","TaskStop","EnterPlanMode","ExitPlanMode","AskUserQuestion","Workflow","iiLocalLLM.agent.run","mcp__app__question"};
        for(const auto& name:forbidden){a::Tool tool;tool.definition={name,"forbidden",{{"type","object"}},{},true};
            if(name=="mcp__app__question")tool.definition.metadata={{"source","mcp"},{"requires_user_interaction",true}};
            tool.execute=[&](const auto&,const auto&){++executed;return a::ToolResult{};};f.registry->add(tool);}
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){
            if(!request.verificationAgent)return a::ModelReply{"main"};
            for(const auto& tool:request.tools)hidden&=!forbidden.contains(tool.name);
            if(++turns==1){QList<a::ToolCall> calls{{"invalid","StructuredOutput",{{"ok","yes"}}}};
                for(const auto& name:forbidden)calls.append({name,name,{}});return a::ModelReply{{},calls};}
            denied=true;for(auto i=request.messages.crbegin();i!=request.messages.crend()&&i->role==a::MessageRole::Tool;++i)denied&=i->isError;
            return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        a::Engine engine(f.model,f.registry,std::make_shared<a::RulePolicy>(),f.options);const auto session=engine.createSession("fixture",f.work);
        CTEST_COMPARE(engine.run({session.id,"verify"}).result.get().status,a::RunStatus::Completed);
        CTEST_COMPARE(turns,2);CTEST_CHECK(hidden&&denied);CTEST_COMPARE(executed,0);CTEST_CHECK(f.clean());
    }
    void ownDeadlineDoesNotCancelParent() {
        Fixture f;f.options.hooks={configured(f.work,"Stop",0.03)};QJsonObject diagnostic;
        f.model->next=[](const a::ModelRequest& request,const CancellationToken& token)->a::ModelReply {
            if(!request.verificationAgent)return {"main"};
            while(!token.isCancelled())std::this_thread::sleep_for(std::chrono::milliseconds(1));token.throwIfCancelled();return {};
        };
        a::Engine engine(f.model,f.registry,std::make_shared<a::RulePolicy>(),f.options);const auto session=engine.createSession("fixture",f.work);
        const auto result=engine.run({session.id,"verify"},[&](const a::Event& event){if(event.kind==a::EventKind::Hook)diagnostic=event.data;}).result.get();
        CTEST_COMPARE(result.status,a::RunStatus::Completed);CTEST_COMPARE(diagnostic["outcome"],"cancelled");CTEST_COMPARE(diagnostic["error_code"],"timeout");CTEST_CHECK(f.clean());
    }
    void parentCancellationJoinsAndRemovesVerifier() {
        Fixture f;f.options.hooks={configured(f.work)};std::atomic_bool started=false;
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken& token)->a::ModelReply {
            if(!request.verificationAgent)return {"main"};started=true;
            while(!token.isCancelled())std::this_thread::sleep_for(std::chrono::milliseconds(1));token.throwIfCancelled();return {};
        };
        a::Engine engine(f.model,f.registry,std::make_shared<a::RulePolicy>(),f.options);const auto session=engine.createSession("fixture",f.work);
        auto handle=engine.run({session.id,"verify"});CTEST_WAIT_VERIFY(started.load(),2000);handle.cancel();
        CTEST_COMPARE(handle.result.get().status,a::RunStatus::Cancelled);CTEST_CHECK(f.clean());
    }
    void verifierByteLimitsAreDiagnosedAndCleaned(bool inputLimit) {
        Fixture f;f.options.hooks={configured(f.work,"Stop",60,inputLimit?2500:1024*1024,inputLimit?1024*1024:512)};
        QJsonObject diagnostic;int calls=0;
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){if(!request.verificationAgent)return a::ModelReply{"main"};++calls;return a::ModelReply{QString(600,'x')};};
        a::Engine engine(f.model,f.registry,std::make_shared<a::RulePolicy>(),f.options);const auto session=engine.createSession("fixture",f.work);
        const auto result=engine.run({session.id,"verify"},[&](const a::Event& event){if(event.kind==a::EventKind::Hook)diagnostic=event.data;}).result.get();
        CTEST_COMPARE(result.status,a::RunStatus::Completed);CTEST_COMPARE(diagnostic["outcome"],"non_blocking_error");CTEST_COMPARE(diagnostic["error_code"],"resource_limit");
        CTEST_COMPARE(calls,inputLimit?0:1);CTEST_CHECK(f.clean());
    }
    void inlineSkillsAndDiscoveryWorkWithoutRecursiveSkillForks() {
        Fixture f;f.options.skills.enabled=true;f.options.hooks={configured(f.work)};int forks=0,turns=0,echoes=0;bool loaded=false,found=false,forkDenied=false;
        f.options.forkedSkill=[&](const auto&,const auto&){++forks;return a::SkillForkResult{};};
        save(f.work+"/.claude/skills/writer/SKILL.md","---\ndescription: Write verification proof\nallowed-tools: [Write]\n---\nINLINE_SKILL_MARKER");
        save(f.work+"/.claude/skills/delegate/SKILL.md","---\ndescription: Nested fork\ncontext: fork\n---\nNESTED_FORK");
        a::Tool echo;echo.definition={"DeferredEcho","Deferred observation",{{"type","object"}},{},true,true,false,true};
        echo.execute=[&](const auto&,const auto&){++echoes;return a::ToolResult{"OBSERVED"};};f.registry->add(echo);
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{{"Skill(writer)",a::PermissionBehavior::Allow},{"Skill(delegate)",a::PermissionBehavior::Allow}});
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){
            if(!request.verificationAgent)return a::ModelReply{"main"};
            if(++turns==1)return a::ModelReply{{},{{"skill","Skill",{{"skill","writer"}}},{"fork","Skill",{{"skill","delegate"}}},
                {"search","ToolSearch",{{"query","select:DeferredEcho"}}}}};
            if(turns==2){for(const auto& m:request.messages){loaded|=m.text.contains("INLINE_SKILL_MARKER");if(m.toolCallId=="fork")forkDenied=m.isError;}
                for(const auto& t:request.tools)found|=t.name=="DeferredEcho";
                return a::ModelReply{{},{{"echo","DeferredEcho",{}},{"write","Write",{{"path","skill.txt"},{"content","SKILL_GRANTED"}}}}};}
            return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        a::Engine engine(f.model,f.registry,policy,f.options);const auto session=engine.createSession("fixture",f.work);
        CTEST_COMPARE(engine.run({session.id,"verify"}).result.get().status,a::RunStatus::Completed);
        CTEST_COMPARE(turns,3);CTEST_COMPARE(echoes,1);CTEST_COMPARE(forks,0);CTEST_CHECK(loaded&&found&&forkDenied);
        CTEST_COMPARE(read(f.work+"/skill.txt"),"SKILL_GRANTED");CTEST_CHECK(f.clean());
        CTEST_COMPARE(policy->decide(f.registry->get("Write").definition,{{"path","skill.txt"}},{session.id,{},f.work}).behavior,a::PermissionBehavior::Ask);
    }
    void scopedSubagentHooksRetainResultToolAndPlanRestrictions() {
        Fixture f;f.options.hooks={configured(f.work,"SubagentStart"),configured(f.work,"SubagentStop")};
        a::Tool probe;probe.definition={"HostProbe","Read host evidence",{{"type","object"}},{},true};
        probe.execute=[](const auto&,const auto&){return a::ToolResult{"HOST_PROBE"};};f.options.additionalTools={probe};
        a::SubagentOptions options;options.workingDirectory=f.work;options.stateDirectory=f.root.filePath("children");
        a::SubagentDefinition profile;profile.name="reviewer";profile.description="Review";profile.tools={"Read","Write","HostProbe"};profile.permissionMode="plan";options.definitions={profile};
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"Write",a::PermissionBehavior::Allow}});
        a::SessionStore store(f.options.sessionsDirectory);const auto parent=store.create("fixture","",f.work);int verifier=0,children=0;bool resultVisible=true,probeVisible=true,denied=true;
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){
            if(!request.verificationAgent){++children;return a::ModelReply{"CHILD_DONE"};}
            bool output=false,host=false;for(const auto& t:request.tools){output|=t.name=="StructuredOutput";host|=t.name=="HostProbe";}
            resultVisible&=output;probeVisible&=host;
            if(++verifier%2==1)return a::ModelReply{{},{{"write","Write",{{"path","plan.txt"},{"content","FORBIDDEN"}}},{"probe","HostProbe",{}}}};
            const auto n=request.messages.size();denied&=request.messages[n-2].isError&&!request.messages[n-1].isError;
            return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        a::Subagents agents(f.model,f.registry,policy,f.options,options);a::ToolContext context{parent.id,{},f.work};context.sessionSnapshot=std::make_shared<a::Session>(parent);
        const auto result=agents.run(context,{{"prompt","Review"},{"subagent_type","reviewer"}});CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
        CTEST_COMPARE(verifier,4);CTEST_COMPARE(children,1);CTEST_CHECK(resultVisible&&probeVisible&&denied);CTEST_CHECK(!QFileInfo::exists(f.work+"/plan.txt"));
        CTEST_CHECK(QDir(f.root.filePath("children/sessions/hook-agents")).entryList(QDir::Dirs|QDir::NoDotAndDotDot).isEmpty());
    }
    void backgroundShellsAreStoppedWhenVerificationEnds() {
        Fixture f;f.registry=std::make_shared<a::ToolRegistry>();auto shells=std::make_shared<a::ShellTasks>(f.work,f.root.filePath("shells"));
        a::registerWorkspaceTools(*f.registry,f.work,shells,{f.options.sessionsDirectory});f.options.hooks={configured(f.work)};
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{{"Bash(sleep 30)",a::PermissionBehavior::Allow}});
        int turns=0;QString task,agent;
        f.model->next=[&](const a::ModelRequest& request,const CancellationToken&){
            if(!request.verificationAgent)return a::ModelReply{"main"};
            if(++turns==1)return a::ModelReply{{},{{"start","Bash",{{"command","sleep 30"},{"run_in_background",true}}}}};
            task=request.messages.last().data["backgroundTaskId"].toString();return a::ModelReply{{},{{"decision","StructuredOutput",{{"ok",true}}}}};
        };
        a::Engine engine(f.model,f.registry,policy,f.options);const auto session=engine.createSession("fixture",f.work);
        const auto result=engine.run({session.id,"verify"},[&](const a::Event& event){if(event.kind==a::EventKind::Hook)agent=event.data["agent_id"].toString();}).result.get();
        CTEST_COMPARE(result.status,a::RunStatus::Completed);CTEST_CHECK(!agent.isEmpty()&&!task.isEmpty());
        CTEST_COMPARE(shells->output(agent,task,false)["task"].toObject()["status"],"killed");CTEST_CHECK(shells->list(session.id).isEmpty());CTEST_CHECK(f.clean());
    }
    void configurationDeduplicatesAgentAndPromptSeparately() {
        Fixture f;a::CommandHookOptions limits;limits.workingDirectory=f.work;
        QJsonObject one{{"type","agent"},{"prompt","same"},{"model","first"}},two=one,prompt{{"type","prompt"},{"prompt","same"}};
        two["model"]="last";two["once"]=true;
        a::CommandHooks hooks({{"hooks",QJsonObject{{"Stop",QJsonArray{QJsonObject{{"hooks",QJsonArray{one,two,prompt}}}}}}}},limits);
        CTEST_COMPARE(hooks.describe()["provider"],"configured");CTEST_COMPARE(hooks.describe()["hooks"].toArray().first().toObject()["timeout_ms"],60000);
        auto context=std::make_shared<a::ModelHookContext>();context->model=f.model;context->modelName="fixture";
        a::HookInput input{a::HookKind::Stop,"owner","run"};input.modelContext=context;
        auto result=hooks.callback()(input,{});CTEST_COMPARE(result.diagnostics.first().toObject()["error_code"],"runtime_unavailable");
        int agents=0,prompts=0;QString selected;
        f.model->next=[&](const auto&,const auto&){++prompts;return a::ModelReply{"{\"ok\":true}"};};
        context->agentExecutor=[&](const a::AgentHookRequest& request,const auto&,const auto&){++agents;selected=request.model;a::AgentHookReply reply;reply.decision={{"ok",true}};return reply;};
        // Missing model code above must not consume the once entry; only a
        // started executor consumes it. Prompt provider is independently run.
        hooks.callback()(input,{});hooks.callback()(input,{});CTEST_COMPARE(agents,1);CTEST_COMPARE(selected,"last");CTEST_COMPARE(prompts,2);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    AgentHooksTests tests;
    runner.run("freshAgentReadsExactTranscriptAndCompletesWithoutLaterTools", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.freshAgentReadsExactTranscriptAndCompletesWithoutLaterTools(); });
    runner.run("failedConditionRequestsAnotherParentTurn", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.failedConditionRequestsAnotherParentTurn(); });
    runner.run("assistantMessageLimit:49th accepted", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.assistantMessageLimit(49); });
    runner.run("assistantMessageLimit:50th not executed", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.assistantMessageLimit(50); });
    runner.run("taskVerifierCanInspectTheSameBoard", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.taskVerifierCanInspectTheSameBoard(); });
    runner.run("mcpVerifierUsesEngineOwnerForTasksAndRuntimeGrants", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.mcpVerifierUsesEngineOwnerForTasksAndRuntimeGrants(); });
    runner.run("verifierUsesDontAskWithExplicitRules:bypass", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.verifierUsesDontAskWithExplicitRules(int(a::PermissionMode::Bypass)); });
    runner.run("verifierUsesDontAskWithExplicitRules:accept edits", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.verifierUsesDontAskWithExplicitRules(int(a::PermissionMode::AcceptEdits)); });
    runner.run("verifierUsesDontAskWithExplicitRules:default", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.verifierUsesDontAskWithExplicitRules(int(a::PermissionMode::Default)); });
    runner.run("transcriptGrantDoesNotExposeAdjacentStateOrWrites", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.transcriptGrantDoesNotExposeAdjacentStateOrWrites(); });
    runner.run("explicitTranscriptDenyOverridesTheExactReadGrant", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.explicitTranscriptDenyOverridesTheExactReadGrant(); });
    runner.run("invalidResultAndForbiddenControlsCannotComplete", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.invalidResultAndForbiddenControlsCannotComplete(); });
    runner.run("ownDeadlineDoesNotCancelParent", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.ownDeadlineDoesNotCancelParent(); });
    runner.run("parentCancellationJoinsAndRemovesVerifier", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.parentCancellationJoinsAndRemovesVerifier(); });
    runner.run("verifierByteLimitsAreDiagnosedAndCleaned:input", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.verifierByteLimitsAreDiagnosedAndCleaned(true); });
    runner.run("verifierByteLimitsAreDiagnosedAndCleaned:output", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.verifierByteLimitsAreDiagnosedAndCleaned(false); });
    runner.run("inlineSkillsAndDiscoveryWorkWithoutRecursiveSkillForks", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.inlineSkillsAndDiscoveryWorkWithoutRecursiveSkillForks(); });
    runner.run("scopedSubagentHooksRetainResultToolAndPlanRestrictions", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.scopedSubagentHooksRetainResultToolAndPlanRestrictions(); });
    runner.run("backgroundShellsAreStoppedWhenVerificationEnds", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.backgroundShellsAreStoppedWhenVerificationEnds(); });
    runner.run("configurationDeduplicatesAgentAndPromptSeparately", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.configurationDeduplicatesAgentAndPromptSeparately(); });
    return runner.result();
}

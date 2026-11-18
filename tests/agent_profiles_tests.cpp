#include "native_link.h"
#include "iiMultimodalAgent/agent/AgentProfiles.h"
#include "iiMultimodalAgent/agent/Subagents.h"
#include "../tools/AgentProfileConfig.h"
#include <iiFileProviderDevice.h>
#include "CTestSupport.h"
#include <QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
void put(const QString& path,const QByteArray& bytes) {
    if(!QDir().mkpath(QFileInfo(path).absolutePath()))qFatal("mkdir failed");
    QFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size())qFatal("write failed");
}
QByteArray profile(const QByteArray& description,const QByteArray& extra={}) {
    return "---\nname: reviewer\ndescription: "+description+"\n"+extra+"---\nInspect the requested file and report observed findings.\n";
}
}
class AgentProfileTests final {
public:
    void hostConfigValidationAndCatalogPrivacy() {
        iiFileProvider::TemporaryDirectory root;const auto path=root.filePath("host.json");
        put(path,"{\"directories\":[\"profiles\"],\"include_builtins\":false,\"model_aliases\":{\"small\":\"local\"},\"allowed_models\":[\"other\"]}");
        CTEST_CHECK(QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner));
        const auto configured=iiLocalLLMClient::profileConfig(path,false);
        CTEST_COMPARE(configured.profiles.directories,QStringList{root.filePath("profiles")});CTEST_CHECK(!configured.profiles.includeBuiltins);
        CTEST_COMPARE(configured.modelAliases.value("small"),"local");CTEST_COMPARE(configured.allowedModels,QStringList{"other"});
        CTEST_THROWS(Error,iiLocalLLMClient::profileConfig(path,true));
        for(const auto& invalid:{"{\"unknown\":true}","{\"overrides\":[]}","{\"model_aliases\":{\"small\":false}}","{\"include_project\":1}","{\"directories\":[\"https://invalid/\"]}"}) {
            put(path,invalid);CTEST_THROWS(Error,iiLocalLLMClient::profileConfig(path,false));
        }
        a::AgentProfileOptions options;options.projectBoundary=root.path();
        put(root.filePath(".claude/agents/read.md"),profile("unsupported","mcpServers:\n  remote:\n    env:\n      SECRET: PRIVATE_MARKER\n"));
        const auto catalog=a::discoverAgentProfiles(root.path(),options);CTEST_CHECK(catalog.find("reviewer").unsupportedFeatures.contains("mcpServers"));
        CTEST_CHECK(!QJsonDocument(catalog.toJson()).toJson().contains("PRIVATE_MARKER"));
        options.managedDirectory=root.filePath("managed");put(options.managedDirectory+"/read.md",profile("managed"));
        options.overrides={{"reviewer",false}};CTEST_COMPARE(a::discoverAgentProfiles(root.path(),options).find("reviewer").source,"managed");
    }
    void missingPreloadLeavesNoChildAndProfileBackgroundIsHonored() {
        QString stage = "setup";
        try {
        iiFileProvider::TemporaryDirectory root;const auto workspace=root.filePath("workspace");QDir().mkpath(workspace);
        class Model final:public a::Model { a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const TextCallback&) override{return {"done",{}};} };
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo{.decision={.enabled=false}};eo.sessionsDirectory=root.filePath("parents");eo.projectContext.enabled=false;eo.compaction.automatic=false;
        a::SubagentOptions so;so.workingDirectory=workspace;so.stateDirectory=root.filePath("children");so.profiles.enabled=true;so.profiles.projectBoundary=workspace;
        auto agents=std::make_shared<a::Subagents>(model,registry,policy,eo,so);a::Subagents::attach(eo,agents);a::Engine engine(model,registry,policy,eo);const auto p=engine.createSession("local",workspace);
        const auto path=workspace+"/.claude/agents/review.md";put(path,profile("missing","skills: [missing]\n"));
        CTEST_CHECK(engine.runSubagentTool(p.id,"Agent",{{"prompt","go"},{"subagent_type","reviewer"}}).isError);
        CTEST_CHECK(agents->list(p.id).isEmpty());CTEST_CHECK(a::SessionStore(so.stateDirectory+"/sessions").list().isEmpty());
        put(path,profile("background","background: true\n"));
        QString id;
        // Inspect every accepted background job while its worker starts. The
        // owner must remain visible throughout queued/running/completed changes.
        for (int attempt = 0; attempt < 32; ++attempt) {
            stage = QString("launch background profile %1").arg(attempt);
            const auto result=engine.runSubagentTool(p.id,"Agent",{{"prompt","go"},{"subagent_type","reviewer"},{"run_in_background",false}});
            CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(result.data["status"],"async_launched");
            stage = QString("wait for background profile %1").arg(attempt);
            id=result.data["agentId"].toString();
            const auto output=agents->output(p.id,id,true,2000);
            CTEST_CHECK_MESSAGE(output["status"]=="completed",
                qPrintable(QStringLiteral("background profile state: %1").arg(output["status"].toString())));
            CTEST_CHECK(output["finished"].toBool());
            CTEST_COMPARE(output["owner_session_id"],p.id);
        }
        CTEST_COMPARE(agents->list(p.id).size(),32);
        // A fatal catalog error cannot prevent lifecycle cleanup or inspection.
        CTEST_CHECK(QFile::remove(path));QDir(workspace+"/.claude/agents").removeRecursively();
        CTEST_CHECK(createNativeTestLink(root.path(),workspace+"/.claude/agents"));
        stage = "stop with invalid profile catalog";
        engine.stopSubagents(p.id);
        stage = "inspect with invalid profile catalog";
        CTEST_CHECK(!engine.runSubagentTool(p.id,"AgentOutput",{{"agent_id",id}}).isError);
        } catch (const std::exception &error) { agent_ctest::fail(__FILE__, __LINE__, qPrintable(stage + ": " + QString::fromUtf8(error.what()))); }
    }
    void actualExecutionFreezesPromptAndIntersectsCurrentScope() {
        iiFileProvider::TemporaryDirectory root;const auto workspace=root.filePath("workspace");QDir().mkpath(workspace);
        class Model final:public a::Model {
        public:std::function<a::ModelReply(const a::ModelRequest&)> next;
            a::ModelReply generate(const a::ModelRequest& r,const CancellationToken&,const TextCallback&) override{return next(r);}
        };
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();
        registry->add({{"Read","read",{{"type","object"}}, {},true},[](const auto&,const auto&){return a::ToolResult{"observed"};}});
        registry->add({{"Write","write",{{"type","object"}}},[](const auto&,const auto&){throw std::runtime_error("write escaped");return a::ToolResult{};}});
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo{.decision={.enabled=false}};eo.sessionsDirectory=root.filePath("parents");eo.projectContext.enabled=false;eo.compaction.automatic=false;
        a::SubagentOptions so;so.workingDirectory=workspace;so.stateDirectory=root.filePath("children");so.profiles.enabled=true;so.profiles.projectBoundary=workspace;
        const auto path=workspace+"/.claude/agents/review.md";
        put(path,profile("original","tools: Read\ninitialPrompt: INITIAL_ONLY_ONCE\n"));
        auto agents=std::make_shared<a::Subagents>(model,registry,policy,eo,so);a::Subagents::attach(eo,agents);
        a::Engine engine(model,registry,policy,eo);const auto parent=engine.createSession("local",workspace);
        model->next=[](const auto& r){
            if(!r.systemPrompt.contains("Inspect the requested file"))throw std::runtime_error("missing profile prompt");
            if(!r.messages.last().text.contains("INITIAL_ONLY_ONCE"))throw std::runtime_error("missing initial prompt");
            for(const auto& t:r.tools)if(t.name!="Read")throw std::runtime_error("scope leak");
            return a::ModelReply{"first",{}};
        };
        auto result=engine.runSubagentTool(parent.id,"Agent",{{"prompt","review"},{"subagent_type","reviewer"}});CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
        const auto id=result.data["agentId"].toString();const auto original=agents->output(parent.id,id)["profile"].toObject();CTEST_CHECK(!original["sha256"].toString().isEmpty());
        put(path,"---\nname: reviewer\ndescription: Changed\ntools: Write\n---\nREPLACED_PROMPT\n");
        CTEST_COMPARE(agents->profiles().find("reviewer").description,"Changed");
        model->next=[](const auto& r){
            if(r.systemPrompt.contains("REPLACED_PROMPT")||!r.tools.isEmpty()||r.messages.last().text!="continue")throw std::runtime_error("resume changed frozen contract");
            return a::ModelReply{"resumed",{}};
        };
        result=engine.runSubagentTool(parent.id,"Agent",{{"prompt","continue"},{"resume",id}});CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
        CTEST_COMPARE(agents->output(parent.id,id)["profile"].toObject(),original);
        put(path,profile("Unsupported","isolation: worktree\n"));
        const auto denied=engine.runSubagentTool(parent.id,"Agent",{{"prompt","run"},{"subagent_type","reviewer"}});CTEST_CHECK(denied.isError);CTEST_COMPARE(agents->list(parent.id).size(),1);
    }
    void liveModelCatalogPreloadAndModelAuthorization() {
        iiFileProvider::TemporaryDirectory root;const auto workspace=root.filePath("workspace");QDir().mkpath(workspace);
        class Model final:public a::Model { public:std::function<a::ModelReply(const a::ModelRequest&)> next;
            a::ModelReply generate(const a::ModelRequest& r,const CancellationToken&,const TextCallback&) override{return next(r);} };
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo{.decision={.enabled=false}};eo.sessionsDirectory=root.filePath("parents");eo.projectContext.enabled=false;eo.compaction.automatic=false;
        a::SubagentOptions so;so.workingDirectory=workspace;so.stateDirectory=root.filePath("children");so.profiles.enabled=true;so.profiles.projectBoundary=workspace;so.modelAliases={{"small","authorized-local"}};
        auto agents=std::make_shared<a::Subagents>(model,registry,policy,eo,so);a::Subagents::attach(eo,agents);
        a::Engine engine(model,registry,policy,eo);const auto parent=engine.createSession("local",workspace);
        // Created after engine construction: the next turn and API must see it.
        put(workspace+"/.claude/agents/review.md",profile("Live agent","model: small\nskills: [inspect]\ntools: []\n"));
        put(workspace+"/.claude/skills/inspect/SKILL.md","---\nname: inspect\ndescription: Inspect data\n---\nPRELOADED_EVIDENCE ${CLAUDE_SESSION_ID}\n");
        int children=0;model->next=[&](const auto& r){
            if(r.contextId==parent.id){
                if(r.messages.last().role==a::MessageRole::Tool)return a::ModelReply{r.messages.last().data["result"].toObject()["text"].toString(),{}};
                bool found=false;for(const auto& t:r.tools)if(t.name=="Agent")found=t.description.contains("Live agent")&&t.inputSchema["properties"].toObject()["subagent_type"].toObject()["enum"].toArray().contains("reviewer");
                if(!found)throw std::runtime_error("stale profile catalog");
                return a::ModelReply{{},{{"delegate","Agent",{{"prompt","review"},{"subagent_type","reviewer"}}}}};
            }
            ++children;if(r.model!="authorized-local"||!r.tools.isEmpty())throw std::runtime_error("model or tool scope mismatch");
            bool found=false;for(const auto& m:r.messages)if(m.metadata.contains("iilocal.skill"))found|=m.text.contains("PRELOADED_EVIDENCE "+r.contextId);
            if(!found)throw std::runtime_error("profile skill missing");return a::ModelReply{"preloaded",{}};
        };
        const auto result=engine.run({parent.id,"delegate"}).result.get();CTEST_COMPARE(result.status,a::RunStatus::Completed);CTEST_COMPARE(result.text,"preloaded");CTEST_COMPARE(children,1);
        put(workspace+"/.claude/agents/review.md",profile("Unauthorized model","model: arbitrary-model\n"));
        CTEST_CHECK(engine.runSubagentTool(parent.id,"Agent",{{"prompt","run"},{"subagent_type","reviewer"}}).isError);CTEST_COMPARE(agents->list(parent.id).size(),1);
        const auto catalog=engine.runSubagentTool(parent.id,"AgentProfiles");CTEST_CHECK(!catalog.isError);CTEST_CHECK(!catalog.data["profiles"].toArray().isEmpty());
    }
    void precedenceAndLiveFileUpdates() {
        iiFileProvider::TemporaryDirectory root;const auto workspace=root.filePath("workspace");QDir().mkpath(workspace);
        a::AgentProfileOptions options;options.projectBoundary=workspace;
        options.userDirectory=root.filePath("user");options.managedDirectory=root.filePath("managed");
        options.pluginDirectories={root.filePath("plugin")};options.directories={root.filePath("flags")};
        const QStringList paths{options.pluginDirectories[0],options.userDirectory,workspace+"/.claude/agents",options.directories[0],options.managedDirectory};
        for(qsizetype i=0;i<paths.size();++i)put(paths[i]+"/review.md",profile(QByteArray::number(i)));
        auto catalog=a::discoverAgentProfiles(workspace,options);CTEST_COMPARE(catalog.find("reviewer").description,"4");
        CTEST_COMPARE(catalog.find("reviewer").source,"managed");CTEST_CHECK(!catalog.find("reviewer").sha256.isEmpty());
        CTEST_CHECK(catalog.shadowed.size()>=4);
        CTEST_CHECK(QFile::remove(paths.last()+"/review.md"));
        catalog=a::discoverAgentProfiles(workspace,options);CTEST_COMPARE(catalog.find("reviewer").description,"3");
        put(paths[3]+"/review.md",profile("changed","tools: [Read, Glob]\ndisallowedTools: [Glob]\n"));
        catalog=a::discoverAgentProfiles(workspace,options);CTEST_COMPARE(catalog.find("reviewer").description,"changed");
        CTEST_COMPARE(catalog.find("reviewer").tools,QStringList({"Read","Glob"}));
        CTEST_COMPARE(catalog.find("reviewer").disallowedTools,QStringList({"Glob"}));
        for(const auto& item:catalog.toJson()["profiles"].toArray())CTEST_CHECK(!item.toObject().contains("system_prompt"));
    }
    void metadataValidationAndUnsupportedExecutionAreVisible() {
        iiFileProvider::TemporaryDirectory root;a::AgentProfileOptions options;options.projectBoundary=root.path();
        put(root.filePath(".claude/agents/read.md"),profile("Read specialist","tools: Read, Grep\nmodel: inherit\nmaxTurns: 3\nbackground: true\npermissionMode: plan\nskills: [inspect]\ninitialPrompt: Verify evidence first.\n"));
        auto catalog=a::discoverAgentProfiles(root.path(),options);auto p=catalog.find("reviewer");
        CTEST_COMPARE(p.tools,QStringList({"Read","Grep"}));CTEST_CHECK(p.model.isEmpty());CTEST_COMPARE(p.maxTurns,3);
        CTEST_CHECK(p.background);CTEST_COMPARE(p.permissionMode,"plan");CTEST_COMPARE(p.skills,QStringList{"inspect"});CTEST_COMPARE(p.initialPrompt,"Verify evidence first.");
        put(root.filePath(".claude/agents/read.md"),profile("Unsupported isolation","isolation: worktree\n"));
        p=a::discoverAgentProfiles(root.path(),options).find("reviewer");CTEST_CHECK(p.unsupportedFeatures.contains("isolation"));
        put(root.filePath(".claude/agents/read.md"),profile("bad","maxTurns: 0\n"));
        catalog=a::discoverAgentProfiles(root.path(),options);CTEST_CHECK(!catalog.failedFiles.isEmpty());
        CTEST_THROWS(Error,catalog.find("reviewer"));
    }
    void projectHierarchyAndHostOverrides() {
        iiFileProvider::TemporaryDirectory root;const auto workspace=root.filePath("repo/child");QDir().mkpath(workspace);
        a::AgentProfileOptions options;options.projectBoundary=root.filePath("repo");
        put(root.filePath("repo/.claude/agents/p.md"),profile("ancestor"));
        put(workspace+"/.claude/agents/p.md",profile("nearest"));
        CTEST_COMPARE(a::discoverAgentProfiles(workspace,options).find("reviewer").description,"nearest");
        a::SubagentDefinition host;host.name="reviewer";host.description="host";
        CTEST_COMPARE(a::discoverAgentProfiles(workspace,options,{host}).find("reviewer").description,"host");
        options.overrides={{"reviewer",QJsonObject{{"description","json"},{"prompt","JSON system"},{"tools",QJsonArray{}},{"maxTurns",2}}}};
        const auto p=a::discoverAgentProfiles(workspace,options,{host}).find("reviewer");CTEST_COMPARE(p.description,"json");CTEST_CHECK(p.tools.isEmpty());CTEST_COMPARE(p.systemPrompt,"JSON system");
    }
    void scanConfinementCancellationAndFileLimits() {
        iiFileProvider::TemporaryDirectory root;const auto workspace=root.filePath("workspace");QDir().mkpath(workspace+"/.claude/agents");
        a::AgentProfileOptions options;options.projectBoundary=workspace;
        put(root.filePath("outside.md"),profile("external"));CTEST_CHECK(createNativeTestLink(root.filePath("outside.md"),workspace+"/.claude/agents/link.md"));
        auto catalog=a::discoverAgentProfiles(workspace,options);CTEST_CHECK(!catalog.failedFiles.isEmpty());
        CTEST_THROWS(Error,catalog.find("reviewer"));
        CancellationToken cancelled;cancelled.cancel();CTEST_THROWS(Error,a::discoverAgentProfiles(workspace,options,{},cancelled));
        CTEST_CHECK(QFile::remove(workspace+"/.claude/agents/link.md"));put(workspace+"/.claude/agents/a.md",profile("valid"));
        options.maxTotalBytes=16;CTEST_THROWS(Error,a::discoverAgentProfiles(workspace,options));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner;
    AgentProfileTests tests;
    runner.run("hostConfigValidationAndCatalogPrivacy", [&] { tests.hostConfigValidationAndCatalogPrivacy(); });
    runner.run("missingPreloadLeavesNoChildAndProfileBackgroundIsHonored", [&] { tests.missingPreloadLeavesNoChildAndProfileBackgroundIsHonored(); });
    runner.run("actualExecutionFreezesPromptAndIntersectsCurrentScope", [&] { tests.actualExecutionFreezesPromptAndIntersectsCurrentScope(); });
    runner.run("liveModelCatalogPreloadAndModelAuthorization", [&] { tests.liveModelCatalogPreloadAndModelAuthorization(); });
    runner.run("precedenceAndLiveFileUpdates", [&] { tests.precedenceAndLiveFileUpdates(); });
    runner.run("metadataValidationAndUnsupportedExecutionAreVisible", [&] { tests.metadataValidationAndUnsupportedExecutionAreVisible(); });
    runner.run("projectHierarchyAndHostOverrides", [&] { tests.projectHierarchyAndHostOverrides(); });
    runner.run("scanConfinementCancellationAndFileLimits", [&] { tests.scanConfinementCancellationAndFileLimits(); });
    return runner.result();
}

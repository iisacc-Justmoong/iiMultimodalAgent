#include "native_link.h"
#include "iiMultimodalAgent/agent/PluginRuntime.h"
#include "iiMultimodalAgent/agent/Api.h"
#include "iiMultimodalAgent/agent/McpServer.h"
#include "iiMultimodalAgent/mcp/Server.h"
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
void put(const QString& path,const QByteArray& bytes){
    if(!QDir().mkpath(QFileInfo(path).absolutePath()))qFatal("mkdir failed");
    QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())qFatal("write failed");
}
void json(const QString& path,const QJsonObject& o){put(path,QJsonDocument(o).toJson());}
QByteArray read(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))qFatal("read failed");return f.readAll();}
class Model final:public a::Model{
public:
    QList<a::ModelRequest> requests;
    a::ModelReply generate(const a::ModelRequest& r,const CancellationToken&,const TextCallback&) override{requests.append(r);return {r.messages.last().text,{}};}
};
struct Fixture{
    QTemporaryDir root{QDir::current().filePath("plg-XXXXXX")};
    QString workspace=root.filePath("workspace"),package=root.filePath("package");
    a::PluginStore store{{root.filePath("store${UNSET_PLUGIN_VARIABLE}")}};
    Fixture(){QDir().mkpath(workspace);manifest();put(package+"/skills/inspect/SKILL.md","---\ndescription: Inspect plugin data\n---\nPLUGIN_SKILL $0 ${CLAUDE_PLUGIN_ROOT} ${CLAUDE_PLUGIN_DATA}\n");}
    void manifest(QJsonObject extra={}){extra["name"]="sample";if(!extra.contains("version"))extra["version"]="1.0.0";json(package+"/.claude-plugin/plugin.json",extra);}
    a::PluginSnapshot install(){store.install(package);return store.snapshot();}
    std::shared_ptr<a::PluginRuntime> runtime(const a::PluginSnapshot& s){a::CommandHookOptions o;o.workingDirectory=workspace;return std::make_shared<a::PluginRuntime>(s,o);}
    a::EngineOptions engineOptions(){a::EngineOptions e{.decision={.enabled=false}};e.sessionsDirectory=root.filePath("sessions");e.projectContext.enabled=false;e.compaction.automatic=false;return e;}
    a::AgentProfileOptions profiles(){a::AgentProfileOptions p;p.includeProject=false;p.includeBuiltins=false;return p;}
    a::McpConnectionOptions connections(){a::McpConnectionOptions m;m.workingDirectory=workspace;m.refreshIntervalMs=0;return m;}
};
}
class PluginTests final:public QObject{
public:
    void additiveNamespacedDocumentsAndPrivateStatus(){
        Fixture f;
        put(f.package+"/commands/nested/legacy.md","---\ndescription: Legacy command\n---\nLEGACY $ARGUMENTS");
        put(f.package+"/extra/task.md","---\ndescription: Extra command\n---\nEXTRA");
        put(f.package+"/agents/reviewer.md","---\nname: reviewer\ndescription: Plugin reviewer\ntools: [Read]\nskills: [inspect]\n---\nREVIEW ${CLAUDE_PLUGIN_ROOT}");
        f.manifest({{"commands",QJsonArray{"./commands","./extra/task.md"}},{"agents","./agents/reviewer.md"},{"settings",QJsonObject{{"private","PRIVATE_MARKER"}}}});
        const auto s=f.install();CTEST_COMPARE(s.skills.size(),3);CTEST_COMPARE(s.agents.size(),1);
        auto e=f.engineOptions();auto p=f.profiles();auto m=f.connections();a::PluginRuntime::attach(e,p,m,f.runtime(s));
        const auto catalog=a::discoverSkills(f.workspace,e.skills);CTEST_COMPARE(catalog.skills.size(),3);
        const auto loaded=a::loadSkill(f.workspace,"sample:nested:legacy","value","session",a::SkillInvocationSource::User,e.skills);CTEST_CHECK(loaded.text.contains("LEGACY value"));
        const auto profile=a::discoverAgentProfiles(f.workspace,p).find("sample:reviewer");CTEST_COMPARE(profile.skills,QStringList{"sample:inspect"});CTEST_CHECK(profile.systemPrompt.contains(s.plugins[0].root));
        const auto info=QJsonDocument(e.pluginSnapshot->toJson()).toJson();CTEST_CHECK(!info.contains("PRIVATE_MARKER"));CTEST_CHECK(!info.contains(f.root.path().toUtf8()));
        CTEST_CHECK(s.plugins[0].unsupportedFeatures.contains("settings"));
        CTEST_THROWS(Error,a::PluginRuntime::attach(e,p,m,f.runtime(s)));
    }
    void revisionsAreStableAndDataSurvivesDisableUpdateAndRemoval(){
        Fixture f;const auto first=f.install();auto e=f.engineOptions();auto p=f.profiles();auto m=f.connections();a::PluginRuntime::attach(e,p,m,f.runtime(first));
        const auto old=a::loadSkill(f.workspace,"sample:inspect","first","s",a::SkillInvocationSource::User,e.skills).text;
        put(first.plugins[0].dataDirectory+"/keep.txt","KEEP");
        f.manifest({{"version","2.0.0"}});put(f.package+"/skills/inspect/SKILL.md","NEW_REVISION $ARGUMENTS");const auto newer=f.install();
        CTEST_CHECK(first.revision!=newer.revision);CTEST_CHECK(first.plugins[0].root!=newer.plugins[0].root);CTEST_COMPARE(newer.plugins[0].dataDirectory,first.plugins[0].dataDirectory);
        CTEST_COMPARE(a::loadSkill(f.workspace,"sample:inspect","first","s",a::SkillInvocationSource::User,e.skills).text,old);
        f.store.setEnabled("sample",false);CTEST_CHECK(f.store.snapshot().skills.isEmpty());CTEST_COMPARE(f.store.snapshot().plugins[0].status,"disabled");
        f.store.setEnabled("sample",true);CTEST_COMPARE(f.store.snapshot().skills.size(),1);
        f.store.uninstall("sample");CTEST_CHECK(f.store.list().isEmpty());CTEST_CHECK(f.store.snapshot().plugins.isEmpty());
        CTEST_COMPARE(read(first.plugins[0].dataDirectory+"/keep.txt"),QByteArray("KEEP"));CTEST_CHECK(QFileInfo::exists(first.skills[0].path));
        CTEST_COMPARE(a::loadSkill(f.workspace,"sample:inspect","first","s",a::SkillInvocationSource::User,e.skills).text,old);
    }
    void dependencyAvailabilityAndCyclesBlockConfiguration(){
        Fixture f;f.manifest({{"dependencies",QJsonArray{"base"}}});f.install();auto s=f.store.snapshot();
        CTEST_COMPARE(s.plugins[0].status,"blocked");CTEST_CHECK(s.skills.isEmpty());
        const auto base=f.root.filePath("base");json(base+"/.claude-plugin/plugin.json",{{"name","base"},{"version","1"}});
        f.store.install(base);s=f.store.snapshot();CTEST_COMPARE(s.skills.size(),1);
        f.store.setEnabled("base",false);CTEST_CHECK(f.store.snapshot().skills.isEmpty());f.store.setEnabled("base",true);
        json(base+"/.claude-plugin/plugin.json",{{"name","base"},{"version","1"},{"dependencies",QJsonArray{"sample"}}});
        f.store.install(base);s=f.store.snapshot();CTEST_CHECK(s.skills.isEmpty());for(const auto& p:s.plugins)CTEST_COMPARE(p.status,"blocked");
        f.manifest({{"dependencies",QJsonArray{"remote@marketplace"}}});f.install();CTEST_CHECK(f.store.snapshot().skills.isEmpty());
    }
    void invalidUpdatesAreAtomicAndPackagesCannotEscape(){
        Fixture f;const auto first=f.install();const auto selected=f.store.list();
        f.manifest({{"commands","../outside.md"}});CTEST_THROWS(Error,f.store.install(f.package));CTEST_COMPARE(f.store.list(),selected);
        f.manifest({{"skills",QJsonArray{QJsonArray{true}}}});CTEST_THROWS(Error,f.store.install(f.package));
        f.manifest();CTEST_CHECK(createNativeTestLink(first.skills[0].path,f.package+"/escape"));CTEST_THROWS(Error,f.store.install(f.package));
        CTEST_CHECK(QFile::remove(f.package+"/escape"));a::PluginStoreOptions limited;limited.directory=f.root.filePath("limited");limited.maxFileBytes=8;
        a::PluginStore small(limited);CTEST_THROWS(Error,small.install(f.package));CTEST_CHECK(small.list().isEmpty());
        CancellationToken cancelled;cancelled.cancel();CTEST_THROWS(Error,f.store.install(f.package,true,cancelled));CTEST_COMPARE(f.store.list(),selected);
        CTEST_THROWS(Error,a::PluginStore({f.root.filePath("package/store")}).install(f.package));
    }
    void tamperedCacheCannotBecomeASelectedPrompt(){
        Fixture f;const auto s=f.install();auto e=f.engineOptions();auto p=f.profiles();auto m=f.connections();a::PluginRuntime::attach(e,p,m,f.runtime(s));
        put(s.skills[0].path,"TAMPERED");
        CTEST_THROWS(Error,f.store.snapshot());
        CTEST_THROWS(Error,a::loadSkill(f.workspace,"sample:inspect","","s",a::SkillInvocationSource::User,e.skills));
        CTEST_THROWS(Error,f.store.install(f.package));
    }
    void actualSkillAndSubagentUseThePluginSnapshot(){
        Fixture f;put(f.package+"/agents/reviewer.md","---\nname: reviewer\ndescription: Reviewer\ntools: [Read]\nskills: [inspect]\n---\nPLUGIN_AGENT ${CLAUDE_PLUGIN_ROOT}");
        put(f.package+"/commands/review.md","---\ndescription: Review in plugin child\ncontext: fork\nagent: reviewer\n---\nPLUGIN_FORK $ARGUMENTS");
        auto e=f.engineOptions();auto p=f.profiles();auto m=f.connections();a::PluginRuntime::attach(e,p,m,f.runtime(f.install()));
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,f.workspace);
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::SubagentOptions so;so.workingDirectory=f.workspace;so.stateDirectory=f.root.filePath("children");so.profiles=p;
        auto agents=std::make_shared<a::Subagents>(model,registry,policy,e,so);a::Subagents::attach(e,agents);
        a::Engine engine(model,registry,policy,e);const auto session=engine.createSession("fixture",f.workspace);
        a::RunRequest request{session.id};request.skill="sample:inspect";request.skillArguments="'two words'";
        const auto result=engine.run(request).result.get();CTEST_CHECK_MESSAGE(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage));CTEST_CHECK(result.text.contains("PLUGIN_SKILL two words"));
        const auto child=engine.runSubagentTool(session.id,"Agent",{{"subagent_type","sample:reviewer"},{"prompt","Review the file"}});
        CTEST_CHECK_MESSAGE(!child.isError,qPrintable(child.text));CTEST_CHECK(!agents->list(session.id).isEmpty());
        const auto& r=model->requests.last();CTEST_CHECK(r.systemPrompt.contains("PLUGIN_AGENT"));
        QString history;for(const auto& message:r.messages)history+=message.text;CTEST_CHECK(history.contains("PLUGIN_SKILL"));
        for(const auto& tool:r.tools)CTEST_CHECK(tool.name=="Read"); // No plugin reattachment widens a child profile.
        a::RunRequest fork{session.id};fork.skill="sample:review";fork.skillArguments="fork argument";
        const auto forked=engine.run(fork).result.get();CTEST_CHECK_MESSAGE(forked.status==a::RunStatus::Completed,qPrintable(forked.errorMessage));CTEST_CHECK(forked.text.contains("PLUGIN_FORK fork argument"));
    }
    void unsupportedServersAndDisabledDiscoveryAreExplicit(){
        Fixture f;
        put(f.package+"/agents/nested/file.md","---\nname: semantic-name\n---\nAgent body.");
        f.manifest({{"mcpServers",QJsonObject{{"legacy",QJsonObject{{"type","sse"},{"url","https://example.invalid/"}}},
            {"oauth",QJsonObject{{"type","http"},{"url","https://example.invalid/"},{"oauth",QJsonObject{}}}}}},
            {"lspServers",QJsonObject{{"network",QJsonObject{{"command","unused"},{"transport","tcp"},{"extensionToLanguage",QJsonObject{{".cpp","cpp"}}}}}}}});
        const auto s=f.install();CTEST_CHECK(s.mcpServers.isEmpty());CTEST_CHECK(s.lsp.servers.isEmpty());CTEST_COMPARE(s.agents[0].name,"sample:nested:semantic-name");
        CTEST_CHECK(s.plugins[0].unsupportedFeatures.contains("mcpServers.legacy:transport"));CTEST_CHECK(s.plugins[0].unsupportedFeatures.contains("mcpServers.oauth.oauth"));
        auto e=f.engineOptions();e.skills.enabled=false;auto p=f.profiles();p.enabled=false;auto m=f.connections();
        a::PluginRuntime::attach(e,p,m,f.runtime(s));CTEST_CHECK(a::discoverSkills(f.workspace,e.skills).skills.isEmpty());
        for(const auto& profile:a::discoverAgentProfiles(f.workspace,p).profiles)CTEST_CHECK(!profile.name.startsWith("sample:"));
    }
    void mcpProcessAndLspProtocolUsePackageResources(){
        Fixture f;QDir().mkpath(f.package+"/bin");CTEST_CHECK(QFile::copy(PLUGIN_PEER,f.package+"/bin/peer"));CTEST_CHECK(QFile::copy(LSP_FIXTURE,f.package+"/bin/lsp"));
        const QJsonObject peer{{"command","${CLAUDE_PLUGIN_ROOT}/bin/peer"},{"args",QJsonArray{"${CLAUDE_PLUGIN_ROOT}"}},{"env",QJsonObject{{"PLUGIN_SECRET","PRIVATE_MARKER"}}}};
        json(f.package+"/.mcp.json",{{"mcpServers",QJsonObject{{"process",peer}}}});
        json(f.package+"/.lsp.json",{{"language",QJsonObject{{"command","${CLAUDE_PLUGIN_ROOT}/bin/lsp"},{"args",QJsonArray{"","${CLAUDE_PLUGIN_DATA}/lsp.jsonl"}},{"extensionToLanguage",QJsonObject{{".cpp","cpp"}}}}}});
        const auto snapshot=f.install();auto e=f.engineOptions();auto p=f.profiles();auto m=f.connections();a::PluginRuntime::attach(e,p,m,f.runtime(snapshot));
        CTEST_COMPARE(snapshot.lsp.servers[0].name,"plugin:sample:language");
        auto registry=std::make_shared<a::ToolRegistry>();a::McpConnections connections(registry,m);
        const auto client=connections.client("plugin:sample:process");CTEST_CHECK(client);const auto result=client->callTool("echo",{});
        CTEST_COMPARE(registry->definitions()[0].name,"mcp__plugin_sample_process__echo");
        CTEST_COMPARE(a::mcpTools(client,{"host.name",{},false})[0].definition.name,"mcp__host.name__echo");
        const auto normalized=a::mcpTools(client,{"plugin:source.name:peer",{},false,true});
        CTEST_COMPARE(normalized[0].definition.name,"mcp__plugin_source_name_peer__echo");
        CTEST_COMPARE(normalized[0].definition.metadata["remote_name"],"echo");
        const auto observed=result["structuredContent"].toObject();CTEST_COMPARE(observed["root"],snapshot.plugins[0].root);CTEST_COMPARE(observed["argument"],snapshot.plugins[0].root);
        CTEST_COMPARE(observed["data"],snapshot.plugins[0].dataDirectory);CTEST_COMPARE(observed["secret"],"PRIVATE_MARKER");
        CTEST_CHECK(!QJsonDocument(QJsonObject{{"status",connections.status()}}).toJson().contains("PRIVATE_MARKER"));
        put(f.workspace+"/main.cpp","int main() { return 0; }\n");a::Lsp lsp(e.lsp);
        a::ToolContext context;context.workingDirectory=f.workspace;context.sessionId="plugin-session";
        const auto hover=lsp.query({{"operation","hover"},{"filePath","main.cpp"},{"line",1},{"character",1}},context);
        CTEST_CHECK_MESSAGE(!hover.isError,qPrintable(hover.text));CTEST_CHECK(hover.text.contains("한글"));CTEST_CHECK(QFileInfo::exists(snapshot.plugins[0].dataDirectory+"/lsp.jsonl"));
        lsp.close();connections.close();
    }
    void commandHooksReceivePluginEnvironmentWithoutPathReevaluation(){
#if defined(Q_OS_UNIX)
        Fixture f;put(f.package+"/hook.sh","#!/bin/sh\ncat > \"$CLAUDE_PLUGIN_DATA/hook.json\"\nprintf '{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"additionalContext\":\"PLUGIN_HOOK\"}}'\n");
        CTEST_CHECK(QFile::setPermissions(f.package+"/hook.sh",QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        json(f.package+"/hooks/hooks.json",{{"hooks",QJsonObject{{"PreToolUse",QJsonArray{QJsonObject{{"matcher","Read"},{"hooks",QJsonArray{QJsonObject{{"type","command"},{"command","\"${CLAUDE_PLUGIN_ROOT}/hook.sh\""}}}}}}}}}});
        const auto s=f.install();auto e=f.engineOptions();auto p=f.profiles();auto m=f.connections();a::PluginRuntime::attach(e,p,m,f.runtime(s));
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,f.workspace);put(f.workspace+"/input.txt","INPUT");
        a::ToolRunnerOptions options;options.hooks=e.hooks;a::ToolRunner runner(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),options);
        a::ToolContext context;context.workingDirectory=f.workspace;context.sessionId="hook-session";
        const auto result=runner.run({"read","Read",{{"path","input.txt"}}},context);CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
        const auto observed=QJsonDocument::fromJson(read(s.plugins[0].dataDirectory+"/hook.json")).object();CTEST_COMPARE(observed["hook_event_name"],"PreToolUse");
#endif
    }
    void authenticatedApiAndMcpExposeTheSameRedactedSnapshot(){
        Fixture f;auto e=f.engineOptions();auto p=f.profiles();auto m=f.connections();const auto s=f.install();a::PluginRuntime::attach(e,p,m,f.runtime(s));
        auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>();
        a::ApiOptions options{.engine={.decision={.enabled=false}}};options.workingDirectory=f.workspace;options.stateDirectory=f.root.filePath("api");options.engine=e;options.engine.sessionsDirectory.clear();options.clientTokens={{"society",QString(48,'a')},{"dreamscapes",QString(48,'b')}};
        a::Api api(model,registry,policy,options);
        const auto result=api.dispatch("agent.plugins.list",{},QString(48,'a')).result.get().toObject();CTEST_COMPARE(result,s.toJson());
        CTEST_COMPARE(api.dispatch("agent.plugins.list",{},QString(48,'b')).result.get().toObject(),result);
        CTEST_THROWS(Error,api.dispatch("agent.plugins.list",{},QString(48,'x')).result.get());
        CTEST_THROWS(Error,api.dispatch("agent.plugins.list",{{"install","unexpected"}},QString(48,'a')).result.get());
        auto engine=std::make_shared<a::Engine>(model,registry,policy,e);a::McpServerOptions mo;mo.engine=engine;mo.model="fixture";mo.workingDirectory=f.workspace;
        mcp::ServerSession server(a::mcpServerOptions(registry,policy,mo));
        auto rpc=[&](int id,QString method,QJsonObject params){server.receive(QJsonObject{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}});
            for(int n=0;n<400;++n)for(const auto& v:server.takeMessages(10))if(v.toObject()["id"]==id)return v.toObject();throw std::runtime_error("MCP timeout");};
        rpc(1,"initialize",{{"protocolVersion","2025-11-25"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","fixture"},{"version","1"}}}});
        server.receive(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/initialized"}});
        const auto response=rpc(2,"tools/call",{{"name","iiLocalLLM.agent.plugins.list"},{"arguments",QJsonObject{}}})["result"].toObject();
        CTEST_CHECK(!response["isError"].toBool());CTEST_COMPARE(response["structuredContent"].toObject(),result);api.close();
    }
    void cliLifecycleReturnsReviewableResults(){
        Fixture f;
        auto call=[&](const QStringList& args){QProcess p;p.setProgram(PLUGIN_CLI);p.setArguments(QStringList{"--store",f.store.directory()}+args);p.start();
            if(!p.waitForFinished(10000)||p.exitCode()!=0)throw std::runtime_error(p.readAllStandardError().toStdString());return QJsonDocument::fromJson(p.readAllStandardOutput()).object();};
        CTEST_COMPARE(call({"install",f.package})["name"],"sample");CTEST_COMPARE(call({"list"})["plugins"].toArray().size(),1);
        CTEST_CHECK(call({"disable","sample"})["changed"].toBool());CTEST_COMPARE(call({"inspect"})["plugins"].toArray()[0].toObject()["status"],"disabled");
        CTEST_CHECK(call({"enable","sample"})["changed"].toBool());CTEST_CHECK(call({"uninstall","sample"})["cache_and_data_retained"].toBool());CTEST_CHECK(call({"list"})["plugins"].toArray().isEmpty());
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) { std::cerr << "Expected a case name or --offline" << std::endl; return 1; }
    const std::string_view selection = argc == 2 ? argv[1] : "";
    const bool offline = selection == "--offline";
    agent_ctest::CTestRunner runner(offline ? std::string_view{} : selection);
    PluginTests tests;
    runner.run("additiveNamespacedDocumentsAndPrivateStatus", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.additiveNamespacedDocumentsAndPrivateStatus(); });
    runner.run("revisionsAreStableAndDataSurvivesDisableUpdateAndRemoval", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.revisionsAreStableAndDataSurvivesDisableUpdateAndRemoval(); });
    runner.run("dependencyAvailabilityAndCyclesBlockConfiguration", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.dependencyAvailabilityAndCyclesBlockConfiguration(); });
    runner.run("invalidUpdatesAreAtomicAndPackagesCannotEscape", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.invalidUpdatesAreAtomicAndPackagesCannotEscape(); });
    runner.run("tamperedCacheCannotBecomeASelectedPrompt", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.tamperedCacheCannotBecomeASelectedPrompt(); });
    runner.run("actualSkillAndSubagentUseThePluginSnapshot", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.actualSkillAndSubagentUseThePluginSnapshot(); });
    if (!offline)
        runner.run("unsupportedServersAndDisabledDiscoveryAreExplicit", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.unsupportedServersAndDisabledDiscoveryAreExplicit(); });
    if (!offline)
        runner.run("mcpProcessAndLspProtocolUsePackageResources", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.mcpProcessAndLspProtocolUsePackageResources(); });
    if (!offline)
        runner.run("commandHooksReceivePluginEnvironmentWithoutPathReevaluation", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.commandHooksReceivePluginEnvironmentWithoutPathReevaluation(); });
    runner.run("authenticatedApiAndMcpExposeTheSameRedactedSnapshot", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.authenticatedApiAndMcpExposeTheSameRedactedSnapshot(); });
    if (!offline)
        runner.run("cliLifecycleReturnsReviewableResults", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.cliLifecycleReturnsReviewableResults(); });
    return runner.result();
}

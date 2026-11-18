#include "native_link.h"
#include <iiMultimodalAgent/mcp/Server.h>
#include <iiMultimodalAgent/agent/McpServer.h>
#include <iiMultimodalAgent/agent/ShellTasks.h>
#include <iiMultimodalAgent/agent/Subagents.h>
#include <iiMultimodalAgent/agent/Teams.h>
#include <iiMultimodalAgent/agent/PermissionSettings.h>
#include <iiMultimodalAgent/agent/GraphQL.h>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonDocument>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <chrono>
#include <thread>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
using namespace std::chrono_literals;
namespace m = iiMultimodalAgent::mcp;
namespace a = iiMultimodalAgent::agent;
namespace {
QJsonObject request(int id, QString method, QJsonObject params = {}) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}
QJsonObject tool(QString name) { return {{"name", name}, {"inputSchema", QJsonObject{{"type", "object"}}}}; }
m::ServerOptions options() {
    m::ServerOptions o;
    o.lists["tools/list"] = [](const auto&) { return QJsonArray{tool("echo")}; };
    o.handlers["tools/call"] = [](const QJsonObject& p, const m::ServerRequestContext& c) {
        c.cancellation.throwIfCancelled();
        c.progress({{"progress", 1}, {"total", 2}});
        c.progress({{"progress", 2}, {"total", 2}});
        return QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", p["arguments"].toObject()["text"].toString()}}}},
            {"structuredContent", p["arguments"].toObject()}, {"_meta", QJsonObject{{"app", "fixture"}}}};
    };
    o.requestTimeoutMs = 2000;
    return o;
}
QJsonObject next(m::ServerSession& s, int expectedId = -1) {
    // Some requests persist provider-backed state or create a child agent.
    // Their correctness is independent of a two-second external-drive deadline.
    auto messages = s.takeMessages(10000);
    if (messages.size() != 1) {
        QJsonArray frames; for (const auto& message : messages) frames.append(message);
        throw std::runtime_error(QStringLiteral("Expected one response for %1, received %2: %3")
            .arg(expectedId).arg(messages.size())
            .arg(QString::fromUtf8(QJsonDocument(frames).toJson(QJsonDocument::Compact))).toStdString());
    }
    return messages[0].toObject();
}
void initialize(m::ServerSession& s, QJsonObject caps = {}, QString version = "2025-11-25") {
    s.receive(request(1, "initialize", {{"protocolVersion", version}, {"capabilities", caps},
        {"clientInfo", QJsonObject{{"name", "test-client"}, {"version", "1"}}}}));
    const auto result = next(s, 1)["result"].toObject();
    if (result["protocolVersion"] != version) throw std::runtime_error("Handshake failed");
    s.receive({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
}
QJsonObject call(m::ServerSession& s, int id, QString name, QJsonObject args = {}) {
    s.receive(request(id, "tools/call", {{"name", name}, {"arguments", args}}));
    return next(s, id)["result"].toObject();
}
class HistoryModel final : public a::Model {
public:
    std::atomic_bool waiting = false, released = false, forceWaiting = false;
    std::optional<ContextBudget> measure(const a::ModelRequest& r, const CancellationToken&) override {
        qint64 count = 100 + r.systemPrompt.size() + r.tools.size() * 40;
        for (const auto& message : r.messages) count += message.text.size() + 10;
        return ContextBudget{count, 16384};
    }
    a::ModelReply generate(const a::ModelRequest& r, const CancellationToken& token, const TextCallback&) override {
        token.throwIfCancelled();
        if (forceWaiting || (!r.messages.isEmpty() && r.messages.last().text == "hold")) {
            waiting = true;
            while (!released && !token.isCancelled()) std::this_thread::sleep_for(1ms);
            token.throwIfCancelled();
        }
        if (r.summarizing) return {"MCP conversation summary with preserved source observations", {}, {100, 10, 0, 0}};
        QStringList history;
        for (const auto& m : r.messages) if (m.role == a::MessageRole::User) history.append(m.text);
        return {history.join('|'), {}, {}};
    }
};
class SkillGraphQLModel final : public a::Model {
public:
    a::ModelReply generate(const a::ModelRequest& r,const CancellationToken& token,const TextCallback&) override {
        token.throwIfCancelled();
        if(!r.messages.isEmpty() && r.messages.last().role==a::MessageRole::Tool)
            return {"SKILL_QUERY "+r.messages.last().text};
        for(const auto& message:r.messages)
            if(message.role==a::MessageRole::User && message.text.contains("INTEGRATED_SKILL ok"))
                return {{},{{"skill-query","GraphQL",{{"endpoint","fixture"},{"query","query { item }"}}}}};
        return {"unexpected"};
    }
};
}
class McpServerTests : public QObject {
public:
    void graphqlCrudShellAndSkillShareTheMcpSession() {
        QTemporaryDir root;const auto work=root.filePath("work");CTEST_CHECK(QDir().mkpath(work+"/.claude/skills/inspect"));
        QFile skill(work+"/.claude/skills/inspect/SKILL.md");CTEST_CHECK(skill.open(QIODevice::WriteOnly));
        skill.write("---\ndescription: Inspect\ndisable-model-invocation: true\n---\nINTEGRATED_SKILL $ARGUMENTS");skill.close();
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work);
        a::GraphQLOptions graph;graph.endpoints.insert("fixture",{QUrl("http://127.0.0.1/graphql"),"Bearer secret"});
        int requests=0;registry->add(a::graphQLTool(graph,[&](const auto& endpoint,const auto& payload,const auto&,int,int){
            ++requests;if(endpoint.authorization!="Bearer secret" || payload["query"]!="query { item }")throw std::runtime_error("Invalid GraphQL request");
            return QJsonObject{{"data",QJsonObject{{"item","ok"}}}};
        }));
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);auto model=std::make_shared<SkillGraphQLModel>();
        a::EngineOptions eo{.decision={.enabled=false}};eo.sessionsDirectory=root.filePath("sessions");eo.toolSearch.enabled=false;
        auto engine=std::make_shared<a::Engine>(model,registry,policy,eo);
        a::McpServerOptions config;config.workingDirectory=work;config.engine=engine;config.model="fixture";
        m::ServerSession server(a::mcpServerOptions(registry,policy,config));initialize(server);
        CTEST_CHECK(!call(server,2,"Write",{{"path","file.txt"},{"content","first"}})["isError"].toBool());
        CTEST_CHECK(!call(server,3,"Read",{{"path","file.txt"}})["isError"].toBool());
        CTEST_CHECK(!call(server,4,"Edit",{{"path","file.txt"},{"old_string","first"},{"new_string","second"}})["isError"].toBool());
        const auto result=call(server,5,"GraphQL",{{"endpoint","fixture"},{"query","query { item }"}});
        CTEST_CHECK_MESSAGE(!result["isError"].toBool(),qPrintable(QJsonDocument(result).toJson()));
        CTEST_COMPARE(result["structuredContent"].toObject()["data"].toObject()["item"],"ok");CTEST_COMPARE(requests,1);
#ifdef Q_OS_UNIX
        const auto shell=call(server,6,"Bash",{{"command","printf integrated-cli"}});
        CTEST_CHECK_MESSAGE(!shell["isError"].toBool(),qPrintable(QJsonDocument(shell).toJson()));
#endif
        CTEST_COMPARE(call(server,7,"iiLocalLLM.agent.skills.list")["structuredContent"].toObject()["skills"].toArray().size(),1);
        const auto invoked=call(server,8,"iiLocalLLM.agent.run",{{"skill","inspect"},{"skill_arguments","ok"}});
        CTEST_CHECK_MESSAGE(invoked["structuredContent"].toObject()["text"].toString().contains("SKILL_QUERY"),qPrintable(QJsonDocument(invoked).toJson()));
        CTEST_COMPARE(requests,2);
        const auto removed=call(server,9,"Delete",{{"path","file.txt"}});
        CTEST_CHECK_MESSAGE(!removed["isError"].toBool(),qPrintable(QJsonDocument(removed).toJson()));
        CTEST_CHECK(!QFileInfo::exists(work+"/file.txt"));server.close();
    }
    void procedureControlIsConnectionBoundAndAvailableDuringAgentRun() {
        QTemporaryDir root;const auto workspace=root.filePath("workspace");QDir().mkpath(workspace);
        auto model=std::make_shared<HistoryModel>();auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo{.decision={.enabled=false}};eo.sessionsDirectory=root.filePath("sessions");eo.compaction.automatic=false;
        a::ProcedureOptions config;config.intercept={"model"};eo.procedures=std::make_shared<a::Procedures>(config);
        auto engine=std::make_shared<a::Engine>(model,registry,policy,eo);a::McpServerOptions options;options.workingDirectory=workspace;options.engine=engine;options.model="local";
        auto bridge=a::mcpServerOptions(registry,policy,options);m::ServerSession first(bridge),second(bridge);initialize(first);initialize(second);
        CTEST_CHECK(bridge.experimentalCapabilities.contains("iisacc/procedures"));
        first.receive(request(2,"tools/call",{{"name","iiLocalLLM.agent.run"},{"arguments",QJsonObject{{"prompt","candidate"}}}}));
        QString procedureId;int sequence=3;
        CTEST_WAIT_VERIFY(([&]{first.receive(request(sequence++,"iisacc/procedures/list"));const auto page=next(first)["result"].toObject();for(const auto& value:page["procedures"].toArray())if(value.toObject()["phase"]=="waiting"){procedureId=value.toObject()["procedure_id"].toString();return true;}return false;})(),3000);
        const QJsonObject response{{"action","replace"},{"output",QJsonObject{{"text","MCP host return"},{"tool_calls",QJsonArray{}}}}};
        second.receive(request(2,"iisacc/procedures/respond",{{"procedure_id",procedureId},{"response",response}}));CTEST_CHECK(next(second).contains("error"));
        first.receive(request(sequence,"iisacc/procedures/respond",{{"procedure_id",procedureId},{"response",response}}));
        bool acknowledged=false,finished=false;
        CTEST_WAIT_VERIFY(([&]{for(const auto& frame:first.takeMessages(100)){const auto message=frame.toObject();if(message["id"]==sequence)acknowledged=message["result"].toObject()["accepted"].toBool();if(message["id"]==2)finished=message["result"].toObject()["structuredContent"].toObject()["text"]=="MCP host return";}return acknowledged&&finished;})(),3000);
        first.close();second.close();
    }
    void teamMcpRoutesBindOnlyTheCallingConnection(){
        QTemporaryDir root;const auto work=root.filePath("work");CTEST_CHECK(QDir().mkpath(work));
        auto model=std::make_shared<HistoryModel>();auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo{.decision={.enabled=false}};eo.sessionsDirectory=root.filePath("sessions");eo.toolSearch.enabled=false;eo.projectContext.enabled=false;
        a::TeamsOptions config;config.workingDirectory=work;auto teams=std::make_shared<a::Teams>(model,registry,policy,eo,config);a::Teams::attach(eo,teams);
        auto engine=std::make_shared<a::Engine>(model,registry,policy,eo);a::McpServerOptions bridge;bridge.workingDirectory=work;bridge.engine=engine;bridge.model="fixture";
        const auto server=a::mcpServerOptions(registry,policy,bridge);m::ServerSession first(server),second(server);initialize(first);initialize(second);
        first.receive(request(2,"tools/list"));const auto definitions=next(first)["result"].toObject()["tools"].toArray();int count=0;
        for(const auto& v:definitions){const auto t=v.toObject();if(t["name"].toString().startsWith("iiLocalLLM.agent.teams.")){
            ++count;CTEST_CHECK(!t["inputSchema"].toObject()["properties"].toObject().contains("session_id"));
            if(t["name"]=="iiLocalLLM.agent.teams.spawn")CTEST_CHECK(t["inputSchema"].toObject()["required"].toArray().contains("name"));}}
        CTEST_COMPARE(count,8);
        const auto created=call(first,3,"iiLocalLLM.agent.teams.create",{{"team_name","mcp"}});CTEST_CHECK_MESSAGE(!created["isError"].toBool(),QJsonDocument(created).toJson().constData());
        CTEST_CHECK(call(second,2,"iiLocalLLM.agent.teams.status")["structuredContent"].toObject()["team"].isNull());
        const auto child=call(first,4,"iiLocalLLM.agent.teams.spawn",{{"name","reader"},{"prompt","MCP_TEAM"}});CTEST_CHECK_MESSAGE(!child["isError"].toBool(),QJsonDocument(child).toJson().constData());
        // Poll the observed child state; persistence can outlast one wait call.
        int waitRequest=1000;
        CTEST_WAIT_VERIFY(call(first,waitRequest++,"iiLocalLLM.agent.teams.wait",{{"timeout_ms",250}})
            ["structuredContent"].toObject()["idle"].toBool(),10000);
        CTEST_CHECK(call(first,6,"iiLocalLLM.agent.teams.inbox")["structuredContent"].toObject()["total"].toInt()>0);
        CTEST_CHECK(call(second,3,"iiLocalLLM.agent.teams.send",{{"to","reader"},{"summary","forged"},{"message","foreign"}})["isError"].toBool());
        CTEST_CHECK(call(second,4,"iiLocalLLM.agent.teams.status",{{"session_id",child["structuredContent"].toObject()["session_id"]}})["isError"].toBool());
        CTEST_CHECK(server.controlHandlers.contains("iisacc/teams/status"));
        model->forceWaiting=true;
        first.receive(request(40,"tools/call",{{"name","iiLocalLLM.agent.run"},{"arguments",QJsonObject{{"prompt","hold"}}}}));CTEST_WAIT_DEFAULT(model->waiting.load());
        first.receive(request(41,"iisacc/teams/status"));const auto control=next(first);CTEST_COMPARE(control["id"],41);
        CTEST_COMPARE(control["result"].toObject()["structuredContent"].toObject()["team"].toObject()["team_name"],"mcp");
        model->released=true;CTEST_COMPARE(next(first)["id"],40);
        CTEST_CHECK(!call(first,7,"iiLocalLLM.agent.teams.delete")["isError"].toBool());first.close();second.close();engine->close();teams->close();
    }
    void clearControlInterruptsActiveRunAndDoesNotAcceptForeignSessionIds() {
        QTemporaryDir root;auto registry=std::make_shared<a::ToolRegistry>();auto model=std::make_shared<HistoryModel>();
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);a::EngineOptions engineConfig{.decision={.enabled=false}};engineConfig.sessionsDirectory=root.filePath("sessions");
        QStringList starts;engineConfig.hooks.append([&](const a::HookInput& input,const CancellationToken&){
            if(input.kind==a::HookKind::SessionStart)starts.append(input.context["source"].toString());return a::HookResult{};});
        auto engine=std::make_shared<a::Engine>(model,registry,policy,engineConfig);
        a::McpServerOptions config;config.workingDirectory=root.path();config.engine=engine;config.model="fixture";
        auto options=a::mcpServerOptions(registry,policy,config);m::ServerSession first(options),second(options);initialize(first);initialize(second);
        CTEST_CHECK(call(second,2,"iiLocalLLM.agent.clear")["isError"].toBool());
        first.receive(request(2,"tools/call",{{"name","iiLocalLLM.agent.run"},{"arguments",QJsonObject{{"prompt","hold"}}}}));
        CTEST_WAIT_DEFAULT(model->waiting.load());
        first.receive(request(3,"tools/call",{{"name","iiLocalLLM.agent.clear"},{"arguments",QJsonObject{}}}));
        QJsonObject run,cleared;QElapsedTimer timer;timer.start();
        while((run.isEmpty()||cleared.isEmpty())&&timer.elapsed()<3000)for(const auto& value:first.takeMessages(20)) {
            const auto message=value.toObject();if(message["id"]==2)run=message;if(message["id"]==3)cleared=message["result"].toObject();
        }
        model->released=true;
        CTEST_CHECK_MESSAGE(run.contains("error"),qPrintable(QString::fromUtf8(QJsonDocument(run).toJson())));CTEST_CHECK(cleared["structuredContent"].toObject()["complete"].toBool());
        CTEST_COMPARE(run["error"].toObject()["data"].toObject()["error_code"],"cancelled");
        const auto id=cleared["structuredContent"].toObject()["session_id"].toString();CTEST_CHECK(engine->session(id).messages.isEmpty());
        CTEST_COMPARE(starts,(QStringList{"startup","clear"}));
        CTEST_CHECK(call(second,3,"iiLocalLLM.agent.clear",{{"session_id",id}})["isError"].toBool());
        CTEST_COMPARE(call(first,4,"iiLocalLLM.agent.run",{{"prompt","fresh"}})["structuredContent"].toObject()["text"],"fresh");
        first.close();second.close();
    }
    void sessionEndFollowsConversationReplacementAndConnectionClose() {
        QTemporaryDir root;const auto workspace=root.filePath("workspace");QDir().mkpath(workspace);
        auto model=std::make_shared<HistoryModel>();auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo{.decision={.enabled=false}};eo.sessionsDirectory=root.filePath("sessions");eo.compaction.automatic=false;
        QJsonArray ended;eo.hooks.append([&](const a::HookInput& input,const CancellationToken&){
            if(input.kind==a::HookKind::SessionEnd)ended.append(QJsonObject{{"id",input.sessionId},{"reason",input.context["reason"]}});
            return a::HookResult{};});
        auto engine=std::make_shared<a::Engine>(model,registry,policy,eo);a::McpServerOptions options;options.workingDirectory=workspace;options.engine=engine;options.model="local";
        auto bridge=a::mcpServerOptions(registry,policy,options);m::ServerSession first(bridge),second(bridge);initialize(first);initialize(second);
        const auto old=call(first,2,"iiLocalLLM.agent.run",{{"prompt","one"}})["structuredContent"].toObject().value("session_id");
        const auto other=call(second,2,"iiLocalLLM.agent.run",{{"prompt","two"}})["structuredContent"].toObject().value("session_id");
        CTEST_CHECK(ended.isEmpty());const auto fresh=call(first,3,"iiLocalLLM.agent.run",{{"prompt","fresh"},{"new_session",true}})["structuredContent"].toObject().value("session_id");
        CTEST_CHECK(old!=fresh);CTEST_COMPARE(ended.size(),1);CTEST_COMPARE(ended.first().toObject()["id"],old);CTEST_COMPARE(ended.first().toObject()["reason"],"clear");
        first.close();CTEST_COMPARE(ended.size(),2);CTEST_COMPARE(ended.last().toObject()["id"],fresh);CTEST_COMPARE(ended.last().toObject()["reason"],"other");
        first.close();CTEST_COMPARE(ended.size(),2);second.close();CTEST_COMPARE(ended.size(),3);CTEST_COMPARE(ended.last().toObject()["id"],other);
        engine->close();CTEST_COMPARE(ended.size(),3);CTEST_CHECK(!engine->session(old.toString()).messages.isEmpty());
    }
    void permissionInspectionAndFileReloadUseHostWorkspace() {
        QTemporaryDir root;const auto workspace=root.filePath("workspace");CTEST_CHECK(QDir().mkpath(workspace+"/.claude"));
        const auto path=workspace+"/.claude/settings.json";
        auto save=[&](const char* behavior) { QFile file(path);if(!file.open(QIODevice::WriteOnly))throw std::runtime_error("fixture");file.write(QJsonDocument(QJsonObject{{"permissions",QJsonObject{{behavior,QJsonArray{"Write(/output/**)"}}}}}).toJson()); };
        save("allow");a::PermissionSettingsOptions settings;settings.workingDirectory=workspace;settings.fallbackMode=a::PermissionMode::DontAsk;
        auto policy=std::make_shared<a::SettingsPermissionPolicy>(settings);auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,workspace);
        a::McpServerOptions options;options.workingDirectory=workspace;
        m::ServerSession session(a::mcpServerOptions(registry,policy,options));initialize(session);
        auto result=call(session,2,"iiLocalLLM.agent.permissions.get");CTEST_COMPARE(result["structuredContent"].toObject()["provider"],"settings");
        const auto written=call(session,3,"Write",{{"path","output/first"},{"content","first"}});
        CTEST_CHECK_MESSAGE(!written["isError"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(written).toJson(QJsonDocument::Compact))));
        CTEST_CHECK(QFileInfo::exists(workspace+"/output/first"));save("deny");
        CTEST_CHECK(call(session,4,"Write",{{"path","output/second"},{"content","second"}})["isError"].toBool());CTEST_CHECK(!QFileInfo::exists(workspace+"/output/second"));
        CTEST_CHECK(call(session,5,"iiLocalLLM.agent.permissions.get",{{"working_directory",root.path()}})["isError"].toBool());
    }
    void subagentConnectionsAreIsolatedAndClosedChildrenStop() {
        QTemporaryDir root; const auto workspace=root.filePath("workspace");QDir().mkpath(workspace);
        auto model=std::make_shared<HistoryModel>();auto registry=std::make_shared<a::ToolRegistry>();
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo{.decision={.enabled=false}};eo.sessionsDirectory=root.filePath("sessions");eo.compaction.automatic=false;
        a::SubagentOptions so;so.workingDirectory=workspace;so.stateDirectory=root.filePath("subagents");
        so.profiles.enabled=true;so.profiles.projectBoundary=workspace;
        auto agents=std::make_shared<a::Subagents>(model,registry,policy,eo,so);a::Subagents::attach(eo,agents);
        auto engine=std::make_shared<a::Engine>(model,registry,policy,eo);
        a::McpServerOptions options;options.workingDirectory=workspace;options.engine=engine;options.model="local";
        auto bridge=a::mcpServerOptions(registry,policy,options);
        m::ServerSession first(bridge),second(bridge);initialize(first);initialize(second);
        auto profiles=call(first,20,"iiLocalLLM.agent.agents.profiles");
        CTEST_COMPARE(profiles["structuredContent"].toObject()["profiles"].toArray().size(),3);
        CTEST_CHECK(QDir().mkpath(workspace+"/.claude/agents"));QFile profile(workspace+"/.claude/agents/check.md");CTEST_CHECK(profile.open(QIODevice::WriteOnly));profile.write("---\nname: custom\ndescription: MCP profile\n---\nInspect data.\n");profile.close();
        profiles=call(first,21,"iiLocalLLM.agent.agents.profiles");CTEST_COMPARE(profiles["structuredContent"].toObject()["profiles"].toArray().size(),4);
        const auto custom=call(first,22,"iiLocalLLM.agent.agents.run",{{"prompt","MCP_PROFILE"},{"subagent_type","custom"}});CTEST_CHECK(!custom["isError"].toBool());
        auto launched=call(first,2,"iiLocalLLM.agent.agents.run",{{"prompt","hold"},{"run_in_background",true}});
        CTEST_CHECK(!launched["isError"].toBool());const auto id=launched["structuredContent"].toObject().value("agentId");
        CTEST_WAIT_DEFAULT(model->waiting.load());
        auto foreign=call(second,2,"iiLocalLLM.agent.agents.output",{{"agent_id",id}});CTEST_CHECK(foreign["isError"].toBool());
        auto listed=call(first,3,"iiLocalLLM.agent.agents.list");
        QJsonObject state;for(const auto& value:listed["structuredContent"].toObject()["agents"].toArray())if(value.toObject()["agentId"]==id)state=value.toObject();
        CTEST_CHECK(!state.isEmpty());
        CTEST_CHECK(QDir().rename(workspace+"/.claude/agents",workspace+"/.claude/saved-agents"));
        CTEST_CHECK(createNativeTestLink(root.path(),workspace+"/.claude/agents"));
        CTEST_CHECK(call(first,23,"iiLocalLLM.agent.agents.profiles")["isError"].toBool());
        CTEST_CHECK(!call(first,24,"iiLocalLLM.agent.agents.output",{{"agent_id",id}})["isError"].toBool());
        // Use the connection's actual parent identity instead of depending on UUID sort order.
        const auto metadata=call(first,4,"iiLocalLLM.agent.session")["structuredContent"].toObject();
        const auto owner=metadata["session_id"].toString();
        first.close();CTEST_COMPARE(agents->output(owner,id.toString(),true,2000)["status"],"cancelled");
        auto other=call(second,3,"iiLocalLLM.agent.agents.list");CTEST_CHECK(other["structuredContent"].toObject()["agents"].toArray().isEmpty());
        CTEST_COMPARE(state["agentId"],id);
    }
    void skillCatalogAndInvocationUseTheConnectionConversation() {
        for(bool fork:{false,true}) {
        QTemporaryDir root; const auto path = root.filePath(".claude/skills/inspect"); CTEST_CHECK(QDir().mkpath(path));
        QFile file(path + "/SKILL.md"); CTEST_CHECK(file.open(QIODevice::WriteOnly));
        file.write(QByteArray("---\ndescription: Inspect\ndisable-model-invocation: true\n")+(fork?"context: fork\n":"")+"---\nMCP_SKILL $ARGUMENTS"); file.close();
        auto registry = std::make_shared<a::ToolRegistry>(); auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo{.decision={.enabled=false}}; eo.sessionsDirectory = root.filePath("sessions");
        auto model=std::make_shared<HistoryModel>();std::shared_ptr<a::Subagents> agents;QTemporaryDir children;
        if(fork) {
            a::SubagentOptions so;so.workingDirectory=root.path();so.stateDirectory=children.filePath("state");
            agents=std::make_shared<a::Subagents>(model,registry,policy,eo,so);a::Subagents::attach(eo,agents);
        }
        auto engine = std::make_shared<a::Engine>(model, registry, policy, eo);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.engine = engine; config.model = "fixture";
        const auto options = a::mcpServerOptions(registry, policy, config);
        m::ServerSession first(options), second(options); initialize(first); initialize(second);
        CTEST_COMPARE(call(first, 2, "iiLocalLLM.agent.skills.list")["structuredContent"].toObject()["skills"].toArray().size(), 1);
        const auto value = call(first, 3, "iiLocalLLM.agent.run", {{"skill", "inspect"}, {"skill_arguments", "private args"}});
        CTEST_CHECK_MESSAGE(!value["isError"].toBool(), qPrintable(QJsonDocument(value).toJson()));
        CTEST_CHECK(value["structuredContent"].toObject()["text"].toString().contains("MCP_SKILL private args"));
        const auto other = call(second, 2, "iiLocalLLM.agent.run", {{"prompt", "other"}});
        CTEST_CHECK(!other["structuredContent"].toObject()["text"].toString().contains("private args"));
        CTEST_CHECK(call(first, 4, "iiLocalLLM.agent.run", {{"skill", "missing"}})["isError"].toBool());
        CTEST_CHECK(call(first, 5, "iiLocalLLM.agent.inputs.run", {{"skill", "inspect"}})["isError"].toBool());
        if(fork) {
            const auto state=call(first,6,"iiLocalLLM.agent.session",{{"include_messages",true}})["structuredContent"].toObject();
            CTEST_COMPARE(state["messages"].toArray().size(),2);
            const auto jobs=call(first,7,"iiLocalLLM.agent.agents.list")["structuredContent"].toObject()["agents"].toArray();CTEST_COMPARE(jobs.size(),1);
        }
        }
    }
    void urgentInputBypassesTheRunningConversationLock() {
        QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>();
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass); auto model = std::make_shared<HistoryModel>();
        a::EngineOptions eo{.decision={.enabled=false}}; eo.sessionsDirectory = root.filePath("sessions");
        auto engine = std::make_shared<a::Engine>(model, registry, policy, eo);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.engine = engine; config.model = "fixture";
        const auto options = a::mcpServerOptions(registry, policy, config);
        m::ServerSession first(options), second(options); initialize(first); initialize(second);
        first.receive(request(10, "tools/call", {{"name", "iiLocalLLM.agent.run"}, {"arguments", QJsonObject{{"prompt", "hold"}}}}));
        CTEST_WAIT_VERIFY(model->waiting.load(), 2000);
        first.receive(request(11, "tools/call", {{"name", "iiLocalLLM.agent.inputs.enqueue"},
            {"arguments", QJsonObject{{"text", "follow-up"}, {"priority", "now"}}}}));
        QJsonObject queued, done; QElapsedTimer clock; clock.start();
        while ((queued.isEmpty() || done.isEmpty()) && clock.elapsed() < 3000) for (const auto& value : first.takeMessages(20)) {
            const auto message = value.toObject();
            if (message["id"].toInt() == 11) queued = message;
            if (message["id"].toInt() == 10) done = message;
        }
        CTEST_CHECK_MESSAGE(!queued.isEmpty() && !queued.contains("error") && !queued["result"].toObject()["isError"].toBool(), "Input must reach a running MCP agent");
        CTEST_CHECK_MESSAGE(!done.isEmpty(), "Urgent input must interrupt the current model call");
        CTEST_CHECK(done["result"].toObject()["structuredContent"].toObject()["text"].toString().endsWith("follow-up"));
        const auto saved = call(first, 12, "iiLocalLLM.agent.inputs.enqueue", {{"text", "first connection only"}});
        CTEST_CHECK(!saved["isError"].toBool());
        CTEST_COMPARE(call(second, 2, "iiLocalLLM.agent.inputs.list")["structuredContent"].toObject()["count"].toInt(), 0);
        CTEST_CHECK(!call(first, 13, "iiLocalLLM.agent.inputs.run")["isError"].toBool());
        CTEST_COMPARE(call(first, 14, "iiLocalLLM.agent.inputs.list")["structuredContent"].toObject()["count"].toInt(), 0);
    }
    void newConversationPreservesPreviousBackgroundShells() {
#if (!defined(Q_OS_UNIX) && !defined(Q_OS_WIN)) || defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
        agent_ctest::skip("Background shell execution requires a desktop host");
#endif
        QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>();
        auto shells = std::make_shared<a::ShellTasks>(root.path(), root.filePath("shells"));
        a::registerWorkspaceTools(*registry, root.path(), shells);
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions eo{.decision={.enabled=false}}; eo.sessionsDirectory = root.filePath("sessions");
        auto engine = std::make_shared<a::Engine>(std::make_shared<HistoryModel>(), registry, policy, eo);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.engine = engine; config.model = "fixture";
        m::ServerSession session(a::mcpServerOptions(registry, policy, config)); initialize(session);
        const auto started = call(session, 2, "Bash", {{"command", "sleep 30"}, {"run_in_background", true}});
        const auto taskId = started["structuredContent"].toObject()["backgroundTaskId"].toString(); CTEST_CHECK(!taskId.isEmpty());
        const auto original = call(session, 3, "iiLocalLLM.agent.session")["structuredContent"].toObject()["session_id"].toString();
        const auto reset = call(session, 4, "iiLocalLLM.agent.run", {{"prompt", "New session"}, {"new_session", true}});
        CTEST_CHECK(!reset["isError"].toBool());
        const auto fresh=reset["structuredContent"].toObject()["session_id"].toString();
        CTEST_CHECK(reset["structuredContent"].toObject()["clear"].toObject()["complete"].toBool());
        CTEST_THROWS(Error,shells->output(original,taskId,false));
        CTEST_COMPARE(call(session, 5, "ShellTaskList")["structuredContent"].toObject()["tasks"].toArray().size(),1);
        CTEST_CHECK(!call(session, 6, "TaskOutput", {{"task_id", taskId}, {"block", false}})["isError"].toBool());
        session.close();CTEST_COMPARE(shells->output(fresh,taskId,false)["task"].toObject()["status"],"killed");
    }
    void shellControlsShareAgentIdentityAndInterruptDuringRun() {
#if (!defined(Q_OS_UNIX) && !defined(Q_OS_WIN)) || defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
        agent_ctest::skip("Background shell execution requires a desktop host");
#endif
        QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>();
        auto shells = std::make_shared<a::ShellTasks>(root.path(), root.filePath("shells"));
        a::registerWorkspaceTools(*registry, root.path(), shells);
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass); auto model = std::make_shared<HistoryModel>();
        a::EngineOptions eo{.decision={.enabled=false}}; eo.sessionsDirectory = root.filePath("sessions");
        auto engine = std::make_shared<a::Engine>(model, registry, policy, eo);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.engine = engine; config.model = "fixture";
        const auto options = a::mcpServerOptions(registry, policy, config);
        m::ServerSession first(options), second(options); initialize(first); initialize(second);
        const auto started = call(first, 2, "Bash", {{"command", "printf mcp-job; sleep 30"}, {"run_in_background", true}});
        CTEST_CHECK(!started["isError"].toBool()); const auto taskId = started["structuredContent"].toObject().value("backgroundTaskId");
        CTEST_CHECK(!taskId.toString().isEmpty());
        const auto sessionId = call(first, 3, "iiLocalLLM.agent.session")["structuredContent"].toObject()["session_id"].toString();
        CTEST_CHECK(!sessionId.isEmpty());
        CTEST_COMPARE(engine->runShellTool(sessionId, "ShellTaskList").data["tasks"].toArray().size(), 1);
        CTEST_CHECK(call(second, 2, "TaskOutput", {{"task_id", taskId}, {"block", false}})["isError"].toBool());
        first.receive(request(10, "tools/call", {{"name", "iiLocalLLM.agent.run"}, {"arguments", QJsonObject{{"prompt", "hold"}}}}));
        CTEST_WAIT_VERIFY(model->waiting.load(), 2000);
        first.receive(request(11, "tools/call", {{"name", "TaskOutput"}, {"arguments", QJsonObject{{"task_id", taskId}, {"timeout", 30000}}}}));
        first.receive(request(12, "tools/call", {{"name", "TaskStop"}, {"arguments", QJsonObject{{"task_id", taskId}}}}));
        QJsonObject stop, output; QElapsedTimer clock; clock.start();
        while ((stop.isEmpty() || output.isEmpty()) && clock.elapsed() < 2000) for (const auto& value : first.takeMessages(20)) {
            const auto message = value.toObject();
            if (message["id"].toInt() == 12) stop = message["result"].toObject();
            if (message["id"].toInt() == 11) output = message["result"].toObject();
        }
        model->released = true;
        CTEST_CHECK_MESSAGE(!stop.isEmpty() && !stop["isError"].toBool(), "TaskStop must run while the model and TaskOutput are waiting");
        CTEST_COMPARE(output["structuredContent"].toObject()["task"].toObject()["status"], "killed");
        const auto remaining = engine->runShellTool(sessionId, "Bash", {{"command", "sleep 30"}, {"run_in_background", true}}).data["backgroundTaskId"].toString();
        first.close(); second.close();
        CTEST_COMPARE(shells->output(sessionId, remaining, false, 0, 0, 1024)["task"].toObject()["status"], "killed");
    }
    void taskToolsAreConnectionBoundAndShareAgentState() {
        QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>();
        auto policy = std::make_shared<a::RulePolicy>();
        a::McpServerOptions config; config.workingDirectory = root.path();
        config.taskStore = std::make_shared<a::TaskStore>(root.filePath("standalone"));
        auto serverOptions = a::mcpServerOptions(registry, policy, config);
        m::ServerSession first(serverOptions), second(serverOptions); initialize(first); initialize(second);
        auto created = call(first, 2, "TaskCreate", {{"subject", "Verify"}, {"description", "Run checks"}});
        CTEST_CHECK(!created["isError"].toBool()); CTEST_COMPARE(created["structuredContent"].toObject()["task"].toObject()["id"], "1");
        CTEST_COMPARE(call(first, 3, "TaskList")["structuredContent"].toObject()["tasks"].toArray().size(), 1);
        CTEST_CHECK(call(second, 2, "TaskList")["structuredContent"].toObject()["tasks"].toArray().isEmpty());
        CTEST_CHECK(call(second, 3, "TaskList", {{"listId", "first"}})["isError"].toBool());
        first.close();
        m::ServerSession replacement(serverOptions); initialize(replacement);
        CTEST_CHECK(call(replacement, 2, "TaskList")["structuredContent"].toObject()["tasks"].toArray().isEmpty());

        a::EngineOptions options{.decision={.enabled=false}}; options.sessionsDirectory = root.filePath("sessions"); options.taskToolsEnabled = true;
        auto enginePolicy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        config.taskStore.reset(); config.engine = std::make_shared<a::Engine>(std::make_shared<HistoryModel>(), registry, enginePolicy, options);
        config.model = "fixture";
        m::ServerSession agent(a::mcpServerOptions(registry, enginePolicy, config)); initialize(agent);
        CTEST_CHECK(!call(agent, 2, "TaskCreate", {{"subject", "Persisted"}, {"description", "Inspect actual state"}})["isError"].toBool());
        const auto id = call(agent, 3, "iiLocalLLM.agent.session")["structuredContent"].toObject()["session_id"].toString();
        CTEST_COMPARE(config.engine->runTaskTool(id, "TaskList").data["total"].toInt(), 1);
        const auto reply = call(agent, 4, "iiLocalLLM.agent.run", {{"prompt", "Inspect tasks"}});
        CTEST_CHECK(reply["structuredContent"].toObject()["text"].toString().contains("Persisted"));
        call(agent, 5, "iiLocalLLM.agent.run", {{"prompt", "New work"}, {"new_session", true}});
        CTEST_CHECK(call(agent, 6, "TaskList")["structuredContent"].toObject()["tasks"].toArray().isEmpty());
        CTEST_COMPARE(config.engine->runTaskTool(id, "TaskList").data["total"].toInt(), 1);
    }
    void lifecycleAndValidation() {
        m::ServerSession s(options());
        s.receive(request(9, "tools/list")); CTEST_COMPARE(next(s)["error"].toObject()["code"].toInt(), -32002);
        initialize(s); CTEST_CHECK(s.isInitialized()); CTEST_COMPARE(s.clientInfo()["name"].toString(), "test-client");
        s.receive(request(2, "tools/list")); CTEST_COMPARE(next(s)["result"].toObject()["tools"].toArray().size(), 1);
        s.receive(request(3, "unknown")); CTEST_COMPARE(next(s)["error"].toObject()["code"].toInt(), -32601);
        auto invalid = request(4, "tools/call"); invalid["params"] = QJsonArray{};
        s.receive(invalid); CTEST_COMPARE(next(s)["error"].toObject()["code"].toInt(), -32602);
        s.receive(request(5, "initialize")); CTEST_COMPARE(next(s)["error"].toObject()["code"].toInt(), -32600);
    }
    void progressAndStructuredResult() {
        m::ServerSession s(options()); initialize(s);
        s.receive(request(2, "tools/call", {{"name", "echo"}, {"arguments", QJsonObject{{"text", "한글 값"}}},
            {"_meta", QJsonObject{{"progressToken", "caller-token"}}}}));
        QList<QJsonValue> messages;
        QElapsedTimer timer; timer.start();
        while (messages.size() < 3 && timer.elapsed() < 5000) messages += s.takeMessages(200);
        CTEST_COMPARE(messages.size(), 3);
        CTEST_COMPARE(messages[0].toObject()["params"].toObject()["progressToken"].toString(), "caller-token");
        CTEST_COMPARE(messages[1].toObject()["params"].toObject()["progress"].toInt(), 2);
        CTEST_COMPARE(messages[2].toObject()["result"].toObject()["structuredContent"].toObject()["text"].toString(), "한글 값");
        CTEST_COMPARE(messages[2].toObject()["result"].toObject()["_meta"].toObject()["app"].toString(), "fixture");
    }
    void boundedRequestsAndCancellation() {
        auto o = options(); o.maxConcurrentRequests = 1; o.maxQueuedRequests = 0;
        std::atomic_bool started = false, stopped = false;
        o.handlers["test/wait"] = [&](const auto&, const m::ServerRequestContext& c) {
            started = true;
            while (!c.cancellation.isCancelled()) std::this_thread::sleep_for(1ms);
            stopped = true; c.cancellation.throwIfCancelled(); return QJsonObject{};
        };
        m::ServerSession s(o); initialize(s);
        s.receive(request(2, "test/wait")); CTEST_WAIT_DEFAULT(started.load());
        s.receive(request(3, "test/wait")); CTEST_COMPARE(next(s)["error"].toObject()["code"].toInt(), -32000);
        s.receive({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", QJsonObject{{"requestId", 2}}}});
        CTEST_WAIT_DEFAULT(stopped.load());
        s.receive(request(4, "ping")); CTEST_COMPARE(next(s)["id"].toInt(), 4);
        CTEST_CHECK(s.takeMessages().isEmpty());
    }
    void snapshotsAndSessionIsolation() {
        auto o = options(); o.listPageSize = 1;
        QJsonArray definitions{tool("one"), tool("two"), tool("three")};
        o.lists["tools/list"] = [&](const auto&) { return definitions; };
        m::ServerSession a(o), b(o); initialize(a); initialize(b);
        CTEST_CHECK(a.id() != b.id());
        a.receive(request(2, "tools/list")); auto first = next(a)["result"].toObject();
        const auto cursor = first["nextCursor"].toString(); CTEST_CHECK(!cursor.isEmpty());
        definitions = {tool("replacement")};
        b.receive(request(2, "tools/list", {{"cursor", cursor}})); CTEST_COMPARE(next(b)["error"].toObject()["code"].toInt(), -32602);
        a.receive(request(3, "tools/list", {{"cursor", cursor}})); auto second = next(a)["result"].toObject();
        CTEST_COMPARE(second["tools"].toArray()[0].toObject()["name"].toString(), "two");
        a.receive(request(4, "tools/list", {{"cursor", second["nextCursor"]}}));
        const auto last = next(a)["result"].toObject(); CTEST_CHECK(!last.contains("nextCursor"));
        CTEST_COMPARE(last["tools"].toArray()[0].toObject()["name"].toString(), "three");
    }
    void reverseRequestAndTimeout() {
        auto o = options();
        o.handlers["test/roots"] = [](const auto&, const m::ServerRequestContext& c) { return c.requestClient("roots/list", {}, 1000); };
        o.handlers["test/timeout"] = [](const auto&, const m::ServerRequestContext& c) { return c.requestClient("roots/list", {}, 30); };
        m::ServerSession s(o); initialize(s, {{"roots", QJsonObject{}}});
        s.receive(request(2, "test/roots")); const auto reverse = next(s);
        CTEST_COMPARE(reverse["method"].toString(), "roots/list");
        s.receive({{"jsonrpc", "2.0"}, {"id", reverse["id"]}, {"result", QJsonObject{{"roots", QJsonArray{}}}}});
        CTEST_COMPARE(next(s)["id"].toInt(), 2);
        s.receive(request(3, "test/timeout")); const auto pending = next(s); CTEST_CHECK(pending.contains("method"));
        QList<QJsonValue> messages;
        QElapsedTimer timer; timer.start();
        while (messages.size() < 2 && timer.elapsed() < 5000) messages += s.takeMessages(200);
        CTEST_COMPARE(messages.size(), 2);
        CTEST_COMPARE(messages[0].toObject()["method"].toString(), "notifications/cancelled");
        CTEST_COMPARE(messages[1].toObject()["id"].toInt(), 3); CTEST_CHECK(messages[1].toObject().contains("error"));
    }
    void legacyBatchAndModernRejection() {
        m::ServerSession legacy(options()); initialize(legacy, {}, "2025-03-26");
        legacy.receiveBatch({request(2, "tools/list"), request(3, "ping"), 42,
            QJsonObject{{"jsonrpc", "2.0"}, {"method", "notifications/example"}}});
        const auto messages = legacy.takeMessages(2000); CTEST_COMPARE(messages.size(), 1); CTEST_CHECK(messages[0].isArray());
        const auto replies = messages[0].toArray(); CTEST_COMPARE(replies.size(), 3);
        QSet<int> ids; for (const auto& value : replies) if (!value.toObject()["id"].isNull()) ids.insert(value.toObject()["id"].toInt());
        CTEST_COMPARE(ids, (QSet<int>{2, 3})); CTEST_COMPARE(legacy.takeNotifications().size(), 1);
        m::ServerSession modern(options()); initialize(modern);
        modern.receiveBatch({request(2, "ping")}); CTEST_COMPARE(next(modern)["error"].toObject()["code"].toInt(), -32600);
        modern.receive(request(3, "ping")); CTEST_COMPARE(next(modern)["id"].toInt(), 3);
    }
    void oversizedLegacyResponseKeepsBatchMembership() {
        auto o = options(); o.maxMessageBytes = 1024; o.maxQueuedBytes = 4096;
        o.handlers["test/large"] = [](const auto&, const auto&) {
            return QJsonObject{{"text", QString(2048, 'x')}};
        };
        m::ServerSession s(o); initialize(s, {}, "2025-03-26");
        s.receiveBatch({request(2, "test/large"), request(3, "ping")});
        const auto frames = s.takeMessages(2000);
        CTEST_COMPARE(frames.size(), 1); CTEST_CHECK(frames[0].isArray());
        const auto replies = frames[0].toArray(); CTEST_COMPARE(replies.size(), 2);
        QSet<int> ids;
        for (const auto& value : replies) {
            const auto response = value.toObject(); ids.insert(response["id"].toInt());
            if (response["id"] == 2) CTEST_COMPARE(response["error"].toObject()["code"].toInt(), -32000);
            else CTEST_CHECK(response.contains("result"));
        }
        CTEST_COMPARE(ids, (QSet<int>{2, 3}));
        s.receive(request(4, "ping")); CTEST_COMPARE(next(s)["id"].toInt(), 4);
    }
    void exportedToolsKeepPolicyAndContent() {
        QTemporaryDir root;
        auto registry = std::make_shared<a::ToolRegistry>();
        a::Tool value; value.definition = {"value", "Read a value", {{"type", "object"}, {"additionalProperties", false}}, {}, true, true};
        value.execute = [](const auto&, const auto&) { return a::ToolResult{"hello", {{"number", 7}}, false,
            {QJsonObject{{"type", "image"}, {"data", "eA=="}, {"mimeType", "image/png"}}}, {{"private", "payload"}}}; };
        registry->add(value);
        a::Tool denied = value; denied.definition.name = "write"; denied.definition.readOnly = false; registry->add(denied);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.appId = "com.iisacc.fixture";
        m::ServerSession s(a::mcpServerOptions(registry, std::make_shared<a::RulePolicy>(), config)); initialize(s);
        const auto result = call(s, 2, "value");
        CTEST_COMPARE(result["structuredContent"].toObject()["number"].toInt(), 7);
        CTEST_COMPARE(result["_meta"].toObject()["private"].toString(), "payload");
        const auto content = result["content"].toArray();
        CTEST_COMPARE(content[0].toObject()["type"].toString(), "text");
        CTEST_COMPARE(content[1].toObject()["type"].toString(), "image");
        CTEST_COMPARE(content.size(), 3);
        CTEST_COMPARE(QJsonDocument::fromJson(content.last().toObject()["text"].toString().toUtf8()).object(),
            result["structuredContent"].toObject());
        CTEST_CHECK(call(s, 3, "write")["isError"].toBool());
        CTEST_CHECK(call(s, 4, "value", {{"extra", true}})["isError"].toBool());
        s.receive(request(5, "tools/call", {{"name", "missing"}})); CTEST_COMPARE(next(s)["error"].toObject()["code"].toInt(), -32602);
    }
    void structuredOutputTextIsNotDuplicated() {
        QTemporaryDir root;
        for (const auto& json : QStringList{"{\"value\":7}", "{\n  \"value\": 7\n}"}) {
            for (const auto& version : QStringList{"2025-11-25", "2025-03-26"}) {
                auto registry = std::make_shared<a::ToolRegistry>();
                a::Tool value; value.definition = {"value", "Read a value", {{"type", "object"}}, {}, true, true};
                value.execute = [json](const auto&, const auto&) { return a::ToolResult{json, {{"value", 7}}}; };
                registry->add(value);
                a::McpServerOptions config; config.workingDirectory = root.path();
                m::ServerSession session(a::mcpServerOptions(registry, std::make_shared<a::RulePolicy>(), config)); initialize(session, {}, version);
                CTEST_COMPARE(call(session, 2, "value")["content"].toArray().size(), 1);
            }
        }
    }
    void manualCompactionIsConnectionBound() {
        QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>();
        a::EngineOptions engineConfig{.decision={.enabled=false}}; engineConfig.sessionsDirectory = root.filePath("sessions");
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        auto engine = std::make_shared<a::Engine>(std::make_shared<HistoryModel>(), registry, policy, engineConfig);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.engine = engine; config.model = "fixture";
        const auto serverOptions = a::mcpServerOptions(registry, policy, config);
        m::ServerSession first(serverOptions), second(serverOptions); initialize(first); initialize(second);
        call(first, 2, "iiLocalLLM.agent.run", {{"prompt", QString(1000, 'a')}});
        call(first, 3, "iiLocalLLM.agent.run", {{"prompt", QString(1000, 'b')}});
        call(first, 4, "iiLocalLLM.agent.run", {{"prompt", "Continue the task"}});
        const auto compacted = call(first, 5, "iiLocalLLM.agent.compact", {{"instructions", "Preserve unfinished work"}});
        CTEST_CHECK(!compacted.value("isError").toBool());
        CTEST_COMPARE(compacted.value("structuredContent").toObject().value("usage").toObject().value("compactions").toInt(), 1);
        const auto state = call(first, 6, "iiLocalLLM.agent.session").value("structuredContent").toObject();
        CTEST_COMPARE(state.value("message_count").toInt(), 6); CTEST_COMPARE(state.value("compaction_count").toInt(), 1);
        CTEST_CHECK(call(second, 2, "iiLocalLLM.agent.compact").value("isError").toBool());
        CTEST_CHECK(call(second, 3, "iiLocalLLM.agent.compact", {{"session_id", state.value("session_id")}}).value("isError").toBool());
    }
    void localAgentConversationsAreIsolated() {
        QTemporaryDir root;
        auto registry = std::make_shared<a::ToolRegistry>();
        a::EngineOptions engineConfig{.decision={.enabled=false}}; engineConfig.sessionsDirectory = root.filePath("sessions");
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        auto engine = std::make_shared<a::Engine>(std::make_shared<HistoryModel>(), registry, policy, engineConfig);
        a::McpServerOptions config; config.workingDirectory = root.path(); config.engine = engine; config.model = "fixture";
        const auto serverOptions = a::mcpServerOptions(registry, policy, config);
        m::ServerSession first(serverOptions), second(serverOptions); initialize(first); initialize(second);
        const auto one = call(first, 2, "iiLocalLLM.agent.run", {{"prompt", "private-first"}});
        const auto two = call(second, 2, "iiLocalLLM.agent.run", {{"prompt", "private-second"}});
        const auto resumed = call(first, 3, "iiLocalLLM.agent.run", {{"prompt", "follow-up"}});
        CTEST_CHECK(one["structuredContent"].toObject()["session_id"] != two["structuredContent"].toObject()["session_id"]);
        CTEST_COMPARE(two["structuredContent"].toObject()["text"].toString(), "private-second");
        CTEST_COMPARE(resumed["structuredContent"].toObject()["text"].toString(), "private-first|follow-up");
        CTEST_CHECK(call(second, 3, "iiLocalLLM.agent.session", {{"session_id", one["structuredContent"].toObject()["session_id"]}})["isError"].toBool());
        const auto reset = call(first, 4, "iiLocalLLM.agent.run", {{"prompt", "new"}, {"new_session", true}});
        CTEST_COMPARE(reset["structuredContent"].toObject()["text"].toString(), "new");
        CTEST_CHECK(QDir().mkpath(root.filePath("src")));
        QFile rules(root.filePath("src/AGENTS.md")); CTEST_CHECK(rules.open(QIODevice::WriteOnly));
        rules.write("Scoped MCP instructions"); rules.close();
        const auto scoped = call(first, 5, "iiLocalLLM.agent.run", {{"prompt", "scoped"}, {"context_paths", QJsonArray{"src/new.cpp"}}});
        CTEST_CHECK(scoped["structuredContent"].toObject()["text"].toString().contains("Scoped MCP instructions"));
        const auto unrelated = call(second, 4, "iiLocalLLM.agent.run", {{"prompt", "unchanged"}});
        CTEST_CHECK(!unrelated["structuredContent"].toObject()["text"].toString().contains("Scoped MCP instructions"));
        CTEST_CHECK(call(first, 6, "iiLocalLLM.agent.run", {{"prompt", "invalid"}, {"context_paths", QJsonArray{"../outside"}}})["isError"].toBool());
    }
    void resourcesPromptsSubscriptionsAndLegacyContent() {
        auto o = options();
        o.lists["resources/list"] = [](const auto&) { return QJsonArray{QJsonObject{{"uri", "fixture://value"}, {"name", "value"}}}; };
        o.lists["resources/templates/list"] = [](const auto&) { return QJsonArray{QJsonObject{{"uriTemplate", "fixture://{key}"}, {"name", "template"}}}; };
        o.handlers["resources/read"] = [](const auto& p, const auto&) { return QJsonObject{{"contents", QJsonArray{QJsonObject{{"uri", p["uri"]}, {"text", "resource text"}}}}}; };
        o.handlers["resources/subscribe"] = [](const auto& p, const auto&) { if (p["uri"] != "fixture://value") throw m::RpcError(-32602, "Unknown resource"); return QJsonObject{}; };
        o.lists["prompts/list"] = [](const auto&) { return QJsonArray{QJsonObject{{"name", "summary"}}}; };
        o.handlers["prompts/get"] = [](const auto&, const auto&) { return QJsonObject{{"messages", QJsonArray{QJsonObject{{"role", "user"},
            {"content", QJsonObject{{"type", "text"}, {"text", "Summarize this"}}}}}}}; };
        m::ServerSession first(o), second(o); initialize(first); initialize(second);
        first.receive(request(2, "resources/list")); CTEST_COMPARE(next(first)["result"].toObject()["resources"].toArray().size(), 1);
        first.receive(request(3, "resources/templates/list")); CTEST_COMPARE(next(first)["result"].toObject()["resourceTemplates"].toArray().size(), 1);
        first.receive(request(4, "resources/read", {{"uri", "fixture://value"}})); CTEST_COMPARE(next(first)["result"].toObject()["contents"].toArray().size(), 1);
        first.receive(request(5, "prompts/get", {{"name", "summary"}})); CTEST_COMPARE(next(first)["result"].toObject()["messages"].toArray().size(), 1);
        first.receive(request(6, "resources/subscribe", {{"uri", "fixture://value"}})); CTEST_CHECK(next(first).contains("result"));
        first.notify("notifications/resources/updated", {{"uri", "fixture://value"}});
        second.notify("notifications/resources/updated", {{"uri", "fixture://value"}});
        CTEST_COMPARE(next(first)["params"].toObject()["uri"].toString(), "fixture://value"); CTEST_CHECK(second.takeMessages().isEmpty());
        first.receive(request(7, "resources/unsubscribe", {{"uri", "fixture://value"}})); CTEST_CHECK(next(first).contains("result"));
        first.notify("notifications/resources/updated", {{"uri", "fixture://value"}}); CTEST_CHECK(first.takeMessages().isEmpty());
        o.handlers["tools/call"] = [](const auto&, const auto&) { return QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "resource_link"}, {"uri", "fixture://value"}, {"name", "value"}}}}, {"structuredContent", QJsonObject{{"n", 7}}}}; };
        m::ServerSession legacy(o); initialize(legacy, {}, "2025-03-26");
        const auto value = call(legacy, 2, "echo"); CTEST_CHECK(!value.contains("structuredContent"));
        CTEST_COMPARE(value["content"].toArray().size(), 2);
        for (const auto& block : value["content"].toArray()) CTEST_COMPARE(block.toObject()["type"].toString(), "text");
    }
    void requestDeadlinesLimitsAndClose() {
        auto o = options(); o.requestTimeoutMs = 30; o.maxMessageBytes = 1024; o.maxQueuedBytes = 1024;
        std::atomic_int closed = 0;
        o.onClosed = [&](const QString&) { ++closed; };
        o.handlers["test/wait"] = [](const auto&, const m::ServerRequestContext& c) {
            while (!c.cancellation.isCancelled()) std::this_thread::sleep_for(1ms);
            c.cancellation.throwIfCancelled(); return QJsonObject{};
        };
        o.handlers["test/large"] = [](const auto&, const auto&) { return QJsonObject{{"text", QString(2048, 'x')}}; };
        m::ServerSession s(o); initialize(s);
        s.receive(request(2, "test/wait")); CTEST_COMPARE(next(s)["error"].toObject()["code"].toInt(), -32000);
        s.receive(request(3, "test/large")); CTEST_COMPARE(next(s)["error"].toObject()["code"].toInt(), -32000);
        s.receive(request(4, "ping")); CTEST_CHECK(next(s).contains("result"));
        auto invalid = request(5, "ping"); invalid["id"] = QJsonValue::Null;
        s.receive(invalid); CTEST_COMPARE(next(s)["error"].toObject()["code"].toInt(), -32600);
        try { s.receive(request(4, "ping")); agent_ctest::fail(__FILE__, __LINE__, "Reused request ID accepted"); }
        catch (const Error& e) { CTEST_COMPARE(int(e.code()), int(ErrorCode::ProtocolError)); }
        CTEST_CHECK(s.isClosed()); s.close(); s.close(); CTEST_COMPARE(closed.load(), 1);
    }
    void exportedReadHistoryDoesNotCrossConnections() {
        QTemporaryDir root; QFile file(root.filePath("existing.txt")); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("original"); file.close();
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, root.path());
        a::McpServerOptions config; config.workingDirectory = root.path();
        const auto o = a::mcpServerOptions(registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass), config);
        m::ServerSession first(o), second(o); initialize(first); initialize(second);
        CTEST_CHECK(!call(first, 2, "Read", {{"path", "existing.txt"}})["isError"].toBool());
        CTEST_CHECK(call(second, 2, "Write", {{"path", "existing.txt"}, {"content", "other client"}})["isError"].toBool());
        CTEST_CHECK(!call(first, 3, "Write", {{"path", "existing.txt"}, {"content", "owner"}})["isError"].toBool());
        CTEST_CHECK(file.open(QIODevice::ReadOnly)); CTEST_COMPARE(file.readAll(), QByteArray("owner"));
    }
    void parallelReadersAndExclusiveWriter() {
        QTemporaryDir root; std::atomic_int active = 0; std::atomic_bool release = false, writer = false;
        auto registry = std::make_shared<a::ToolRegistry>();
        a::Tool read; read.definition = {"reader", "read", {{"type", "object"}}, {}, true, true};
        read.execute = [&](const auto&, const a::ToolContext& c) {
            ++active; while (!release && !c.cancellation.isCancelled()) std::this_thread::sleep_for(1ms); --active;
            c.cancellation.throwIfCancelled(); return a::ToolResult{"read"};
        }; registry->add(read);
        a::Tool write; write.definition = {"writer", "write", {{"type", "object"}}};
        write.execute = [&](const auto&, const auto&) { writer = true; return a::ToolResult{"write", {{"readers", active.load()}}}; }; registry->add(write);
        a::McpServerOptions config; config.workingDirectory = root.path();
        const auto o = a::mcpServerOptions(registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass), config);
        m::ServerSession first(o), second(o); initialize(first); initialize(second);
        first.receive(request(2, "tools/call", {{"name", "reader"}})); second.receive(request(2, "tools/call", {{"name", "reader"}}));
        CTEST_WAIT_COMPARE(active.load(), 2);
        first.receive(request(3, "tools/call", {{"name", "writer"}}));
        CTEST_CHECK(first.takeMessages(30).isEmpty()); CTEST_CHECK(!writer.load()); release = true;
        QList<QJsonValue> messages; QElapsedTimer timer; timer.start();
        while (messages.size() < 2 && timer.elapsed() < 3000) messages += first.takeMessages(100);
        CTEST_COMPARE(messages.size(), 2); CTEST_CHECK(writer.load());
        for (const auto& message : messages) if (message.toObject()["id"] == 3)
            CTEST_COMPARE(message.toObject()["result"].toObject()["structuredContent"].toObject()["readers"].toInt(), 0);
        CTEST_COMPARE(next(second)["id"].toInt(), 2);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    McpServerTests tests;
    runner.run("graphqlCrudShellAndSkillShareTheMcpSession", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.graphqlCrudShellAndSkillShareTheMcpSession(); });
    runner.run("procedureControlIsConnectionBoundAndAvailableDuringAgentRun", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.procedureControlIsConnectionBoundAndAvailableDuringAgentRun(); });
    runner.run("teamMcpRoutesBindOnlyTheCallingConnection", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.teamMcpRoutesBindOnlyTheCallingConnection(); });
    runner.run("clearControlInterruptsActiveRunAndDoesNotAcceptForeignSessionIds", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.clearControlInterruptsActiveRunAndDoesNotAcceptForeignSessionIds(); });
    runner.run("sessionEndFollowsConversationReplacementAndConnectionClose", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.sessionEndFollowsConversationReplacementAndConnectionClose(); });
    runner.run("permissionInspectionAndFileReloadUseHostWorkspace", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.permissionInspectionAndFileReloadUseHostWorkspace(); });
    runner.run("subagentConnectionsAreIsolatedAndClosedChildrenStop", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.subagentConnectionsAreIsolatedAndClosedChildrenStop(); });
    runner.run("skillCatalogAndInvocationUseTheConnectionConversation", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.skillCatalogAndInvocationUseTheConnectionConversation(); });
    runner.run("urgentInputBypassesTheRunningConversationLock", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.urgentInputBypassesTheRunningConversationLock(); });
    runner.run("newConversationPreservesPreviousBackgroundShells", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.newConversationPreservesPreviousBackgroundShells(); });
    runner.run("shellControlsShareAgentIdentityAndInterruptDuringRun", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.shellControlsShareAgentIdentityAndInterruptDuringRun(); });
    runner.run("taskToolsAreConnectionBoundAndShareAgentState", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.taskToolsAreConnectionBoundAndShareAgentState(); });
    runner.run("lifecycleAndValidation", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.lifecycleAndValidation(); });
    runner.run("progressAndStructuredResult", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.progressAndStructuredResult(); });
    runner.run("boundedRequestsAndCancellation", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.boundedRequestsAndCancellation(); });
    runner.run("snapshotsAndSessionIsolation", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.snapshotsAndSessionIsolation(); });
    runner.run("reverseRequestAndTimeout", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.reverseRequestAndTimeout(); });
    runner.run("legacyBatchAndModernRejection", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.legacyBatchAndModernRejection(); });
    runner.run("oversizedLegacyResponseKeepsBatchMembership", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.oversizedLegacyResponseKeepsBatchMembership(); });
    runner.run("exportedToolsKeepPolicyAndContent", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.exportedToolsKeepPolicyAndContent(); });
    runner.run("structuredOutputTextIsNotDuplicated", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.structuredOutputTextIsNotDuplicated(); });
    runner.run("manualCompactionIsConnectionBound", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.manualCompactionIsConnectionBound(); });
    runner.run("localAgentConversationsAreIsolated", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.localAgentConversationsAreIsolated(); });
    runner.run("resourcesPromptsSubscriptionsAndLegacyContent", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.resourcesPromptsSubscriptionsAndLegacyContent(); });
    runner.run("requestDeadlinesLimitsAndClose", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.requestDeadlinesLimitsAndClose(); });
    runner.run("exportedReadHistoryDoesNotCrossConnections", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.exportedReadHistoryDoesNotCrossConnections(); });
    runner.run("parallelReadersAndExclusiveWriter", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.parallelReadersAndExclusiveWriter(); });
    return runner.result();
}

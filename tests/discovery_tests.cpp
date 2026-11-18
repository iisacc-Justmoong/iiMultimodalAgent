#include "iiMultimodalAgent/agent/ToolSearch.h"
#include "iiMultimodalAgent/agent/Engine.h"
#include "iiMultimodalAgent/agent/McpConnections.h"
#include "iiMultimodalAgent/mcp/HttpServer.h"
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <atomic>
#include <algorithm>

using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a = iiMultimodalAgent::agent;
namespace m = iiMultimodalAgent::mcp;
namespace {
a::Tool tool(QString name, QString description, bool deferred = true) {
    a::Tool value;
    value.definition = {name, description, {{"type", "object"}}, {}, true, true, false, deferred};
    value.execute = [name](const QJsonObject&, const a::ToolContext&) { return a::ToolResult{name}; };
    return value;
}
bool visible(const a::ModelRequest& request, const QString& name) {
    return std::any_of(request.tools.begin(), request.tools.end(), [&](const auto& t) { return t.name == name; });
}
class Model final : public a::Model {
public:
    QList<a::ModelReply> replies;
    QList<a::ModelRequest> requests;
    std::function<void(int)> afterRequest;
    a::ModelReply generate(const a::ModelRequest& r, const CancellationToken&, const TextCallback&) override {
        requests.append(r);
        if (afterRequest) afterRequest(requests.size());
        if (replies.isEmpty()) throw Error(ErrorCode::RuntimeFailure, "Unexpected test model request");
        return replies.takeFirst();
    }
};
void write(const QString& path, const QJsonObject& object) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) throw std::runtime_error("Fixture write failed");
    file.write(QJsonDocument(object).toJson());
}
a::EngineOptions engineOptions(const QString& state) {
    a::EngineOptions o{.decision={.enabled=false}}; o.sessionsDirectory = state; o.projectContext.enabled = false;
    o.compaction.automatic = false; return o;
}
}
class DiscoveryTests : public QObject {
public:
    void keywordAndDirectSearch();
    void selectionIsScopedAndPersisted();
    void sameTurnAndChangedSchemaCannotBypassDiscovery();
    void atomicReplacementPreservesSnapshots();
    void configuredHttpToolsAndNotificationRefresh();
    void configErrorsAndCredentialsStayIsolated();
    void configuredStdioReloadAndRecovery();
    void failedConnectionAndDiscoveryDiagnostics();
    void perServerDeadlinesAndReload();
    void validationBeforeEffects();
    void selectionBoundsPolicyAndCompaction();
    void closeDoesNotCancelTheHostsToken();
};
void DiscoveryTests::keywordAndDirectSearch() {
    QList<a::ToolDefinition> catalog{
        tool("mcp__society__list_files", "List project files 파일 목록").definition,
        tool("mcp__dreamscapes__generate", "Generate an image 이미지 생성").definition,
        tool("Read", "Read local text", false).definition};
    const auto found = a::searchTools(catalog, "+society files", 1);
    CTEST_COMPARE(found.data["matches"].toArray(), QJsonArray{"mcp__society__list_files"});
    CTEST_COMPARE(found.data["tools"].toArray().first().toObject()["inputSchema"].toObject(), catalog.first().inputSchema);
    CTEST_COMPARE(a::searchTools(catalog, "이미지", 5).data["matches"].toArray(), QJsonArray{"mcp__dreamscapes__generate"});
    CTEST_COMPARE(a::searchTools(catalog, "mcp__society", 5).data["matches"].toArray(), QJsonArray{"mcp__society__list_files"});
    const auto selected = a::searchTools(catalog, "select:Read,mcp__dreamscapes__generate,missing", 5);
    CTEST_COMPARE(selected.data["matches"].toArray(), (QJsonArray{"Read", "mcp__dreamscapes__generate"}));
    CTEST_COMPARE(selected.data["missing"].toArray(), QJsonArray{"missing"});
    CTEST_THROWS(Error, a::searchTools(catalog, "", 1));
    CTEST_THROWS(Error, a::searchTools(catalog, "files", 0));
}
void DiscoveryTests::selectionIsScopedAndPersisted() {
    QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>();
    registry->add(tool("remote", "Remote action"));
    auto model = std::make_shared<Model>();
    model->replies = {{{}, {{"search", "ToolSearch", {{"query", "select:remote"}}}}},
        {{}, {{"call", "remote", {}}}}, {"done", {}}};
    const auto options = engineOptions(root.filePath("sessions")); QString id, forkId, emptyFork;
    {
        a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
        id = engine.createSession("model://test", root.path()).id;
        CTEST_COMPARE(engine.run({id, "Find remote and call it"}).result.get().status, a::RunStatus::Completed);
        CTEST_CHECK(visible(model->requests[0], "ToolSearch")); CTEST_CHECK(!visible(model->requests[0], "remote"));
        CTEST_CHECK(visible(model->requests[1], "remote"));
        CTEST_COMPARE(engine.session(id).messages[4].text, "remote");
        forkId = engine.forkSession(id).id;
        emptyFork = engine.forkSession(id, engine.session(id).messages.first().id).id;
        const auto other = engine.createSession("model://test", root.path());
        model->replies = {{"other", {}}};
        CTEST_COMPARE(engine.run({other.id, "No prior search"}).result.get().status, a::RunStatus::Completed);
        CTEST_CHECK(!visible(model->requests.last(), "remote"));
    }
    a::Engine resumed(model, registry, std::make_shared<a::RulePolicy>(), options);
    for (const auto& session : {id, forkId, emptyFork}) {
        model->replies = {{"resumed", {}}};
        CTEST_COMPARE(resumed.run({session, "Continue"}).result.get().status, a::RunStatus::Completed);
        CTEST_COMPARE(visible(model->requests.last(), "remote"), session != emptyFork);
    }
}
void DiscoveryTests::sameTurnAndChangedSchemaCannotBypassDiscovery() {
    QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>(); registry->add(tool("remote", "Remote action"));
    auto model = std::make_shared<Model>();
    model->replies = {{{}, {{"search", "ToolSearch", {{"query", "select:remote"}}}, {"premature", "remote", {}}}}, {"done", {}}};
    a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), engineOptions(root.filePath("sessions")));
    const auto id = engine.createSession("model://test", root.path()).id;
    CTEST_COMPARE(engine.run({id, "Search then call in the same batch"}).result.get().status, a::RunStatus::Completed);
    CTEST_CHECK(engine.session(id).messages[3].isError);
    CTEST_CHECK(visible(model->requests.last(), "remote"));
    auto changed = tool("remote", "Changed action");
    registry->replace({"remote"}, {changed});
    model->replies = {{{}, {{"stale", "remote", {}}}}, {"done", {}}};
    CTEST_COMPARE(engine.run({id, "Try old schema"}).result.get().status, a::RunStatus::Completed);
    CTEST_CHECK(!visible(model->requests.last(), "remote"));
    CTEST_CHECK(engine.session(id).messages[7].isError);
}
void DiscoveryTests::atomicReplacementPreservesSnapshots() {
    a::ToolRegistry registry; registry.add(tool("owned", "Original")); registry.add(tool("other", "Host tool"));
    auto snapshot = registry.snapshot();
    CTEST_THROWS(Error, registry.replace({"owned"}, {tool("other", "Collision")}));
    CTEST_COMPARE(registry.get("owned").definition.description, "Original");
    registry.replace({"owned"}, {tool("new", "Replacement")});
    CTEST_THROWS(Error, registry.get("owned"));
    CTEST_COMPARE(snapshot->get("owned").definition.description, "Original");
    CTEST_COMPARE(registry.definitions().size(), 2);
}
void DiscoveryTests::configuredHttpToolsAndNotificationRefresh() {
    QTemporaryDir root; auto exported = std::make_shared<a::ToolRegistry>(); exported->add(tool("one", "First", false));
    m::HttpServerOptions transport; transport.authenticate = [](const QByteArray& token) { return token == "valid-token" ? "app" : QString{}; };
    m::HttpServer server([exported](const QString&) {
        m::ServerOptions o; o.lists["tools/list"] = [exported](const m::ServerRequestContext&) { QJsonArray out; for (const auto& t : exported->definitions()) out.append(a::toJson(t)); return out; };
        o.handlers["tools/call"] = [](const QJsonObject& params, const m::ServerRequestContext&) {
            return QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", params["name"]}}}}};
        }; return o;
    }, transport);
    CTEST_CHECK(server.listen());
    const auto config = root.filePath(".mcp.json");
    write(config, {{"mcpServers", QJsonObject{{"app", QJsonObject{{"type", "http"}, {"url", server.endpoint().toString()},
        {"headers", QJsonObject{{"Authorization", "Bearer ${TOKEN}"}}}, {"appId", "com.iisacc.example"}}}}}});
    auto registry = std::make_shared<a::ToolRegistry>(); registry->add(tool("local", "Host", false));
    a::McpConnectionOptions o; o.workingDirectory = root.path(); o.configFiles = {config}; o.environment.insert("TOKEN", "valid-token"); o.refreshIntervalMs = 25;
    a::McpConnections connections(registry, o);
    CTEST_CHECK_MESSAGE(connections.status().first().toObject()["state"] == "ready", QJsonDocument(connections.status()).toJson().constData());
    const auto remote = registry->get("mcp__app__one"); CTEST_CHECK(remote.definition.deferred);
    CTEST_COMPARE(remote.definition.metadata["app_id"].toString(), "com.iisacc.example");
    CTEST_CHECK(!remote.definition.readOnly); // Remote hints never grant local permission.
    auto frozen = registry->snapshot();
    exported->replace({"one"}, {tool("two", "Second", false)});
    const auto ids = server.sessionIds("app"); CTEST_COMPARE(ids.size(), 1);
    server.notify(ids.first(), "notifications/tools/list_changed");
    CTEST_WAIT_VERIFY(registry->definitions().size() == 2 && registry->definitions().last().name == "mcp__app__two", 5000);
    CTEST_COMPARE(frozen->get("mcp__app__one").definition.name, "mcp__app__one");
    connections.close(); CTEST_COMPARE(registry->definitions().size(), 1);
}
void DiscoveryTests::configErrorsAndCredentialsStayIsolated() {
    QTemporaryDir root; const auto path = root.filePath(".mcp.json");
    write(path, {{"mcpServers", QJsonObject{{"unavailable", QJsonObject{{"type", "http"}, {"url", "http://127.0.0.1:1/mcp"},
        {"headers", QJsonObject{{"Authorization", "Bearer very-secret-token"}}}}}}}});
    a::McpConnectionOptions o; o.workingDirectory = root.path(); o.configFiles = {path}; o.refreshIntervalMs = 0; o.limits.initializeTimeoutMs = 200;
    auto registry = std::make_shared<a::ToolRegistry>(); a::McpConnections connections(registry, o);
    const auto status = QJsonDocument(connections.status()).toJson();
    CTEST_CHECK(!status.contains("very-secret-token")); CTEST_CHECK(!status.contains("Authorization"));
    CTEST_COMPARE(connections.status().first().toObject()["state"].toString(), "failed");
    CTEST_COMPARE(connections.status().first().toObject()["error_phase"], "connect");
    CTEST_CHECK(connections.status().first().toObject().contains("error_elapsed_ms"));
    write(path, {{"mcpServers", QJsonArray{}}});
    CTEST_THROWS(Error, connections.reload());
    CTEST_COMPARE(connections.status().size(), 1);
}
void DiscoveryTests::configuredStdioReloadAndRecovery() {
    QTemporaryDir root; QDir().mkpath(root.filePath("work"));
    const auto first = root.filePath("base.json"), override = root.filePath("override.json");
    const QJsonObject entry{{"command", "${PYTHON}"}, {"args", QJsonArray{"-B", MCP_TEST_PEER}},
        {"cwd", "work"}, {"env", QJsonObject{{"IILOCAL_MCP_TEST_VALUE", "${MISSING:-literal $(not-executed)}"}}}};
    write(first, {{"mcpServers", QJsonObject{{"app", entry}, {"disabled", QJsonObject{{"disabled", true}}}}}});
    write(override, {{"mcpServers", QJsonObject{}}});
    a::McpConnectionOptions o; o.workingDirectory = root.path(); o.configFiles = {first, override};
    o.environment.insert("PYTHON", MCP_TEST_PYTHON); o.environment.remove("MISSING"); o.refreshIntervalMs = 25; o.retryDelayMs = 25;
    o.clientOptions = [root = root.path()](const QString&) {
        m::ClientOptions client; client.roots = {QJsonObject{{"uri", QUrl::fromLocalFile(root).toString()}, {"name", "workspace"}}}; return client;
    };
    auto registry = std::make_shared<a::ToolRegistry>(); a::McpConnections connections(registry, o);
    auto client = connections.client("app"); auto state = client->request("test/state");
    CTEST_COMPARE(state["configuredValue"], "literal $(not-executed)");
    CTEST_COMPARE(QDir::fromNativeSeparators(state["cwd"].toString()), QFileInfo(root.filePath("work")).canonicalFilePath());
    CTEST_COMPARE(client->listResources().size(), 1); CTEST_COMPARE(client->listPrompts().size(), 1);
    CTEST_COMPARE(client->request("test/reverse", {{"method", "roots/list"}})["response"].toObject()["result"].toObject()["roots"].toArray().size(), 1);
    const auto old = registry->get("mcp__app__echo");
    const auto oldId = old.definition.metadata["connection_id"];
    connections.reload(); CTEST_COMPARE(connections.client("app"), client);
    CTEST_COMPARE(registry->get("mcp__app__echo").definition.metadata["connection_id"], oldId);
    try { client->request("test/exit"); } catch (const Error&) {}
    CTEST_WAIT_VERIFY(connections.client("app") != client, 5000);
    CTEST_CHECK(client->isClosed());
    CTEST_CHECK(registry->get("mcp__app__echo").definition.metadata["connection_id"] != oldId);
    CTEST_THROWS(Error, old.execute({{"value", "stale"}}, {}));
    auto replacement = entry; replacement["alwaysLoad"] = true;
    write(override, {{"mcpServers", QJsonObject{{"app", replacement}}}});
    auto recovered = connections.client("app");
    connections.refresh(); CTEST_COMPARE(connections.client("app"), recovered); // No implicit file reload.
    connections.reload(); CTEST_CHECK(recovered->isClosed()); CTEST_CHECK(!registry->get("mcp__app__echo").definition.deferred);
    auto active = connections.client("app");
    write(override, {{"mcpServers", QJsonObject{{"app", QJsonObject{{"disabled", true}}}}}});
    connections.reload(); CTEST_CHECK(active->isClosed()); CTEST_CHECK(registry->definitions().isEmpty());
    write(override, {{"mcpServers", QJsonObject{{"app", QJsonObject{{"disabled", true}, {"appId", "com.iisacc.renamed"}}}}}});
    connections.reload(); CTEST_COMPARE(connections.status().first().toObject()["app_id"].toString(), "com.iisacc.renamed");
    connections.close(); connections.close();
    CTEST_THROWS(Error, connections.reload());
}
void DiscoveryTests::validationBeforeEffects() {
    QTemporaryDir root; const auto path = root.filePath("config.json");
    const QJsonObject valid{{"command", MCP_TEST_PYTHON}, {"args", QJsonArray{"-B", MCP_TEST_PEER}}};
    int effects = 0;
    a::McpConnectionOptions o; o.workingDirectory = root.path(); o.configFiles = {path}; o.refreshIntervalMs = 0;
    o.clientOptions = [&](const QString&) { ++effects; return m::ClientOptions{}; };
    const QList<QJsonObject> invalid{
        {{"type", "sse"}, {"url", "http://localhost/mcp"}},
        {{"command", "${UNSET_DISCOVERY_VARIABLE}"}},
        {{"command", "python3"}, {"args", "not-an-array"}},
        {{"type", "http"}, {"url", "https://secret@example.com/mcp"}},
        {{"type", "http"}, {"url", "https://example.com/mcp"}, {"headers", QJsonObject{{"X-App", "value\r\nInjected: header"}}}},
        {{"type", "http"}, {"url", "https://example.com/mcp"}, {"headers", QJsonObject{{"X-App\n", "value"}}}},
        {{"command", "python3"}, {"env", QJsonObject{{"NAME\n", "value"}}}},
        {{"command", "python3"}, {"initializeTimeoutMs", 0}},
        {{"command", "python3"}, {"initializeTimeoutMs", -1}},
        {{"command", "python3"}, {"requestTimeoutMs", 1.5}},
        {{"command", "python3"}, {"requestTimeoutMs", "30000"}},
        {{"command", "python3"}, {"requestTimeoutMs", true}},
        {{"disabled", true}, {"initializeTimeoutMs", QJsonValue::Null}},
        {{"command", "python3"}, {"initializeTimeoutMs", 2147483648.0}}
    };
    o.environment.remove("UNSET_DISCOVERY_VARIABLE");
    for (const auto& bad : invalid) {
        write(path, {{"mcpServers", QJsonObject{{"a-valid", valid}, {"z-invalid", bad}}}});
        CTEST_THROWS(Error, a::McpConnections(std::make_shared<a::ToolRegistry>(), o));
        CTEST_COMPARE(effects, 0);
    }
    write(root.filePath(".mcp.json"), {{"mcpServers", QJsonObject{{"app", valid}}}});
    o.configFiles.clear();
    a::McpConnections empty(std::make_shared<a::ToolRegistry>(), o);
    CTEST_CHECK(empty.status().isEmpty()); CTEST_COMPARE(effects, 0); // Workspace files require explicit host selection.
}
void DiscoveryTests::failedConnectionAndDiscoveryDiagnostics() {
    QTemporaryDir root; const auto path = root.filePath("mcp.json"), gate = root.filePath("release-list");
    auto registry = std::make_shared<a::ToolRegistry>();
    a::McpConnectionOptions o; o.workingDirectory = root.path(); o.configFiles = {path}; o.refreshIntervalMs = 0;
    o.limits.initializeTimeoutMs = 200; o.limits.requestTimeoutMs = 150;
    QJsonObject peer{{"command", MCP_TEST_PYTHON}, {"args", QJsonArray{"-B", MCP_TEST_PEER, "hang-initialize"}},
        {"env", QJsonObject{{"IILOCAL_MCP_TEST_VALUE", "private-configuration"}}}};
    write(path, {{"mcpServers", QJsonObject{{"peer", peer}}}});
    a::McpConnections failed(registry, o);
    auto status = failed.status().first().toObject();
    CTEST_COMPARE(status["state"], "failed"); CTEST_COMPARE(status["error_code"], "timeout");
    CTEST_COMPARE(status["error_phase"], "connect"); CTEST_CHECK(status["error_elapsed_ms"].toInteger() >= 200);
    auto timeout = status["request_timeout"].toObject();
    CTEST_COMPARE(timeout["method"], "initialize"); CTEST_COMPARE(timeout["timeout_ms"].toInt(), 200);
    CTEST_CHECK(timeout["elapsed_ms"].toInteger() >= 200); CTEST_CHECK(timeout["submitted"].toBool());
    CTEST_CHECK(!QJsonDocument(status).toJson().contains("private-configuration"));
    CTEST_CHECK(registry->definitions().isEmpty()); failed.close();

    peer["args"] = QJsonArray{"-B", MCP_TEST_PEER, "gated-list", gate};
    write(path, {{"mcpServers", QJsonObject{{"peer", peer}}}});
    o.limits.initializeTimeoutMs = 10000;
    a::McpConnections discovery(registry, o);
    status = discovery.status().first().toObject();
    CTEST_COMPARE(status["state"], "failed"); CTEST_COMPARE(status["error_phase"], "discover_tools");
    timeout = status["request_timeout"].toObject();
    CTEST_COMPARE(timeout["method"], "tools/list");
    // Each page receives the remaining aggregate list budget, rounded to ms.
    CTEST_CHECK(timeout["timeout_ms"].toInt() > 0 && timeout["timeout_ms"].toInt() <= 150);
    CTEST_CHECK(timeout["elapsed_ms"].toInteger() >= timeout["timeout_ms"].toInt());
    CTEST_CHECK(status["error_elapsed_ms"].toInteger() >= timeout["elapsed_ms"].toInteger());
    CTEST_CHECK(timeout["submitted"].toBool());
    auto client = discovery.client("peer"); CTEST_CHECK(client && client->isConnected());
    CTEST_CHECK(registry->definitions().isEmpty());
    write(gate, {}); discovery.reload();
    status = discovery.status().first().toObject();
    CTEST_COMPARE(status["state"], "ready"); CTEST_COMPARE(discovery.client("peer"), client);
    CTEST_CHECK(!status.contains("error_code")); CTEST_CHECK(!status.contains("error_phase"));
    CTEST_CHECK(!status.contains("error_elapsed_ms")); CTEST_CHECK(!status.contains("request_timeout"));
    CTEST_COMPARE(registry->definitions().size(), 2);
    peer["command"] = root.filePath("nonexistent-server"); peer["args"] = QJsonArray{};
    write(path, {{"mcpServers", QJsonObject{{"peer", peer}}}}); discovery.reload();
    status = discovery.status().first().toObject();
    CTEST_COMPARE(status["state"], "failed"); CTEST_COMPARE(status["error_code"], "runtime_unavailable");
    CTEST_COMPARE(status["error_phase"], "connect"); CTEST_CHECK(status["error_elapsed_ms"].toInteger() >= 0);
    CTEST_CHECK(!status.contains("request_timeout")); CTEST_CHECK(client->isClosed());
}
void DiscoveryTests::perServerDeadlinesAndReload() {
    QTemporaryDir root; const auto path = root.filePath("mcp.json");
    auto registry = std::make_shared<a::ToolRegistry>();
    a::McpConnectionOptions o; o.workingDirectory = root.path(); o.configFiles = {path}; o.refreshIntervalMs = 0;
    o.limits.initializeTimeoutMs = 100; o.limits.requestTimeoutMs = 2000;
    QJsonObject peer{{"command", MCP_TEST_PYTHON}, {"args", QJsonArray{"-B", MCP_TEST_PEER, "delay-initialize", "0.3"}},
        {"initializeTimeoutMs", 3000}, {"requestTimeoutMs", 75}};
    write(path, {{"mcpServers", QJsonObject{{"slow", peer}}}});
    a::McpConnections connections(registry, o);
    CTEST_COMPARE(connections.status().first().toObject()["state"], "ready");
    auto client = connections.client("slow"); CTEST_CHECK(client);
    try { client->request("test/slow", {{"delay", .3}}); agent_ctest::fail(__FILE__, __LINE__, "Expected per-server deadline"); }
    catch (const m::RequestTimeoutError& error) { CTEST_COMPARE(error.timeoutMs(), 75); }
    peer["requestTimeoutMs"] = 1000;
    write(path, {{"mcpServers", QJsonObject{{"slow", peer}}}}); connections.reload();
    CTEST_CHECK(client->isClosed()); client = connections.client("slow"); CTEST_CHECK(client);
    CTEST_COMPARE(client->request("test/slow", {{"delay", .15}, {"value", "reloaded"}})["value"], "reloaded");
    // Removing the override restores the host's short initialization budget.
    peer.remove("initializeTimeoutMs");
    write(path, {{"mcpServers", QJsonObject{{"slow", peer}}}}); connections.reload();
    auto status = connections.status().first().toObject();
    CTEST_COMPARE(status["state"], "failed"); CTEST_COMPARE(status["error_phase"], "connect");
    // The same short budget also bounds QProcess startup. A process-start
    // deadline may fail before there is an initialize RPC to describe.
    if (status["error_code"] == "timeout")
        CTEST_COMPARE(status["request_timeout"].toObject()["timeout_ms"].toInt(), 100);
    else {
        CTEST_COMPARE(status["error_code"], "runtime_unavailable");
        CTEST_CHECK(!status.contains("request_timeout"));
    }
    CTEST_CHECK(client->isClosed());
}
void DiscoveryTests::selectionBoundsPolicyAndCompaction() {
    QTemporaryDir root; auto registry = std::make_shared<a::ToolRegistry>();
    registry->add(tool("first", "First")); registry->add(tool("second", "Second"));
    auto model = std::make_shared<Model>();
    auto options = engineOptions(root.filePath("sessions")); options.toolSearch.maxResults = 1; options.toolSearch.maxActiveTools = 1;
    a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Default,
        QList<a::PermissionRule>{{"second", a::PermissionBehavior::Deny}}), options);
    const auto id = engine.createSession("model://test", root.path()).id;
    model->replies = {{{}, {{"one", "ToolSearch", {{"query", "first"}}}}},
        {{}, {{"two", "ToolSearch", {{"query", "second"}}}}}, {{}, {{"denied", "second", {}}}}, {"done", {}}};
    CTEST_COMPARE(engine.run({id, "Select two tools"}).result.get().status, a::RunStatus::Completed);
    CTEST_CHECK(!visible(model->requests.last(), "first")); CTEST_CHECK(visible(model->requests.last(), "second"));
    CTEST_CHECK(engine.session(id).messages[6].isError);
    {
        a::SessionStore store(options.sessionsDirectory); auto lease = store.acquire(id);
        a::Compaction checkpoint; checkpoint.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        checkpoint.atMessageId = lease->session().messages.last().id;
        checkpoint.throughMessageId = lease->session().messages[6].id; checkpoint.summary = "Tools were selected; execution of second was denied.";
        checkpoint.retainedUserMessageId = lease->session().messages.first().id;
        checkpoint.inputTokensBefore = 100; checkpoint.inputTokensAfter = 10; lease->compact(checkpoint);
    }
    model->replies = {{"continued", {}}}; CTEST_COMPARE(engine.run({id, "Continue after summary"}).result.get().status, a::RunStatus::Completed);
    CTEST_CHECK(visible(model->requests.last(), "second")); CTEST_CHECK(!visible(model->requests.last(), "first"));
    auto eagerOptions = engineOptions(root.filePath("eager")); eagerOptions.toolSearch.enabled = false;
    a::Engine eager(model, registry, std::make_shared<a::RulePolicy>(), eagerOptions);
    model->replies = {{"all tools", {}}};
    CTEST_COMPARE(eager.run({eager.createSession("model://test", root.path()).id, "Start"}).result.get().status, a::RunStatus::Completed);
    CTEST_CHECK(visible(model->requests.last(), "first")); CTEST_CHECK(visible(model->requests.last(), "second")); CTEST_CHECK(!visible(model->requests.last(), "ToolSearch"));
}
void DiscoveryTests::closeDoesNotCancelTheHostsToken() {
    QTemporaryDir root; a::McpConnectionOptions options;
    options.workingDirectory = root.path(); options.refreshIntervalMs = 0;
    a::McpConnections connections(std::make_shared<a::ToolRegistry>(), options);
    CancellationToken token; connections.refresh(token); connections.close();
    CTEST_CHECK(!token.isCancelled());
}
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    DiscoveryTests tests;
    runner.run("keywordAndDirectSearch", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.keywordAndDirectSearch(); });
    runner.run("selectionIsScopedAndPersisted", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.selectionIsScopedAndPersisted(); });
    runner.run("sameTurnAndChangedSchemaCannotBypassDiscovery", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.sameTurnAndChangedSchemaCannotBypassDiscovery(); });
    runner.run("atomicReplacementPreservesSnapshots", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.atomicReplacementPreservesSnapshots(); });
    runner.run("configuredHttpToolsAndNotificationRefresh", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.configuredHttpToolsAndNotificationRefresh(); });
    runner.run("configErrorsAndCredentialsStayIsolated", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.configErrorsAndCredentialsStayIsolated(); });
    runner.run("configuredStdioReloadAndRecovery", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.configuredStdioReloadAndRecovery(); });
    runner.run("failedConnectionAndDiscoveryDiagnostics", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.failedConnectionAndDiscoveryDiagnostics(); });
    runner.run("perServerDeadlinesAndReload", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.perServerDeadlinesAndReload(); });
    runner.run("validationBeforeEffects", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.validationBeforeEffects(); });
    runner.run("selectionBoundsPolicyAndCompaction", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.selectionBoundsPolicyAndCompaction(); });
    runner.run("closeDoesNotCancelTheHostsToken", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.closeDoesNotCancelTheHostsToken(); });
    return runner.result();
}

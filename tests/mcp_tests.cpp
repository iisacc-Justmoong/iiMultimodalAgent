#include <iiMultimodalAgent/mcp/Client.h>
#include <iiMultimodalAgent/agent/McpTools.h>
#include <iiMultimodalAgent/agent/Engine.h>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrl>
#include <atomic>
#include <chrono>
#include <thread>

using namespace iiLocalLLM; using namespace iiMultimodalAgent;
using namespace std::chrono_literals;
namespace m = iiMultimodalAgent::mcp;
namespace {
m::StdioOptions options(QString mode = "normal") {
    m::StdioOptions o; o.program = MCP_TEST_PYTHON; o.arguments = {"-B", MCP_TEST_PEER, mode};
    o.requestTimeoutMs = 2000; return o;
}
template<class F> void checkError(F f, ErrorCode code) {
    try { f(); agent_ctest::fail(__FILE__, __LINE__, "Expected an iiLocalLLM error"); }
    catch (const Error& e) { CTEST_COMPARE(int(e.code()), int(code)); }
}
}
class McpTests : public QObject {
public:
    void lifecycleAndDiscovery() {
        m::StdioClient client(options("split"));
        CTEST_CHECK(client.isConnected()); CTEST_COMPARE(client.protocolVersion(), "2025-11-25");
        CTEST_COMPARE(client.serverInfo()["name"].toString(), "independent-peer");
        CTEST_COMPARE(client.listTools().size(), 2);
        CTEST_COMPARE(client.listResources().size(), 1); CTEST_COMPARE(client.listResourceTemplates().size(), 1);
        CTEST_COMPARE(client.readResource("test://value")["contents"].toArray()[0].toObject()["text"].toString(), "관측 값");
        client.subscribeResource("test://value"); client.unsubscribeResource("test://value");
        CTEST_COMPARE(client.listPrompts().size(), 1);
        CTEST_COMPARE(client.getPrompt("summarize", {{"topic", "한글"}})["messages"].toArray().size(), 1);
        const auto result = client.callTool("echo", {{"value", "다른 프로세스의 응답"}});
        CTEST_COMPARE(result["content"].toArray().size(), 2);
        CTEST_COMPARE(result["structuredContent"].toObject()["value"].toString(), "다른 프로세스의 응답");
        CTEST_CHECK(client.stderrTail().contains("diagnostic"));
        client.request("test/notify");
        CTEST_COMPARE(client.takeNotifications().size(), 1); CTEST_CHECK(client.takeNotifications().isEmpty());
        client.close(); CTEST_CHECK(!client.isConnected());
    }
    void negotiationAndCapabilities() {
        m::StdioClient older(options("older")); CTEST_COMPARE(older.protocolVersion(), "2025-06-18");
        checkError([] { m::StdioClient invalid(options("bad-version")); }, ErrorCode::ProtocolError);
        m::StdioClient unavailable(options("no-capabilities"));
        checkError([&] { unavailable.listTools(); }, ErrorCode::ProtocolError);
        CTEST_CHECK(unavailable.request("ping").isEmpty());
    }
    void concurrentOutOfOrderRequests() {
        m::StdioClient client(options());
        auto first = std::async(std::launch::async, [&] { return client.request("test/slow", {{"delay", .15}, {"value", "first"}}); });
        auto second = std::async(std::launch::async, [&] { return client.request("test/slow", {{"delay", .01}, {"value", "second"}}); });
        try {
            CTEST_COMPARE(second.get()["value"].toString(), "second");
            CTEST_COMPARE(first.get()["value"].toString(), "first");
        } catch (...) { qWarning().noquote() << client.stderrTail(); throw; }
        try { client.request("test/error"); agent_ctest::fail(__FILE__, __LINE__, "Expected remote error"); }
        catch (const m::RpcError& e) { CTEST_COMPARE(e.rpcCode(), -32602); CTEST_COMPARE(e.data().toObject()["field"].toString(), "x"); }
    }
    void legacyBatchesGroupAsyncReverseResponses() {
        m::ClientOptions config; config.capabilities = {{"sampling", QJsonObject{}}};
        config.requestHandlers["sampling/createMessage"] = [](const auto&, const CancellationToken& token) {
            std::this_thread::sleep_for(30ms); token.throwIfCancelled();
            return QJsonObject{{"model", "fixture"}, {"role", "assistant"}, {"content", QJsonObject{{"type", "text"}, {"text", "sampled"}}}};
        };
        m::StdioClient client(options("legacy"), config);
        CTEST_COMPARE(client.protocolVersion(), "2025-03-26"); CTEST_CHECK(client.request("test/batch")["batch"].toBool());
        CTEST_WAIT_COMPARE(client.request("test/state")["batchResponses"].toArray().size(), 1);
        const auto state = client.request("test/state");
        const auto replies = state["batchResponses"].toArray()[0].toArray(); CTEST_COMPARE(replies.size(), 3);
        QSet<int> ids; for (const auto& v : replies) ids.insert(v.toObject()["id"].toInt());
        CTEST_COMPARE(ids, (QSet<int>{901, 902, 903}));
        CTEST_COMPARE(state["unexpectedResponses"].toInt(), 0); CTEST_COMPARE(client.takeNotifications().size(), 1);
    }
    void cancellationAndTimeout() {
        m::StdioClient client(options()); CancellationToken token;
        std::promise<void> ready; auto notification = ready.get_future();
        std::atomic_bool progressMessageValid{true};
        auto cancelled = std::async(std::launch::async, [&] {
            checkError([&] { client.request("test/slow", {{"delay", .15}}, token,
                [&](const QJsonObject& p) { if (p["message"].toString() != "준비 완료") progressMessageValid = false; ready.set_value(); }); }, ErrorCode::Cancelled);
        });
        CTEST_CHECK(notification.wait_for(2s) == std::future_status::ready); CTEST_CHECK(progressMessageValid.load()); token.cancel(); cancelled.get();
        try {
            client.request("test/slow", {{"delay", .15}, {"value", "private-argument"}}, {}, {}, 30);
            agent_ctest::fail(__FILE__, __LINE__, "Expected request timeout");
        } catch (const Error& e) {
            CTEST_COMPARE(e.code(), ErrorCode::Timeout);
            CTEST_CHECK(QString::fromUtf8(e.what()).contains("test/slow"));
            CTEST_CHECK(!QString::fromUtf8(e.what()).contains("private-argument"));
            const auto* timeout = dynamic_cast<const m::RequestTimeoutError*>(&e);
            CTEST_CHECK(timeout);
            CTEST_COMPARE(timeout->method(), "test/slow"); CTEST_COMPARE(timeout->timeoutMs(), 30);
            CTEST_CHECK(timeout->elapsedMs() >= 30); CTEST_CHECK(timeout->submitted());
        }
        CTEST_COMPARE(client.request("test/state")["cancelled"].toInt(), 2);
        std::this_thread::sleep_for(200ms); // The independent peer deliberately sends late replies.
        CTEST_CHECK(client.request("ping").isEmpty()); CTEST_CHECK(client.isConnected());
    }
    void initializationTimeoutDetails() {
        auto o = options("hang-initialize"); o.initializeTimeoutMs = 200;
        try { m::StdioClient client(o); agent_ctest::fail(__FILE__, __LINE__, "Expected initialization timeout"); }
        catch (const Error& e) {
            const auto* timeout = dynamic_cast<const m::RequestTimeoutError*>(&e);
            CTEST_CHECK(timeout); CTEST_COMPARE(timeout->code(), ErrorCode::Timeout);
            CTEST_COMPARE(timeout->method(), "initialize"); CTEST_COMPARE(timeout->timeoutMs(), 200);
            CTEST_CHECK(timeout->elapsedMs() >= 200); CTEST_CHECK(timeout->submitted());
        }
    }
    void rootsAndReverseRequests() {
        QTemporaryDir directory; m::ClientOptions config;
        config.roots = {QJsonObject{{"uri", QUrl::fromLocalFile(directory.path()).toString()}, {"name", "project"}}};
        config.capabilities = {{"elicitation", QJsonObject{{"form", QJsonObject{}}}}};
        config.requestHandlers["elicitation/create"] = [](const QJsonObject& p, const CancellationToken& token) {
            token.throwIfCancelled(); return QJsonObject{{"action", "accept"}, {"content", QJsonObject{{"answer", p["message"]}}}};
        };
        m::StdioClient client(options(), config);
        const auto roots = client.request("test/reverse", {{"method", "roots/list"}})["response"].toObject()["result"].toObject()["roots"].toArray();
        CTEST_COMPARE(roots, config.roots);
        client.setRoots({});
        CTEST_COMPARE(client.request("test/state")["rootsChanges"].toInt(), 1);
        const auto response = client.request("test/reverse", {{"method", "elicitation/create"}, {"params", QJsonObject{{"message", "host answer"}}}})["response"].toObject();
        CTEST_COMPARE(response["result"].toObject()["content"].toObject()["answer"].toString(), "host answer");
        const auto unsupported = client.request("test/reverse", {{"method", "sampling/createMessage"}})["response"].toObject();
        CTEST_COMPARE(unsupported["error"].toObject()["code"].toInt(), -32601);
        checkError([&] { client.setRoots({QJsonObject{{"uri", "https://example.com"}}}); }, ErrorCode::InvalidArgument);
        checkError([&] { client.setRoots({QJsonObject{{"uri", "file:///tmp/../private"}}}); }, ErrorCode::InvalidArgument);
    }
    void reverseRequestCancellationAndReentry() {
        m::StdioClient* connection = nullptr; m::ClientOptions config;
        config.capabilities = {{"elicitation", QJsonObject{{"form", QJsonObject{}}}}};
        std::promise<void> entered, stopped; auto waiting = entered.get_future(); auto done = stopped.get_future();
        config.requestHandlers["elicitation/create"] = [&](const QJsonObject&, const CancellationToken& token) {
            connection->request("ping"); // The I/O thread must stay free while a host callback runs.
            entered.set_value();
            while (!token.isCancelled()) std::this_thread::sleep_for(2ms);
            stopped.set_value();
            return QJsonObject{{"action", "cancel"}};
        };
        m::StdioClient client(options(), config); connection = &client;
        auto pending = std::async(std::launch::async, [&] { return client.request("test/reverse", {{"method", "elicitation/create"}}); });
        CTEST_CHECK(waiting.wait_for(2s) == std::future_status::ready);
        client.request("test/cancel-host");
        CTEST_CHECK(pending.get()["cancelled"].toBool()); CTEST_CHECK(done.wait_for(2s) == std::future_status::ready);
        client.request("ping"); CTEST_COMPARE(client.request("test/state")["unexpectedResponses"].toInt(), 0);
    }
    void progressConsumerFailureAndInputValidation() {
        m::StdioClient client(options());
        checkError([&] { client.request("ping", {{"_meta", QJsonValue::Null}}); }, ErrorCode::InvalidArgument);
        checkError([&] { client.request("test/slow", {}, {}, [](const QJsonObject&) { throw std::runtime_error("observer failed"); }); }, ErrorCode::ConsumerFailure);
        client.request("ping");
        CTEST_COMPARE(client.request("test/state")["cancelled"].toInt(), 1);
        CancellationToken token; token.cancel();
        checkError([&] { client.request("test/slow", {}, token); }, ErrorCode::Cancelled);
        CTEST_CHECK(client.isConnected());
    }
    void malformedPeer(const QString& kind) { auto o = options(); o.maxMessageBytes = 4096;
        m::StdioClient client(o);
        try { client.request("test/malformed", {{"kind", kind}}); agent_ctest::fail(__FILE__, __LINE__, "Invalid peer frame accepted"); }
        catch (const Error& e) { CTEST_CHECK(e.code() == ErrorCode::ProtocolError || e.code() == ErrorCode::ResourceLimit); }
        CTEST_CHECK(!client.isConnected());
    }
    void badListsAndExit() {
        for (const auto& mode : {"duplicates", "cursor-cycle"}) {
            m::StdioClient client(options(mode));
            checkError([&] { client.listTools(); }, ErrorCode::ProtocolError);
        }
        m::StdioClient client(options());
        checkError([&] { client.request("test/exit"); }, ErrorCode::RuntimeFailure);
        CTEST_CHECK(!client.isConnected());
        checkError([&] { client.request("ping"); }, ErrorCode::ShuttingDown);
    }
    void aggregateListLimit() {
        auto o = options("large-list"); o.maxMessageBytes = 1024; o.maxQueuedBytes = 1024;
        m::StdioClient client(o);
        checkError([&] { client.listTools(); }, ErrorCode::ResourceLimit);
        CTEST_CHECK(client.request("ping").isEmpty());
    }
    void boundedQueueAndClose() {
        auto o = options(); o.maxPendingRequests = 1; m::StdioClient client(o);
        std::promise<void> ready; auto started = ready.get_future();
        auto pending = std::async(std::launch::async, [&] {
            checkError([&] { client.request("test/slow", {{"delay", 5}}, {}, [&](const QJsonObject&) { ready.set_value(); }); }, ErrorCode::ShuttingDown);
        });
        CTEST_CHECK(started.wait_for(2s) == std::future_status::ready);
        checkError([&] { client.request("ping"); }, ErrorCode::QueueFull);
        client.close(); pending.get(); CTEST_CHECK(!client.isConnected());
    }
    void agentBridgePreservesResultsAndPolicy() {
        auto client = std::make_shared<m::StdioClient>(options());
        auto registry = std::make_shared<agent::ToolRegistry>();
        for (auto tool : agent::mcpTools(client, {"app", "com.iisacc.fixture"})) registry->add(std::move(tool));
        const auto definition = registry->get("mcp__app__echo").definition;
        CTEST_CHECK(!definition.readOnly); CTEST_CHECK(!definition.concurrencySafe);
        CTEST_CHECK(definition.metadata["remote_definition"].toObject()["annotations"].toObject()["readOnlyHint"].toBool());
        QTemporaryDir directory; agent::ToolContext context{"session", "run", directory.path(), directory.path(), {}, {}};
        auto denied = agent::ToolRunner(registry, std::make_shared<agent::RulePolicy>(agent::PermissionMode::DontAsk));
        CTEST_CHECK(denied.run({"c1", "mcp__app__echo", {{"value", "denied"}}}, context).isError);
        auto runner = agent::ToolRunner(registry, std::make_shared<agent::RulePolicy>(agent::PermissionMode::Bypass));
        auto output = runner.run({"c2", "mcp__app__echo", {{"value", "observed"}}}, context);
        CTEST_CHECK(!output.isError); CTEST_COMPARE(output.content.size(), 2); CTEST_COMPARE(output.metadata["app"].toString(), "fixture");
        CTEST_COMPARE(output.data["value"].toString(), "observed");
        CTEST_CHECK(output.text.contains("\"value\":\"observed\""));
        CTEST_CHECK(runner.run({"c3", "mcp__app__echo", {{"value", "bad-output"}}}, context).isError);
        CTEST_CHECK(runner.run({"c4", "mcp__app__echo", {{"value", "tool-error"}}}, context).isError);
        struct Model final : agent::Model {
            agent::ModelReply generate(const agent::ModelRequest& request, const CancellationToken&, const std::function<bool(const QString&)>&) override {
                if (request.messages.back().role == agent::MessageRole::User)
                    return {{}, {{"remote-call", "mcp__app__echo", {{"value", request.messages.back().text}}}}, {}};
                const auto& result = request.messages.back();
                if (result.content.size() != 2 || result.metadata["app"] != "fixture") throw std::runtime_error("Lost MCP content");
                return {result.data["value"].toString(), {}, {}};
            }
        };
        agent::EngineOptions engineOptions{.decision={.enabled=false}}; engineOptions.sessionsDirectory = directory.path() + "/sessions";
        agent::Engine engine(std::make_shared<Model>(), registry,
            std::make_shared<agent::RulePolicy>(agent::PermissionMode::Bypass), engineOptions);
        const auto session = engine.createSession("fixture", directory.path());
        const auto result = engine.run({session.id, "fresh value 83"}).result.get();
        CTEST_COMPARE(int(result.status), int(agent::RunStatus::Completed)); CTEST_COMPARE(result.text, "fresh value 83");
        const auto stored = engine.session(session.id).messages[2];
        CTEST_COMPARE(stored.content.size(), 2); CTEST_COMPARE(agent::messageFromJson(agent::toJson(stored)), stored);
    }
    void nativeAdapterDoesNotDropMedia() {
        QTemporaryDir directory; ServiceOptions options; options.modelsDirectory = directory.path();
        Service service(options); agent::ServiceModel model(service); agent::ModelRequest request;
        request.model = "model://not-loaded";
        agent::Message call;call.role=agent::MessageRole::Assistant;call.toolCalls={{"media","camera",{}}};request.messages.append(call);
        agent::Message message; message.role = agent::MessageRole::Tool;message.toolCallId="media";
        message.content = {QJsonObject{{"type", "image"}, {"mimeType", "image/png"}, {"data", "AA=="}}};
        request.messages.append(message);
        checkError([&] { model.generate(request, {}, {}); }, ErrorCode::NotFound);
        request.messages.last().content[0]=QJsonObject{{"type","image"},{"mimeType","image/png"},{"data","AA="}};
        checkError([&] { model.generate(request, {}, {}); }, ErrorCode::InvalidArgument);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    McpTests tests;
    runner.run("lifecycleAndDiscovery", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.lifecycleAndDiscovery(); });
    runner.run("negotiationAndCapabilities", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.negotiationAndCapabilities(); });
    runner.run("concurrentOutOfOrderRequests", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.concurrentOutOfOrderRequests(); });
    runner.run("legacyBatchesGroupAsyncReverseResponses", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.legacyBatchesGroupAsyncReverseResponses(); });
    runner.run("cancellationAndTimeout", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.cancellationAndTimeout(); });
    runner.run("initializationTimeoutDetails", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.initializationTimeoutDetails(); });
    runner.run("rootsAndReverseRequests", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.rootsAndReverseRequests(); });
    runner.run("reverseRequestCancellationAndReentry", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.reverseRequestCancellationAndReentry(); });
    runner.run("progressConsumerFailureAndInputValidation", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.progressConsumerFailureAndInputValidation(); });
    for (const auto* kind : {"json", "batch", "utf8", "oversize", "invalid-id"}) {
        const auto name = std::string("malformedPeer:") + kind;
        runner.run(name, [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.malformedPeer(QString::fromUtf8(kind)); });
    }
    runner.run("badListsAndExit", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.badListsAndExit(); });
    runner.run("aggregateListLimit", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.aggregateListLimit(); });
    runner.run("boundedQueueAndClose", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.boundedQueueAndClose(); });
    runner.run("agentBridgePreservesResultsAndPolicy", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.agentBridgePreservesResultsAndPolicy(); });
    runner.run("nativeAdapterDoesNotDropMedia", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.nativeAdapterDoesNotDropMedia(); });
    return runner.result();
}

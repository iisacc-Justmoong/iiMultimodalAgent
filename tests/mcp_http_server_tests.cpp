#include <iiMultimodalAgent/mcp/HttpServer.h>
#include <iiMultimodalAgent/mcp/HttpClient.h>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QCoreApplication>
#include <chrono>
#include <thread>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
using namespace std::chrono_literals;
namespace m = iiMultimodalAgent::mcp;
namespace {
m::HttpServerOptions transportOptions() {
    m::HttpServerOptions options;
    options.authenticate = [](const QByteArray& token) {
        return token == "alpha-fixture-credential" ? QString("alpha")
            : token == "beta-fixture-credential" ? QString("beta") : QString{};
    };
    return options;
}
m::ServerOptions protocolOptions(const QString& principal) {
    m::ServerOptions options; options.requestTimeoutMs = 3000;
    options.lists["tools/list"] = [](const auto&) {
        return QJsonArray{QJsonObject{{"name", "echo"}, {"inputSchema", QJsonObject{{"type", "object"}}}}};
    };
    options.handlers["tools/call"] = [](const auto& p, const auto&) {
        return QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", p["arguments"].toObject()["value"]}}}}};
    };
    options.handlers["test/identity"] = [principal](const auto&, const auto& context) {
        return QJsonObject{{"principal", principal}, {"session", context.sessionId}};
    };
    options.handlers["test/reverse"] = [](const auto& p, const auto& context) {
        context.progress({{"progress", 1}});
        const auto result = context.requestClient("sampling/createMessage", p, 2000);
        context.progress({{"progress", 2}});
        return result;
    };
    return options;
}
m::HttpOptions clientOptions(const m::HttpServer& server, QByteArray credential = "alpha-fixture-credential") {
    m::HttpOptions options; options.endpoint = server.endpoint(); options.requestTimeoutMs = 3000;
    options.bearerToken = [credential] { return credential; }; return options;
}
}
class McpHttpServerTests : public QObject {
public:
    void registeredControlsHaveCapacityWhenNormalStreamsAndRetentionAreFull() {
        std::atomic_bool entered=false,released=false;
        auto http=transportOptions();http.maxStreams=2;http.maxStreamsPerSession=2;
        m::HttpServer server([&](const QString& principal) {
            auto options=protocolOptions(principal);options.maxConcurrentRequests=1;options.maxQueuedRequests=0;
            options.handlers["test/wait"]=[&](const auto&,const auto& context) {
                entered=true;while(!released&&!context.cancellation.isCancelled())std::this_thread::sleep_for(1ms);
                context.cancellation.throwIfCancelled();return QJsonObject{{"released",true}};
            };
            options.controlHandlers["test/status"]=[&](const auto&,const auto&){return QJsonObject{{"entered",entered.load()}};};
            options.controlHandlers["test/release"]=[&](const auto&,const auto&){released=true;return QJsonObject{};};
            return options;
        },http);CTEST_CHECK(server.listen());m::HttpClient client(clientOptions(server));
        CTEST_WAIT_DEFAULT(!server.sessionIds("alpha").isEmpty());
        const auto sessionId=server.sessionIds("alpha").value(0);CTEST_CHECK(!sessionId.isEmpty());
        server.notify(sessionId,"notifications/tools/list_changed");
        bool notified=false;CTEST_WAIT_DEFAULT((notified|=!client.takeNotifications().isEmpty()));
        auto waiting=std::async(std::launch::async,[&]{return client.request("test/wait");});
        struct Release {std::atomic_bool& value;~Release(){value=true;}} cleanup{released};CTEST_WAIT_DEFAULT(entered.load());
        try {CTEST_CHECK(client.request("test/status")["entered"].toBool());client.request("test/release");}
        catch(const Error& error){agent_ctest::fail(__FILE__, __LINE__, qPrintable(QString("Permission control failed at stream capacity: ")+error.what()));}
        CTEST_CHECK(waiting.get()["released"].toBool());
    }
    void authenticatedSessionsAndNotifications() {
        m::HttpServer server(protocolOptions, transportOptions()); CTEST_CHECK(server.listen());
        m::HttpClient first(clientOptions(server)), second(clientOptions(server, "beta-fixture-credential"));
        CTEST_COMPARE(first.request("test/identity")["principal"].toString(), "alpha");
        CTEST_COMPARE(second.request("test/identity")["principal"].toString(), "beta");
        CTEST_CHECK(first.request("test/identity")["session"] != second.request("test/identity")["session"]);
        CTEST_COMPARE(first.listTools().size(), 1);
        CTEST_COMPARE(first.callTool("echo", {{"value", "HTTP 앱"}})["content"].toArray()[0].toObject()["text"].toString(), "HTTP 앱");
        const auto ids = server.sessionIds("alpha"); CTEST_COMPARE(ids.size(), 1);
        server.notify(ids.first(), "notifications/tools/list_changed");
        QList<QJsonObject> received;
        CTEST_WAIT_DEFAULT(([&] { received += first.takeNotifications(); return !received.isEmpty(); })());
        CTEST_COMPARE(received.first()["method"].toString(), "notifications/tools/list_changed");
        CTEST_CHECK(second.takeNotifications().isEmpty());
        first.setRoots({QJsonObject{{"uri", "file:///fixture"}}});
        QList<m::HttpServerNotification> notifications;
        CTEST_WAIT_DEFAULT(([&] { notifications += server.takeNotifications(); return !notifications.isEmpty(); })());
        CTEST_COMPARE(notifications.first().principal, "alpha");
        CTEST_COMPARE(notifications.first().message["method"].toString(), "notifications/roots/list_changed");
        first.close(); CTEST_WAIT_DEFAULT(server.sessionIds("alpha").isEmpty());
        CTEST_COMPARE(second.request("test/identity")["principal"].toString(), "beta");
    }
    void concurrentReverseRequestsStayOnTheirOriginalStreams() {
        // Eight reverse handlers issue ping concurrently. This routing test
        // needs eight control slots; overload behavior has separate coverage.
        auto transport=transportOptions();transport.maxControlStreams=8;
        m::HttpServer server(protocolOptions, transport); CTEST_CHECK(server.listen());
        m::HttpClient* connected = nullptr;
        m::ClientOptions host; host.capabilities = {{"sampling", QJsonObject{}}};
        host.requestHandlers["sampling/createMessage"] = [&](const QJsonObject& p, const auto&) {
            connected->request("ping"); return QJsonObject{{"value", p["value"]}};
        };
        auto options = clientOptions(server); options.maxServerRequests = 8;
        m::HttpClient client(options, host); connected = &client;
        std::vector<std::future<QJsonObject>> results;
        for (int n = 0; n < 8; ++n) results.push_back(std::async(std::launch::async, [&, n] {
            int progress = 0;
            auto result = client.request("test/reverse", {{"value", n}}, {}, [&](const auto&) { ++progress; });
            if (progress != 2) throw std::runtime_error("Progress escaped its original request stream");
            return result;
        }));
        for (int n = 0; n < 8; ++n) CTEST_COMPARE(results[n].get()["value"].toInt(), n);
    }
    void streamCapacityKeepsCancellationAvailable() {
        std::atomic_bool stopped = false;
        auto http = transportOptions(); http.maxStreams = 2;
        m::HttpServer server([&](const QString& principal) {
            auto options = protocolOptions(principal);
            options.handlers["test/wait"] = [&](const auto&, const auto& context) {
                context.progress({{"progress", 1}});
                while (!context.cancellation.isCancelled()) std::this_thread::sleep_for(1ms);
                stopped = true; context.cancellation.throwIfCancelled(); return QJsonObject{};
            }; return options;
        }, http); CTEST_CHECK(server.listen());
        m::HttpClient client(clientOptions(server)); CancellationToken token;
        CTEST_WAIT_DEFAULT(!server.sessionIds("alpha").isEmpty());
        server.notify(server.sessionIds("alpha").first(), "notifications/tools/list_changed");
        bool notified = false;
        CTEST_WAIT_DEFAULT((notified |= !client.takeNotifications().isEmpty())); // The background GET occupies the first slot.
        std::promise<void> started; auto ready = started.get_future();
        auto pending = std::async(std::launch::async, [&] {
            try { client.request("test/wait", {}, token, [&](const auto&) { started.set_value(); }); return ErrorCode::ProtocolError; }
            catch (const Error& error) { return error.code(); }
        });
        CTEST_CHECK(ready.wait_for(2s) == std::future_status::ready);
        try { client.request("test/identity"); agent_ctest::fail(__FILE__, __LINE__, "Expected a full stream capacity"); }
        catch (const m::HttpError& error) { CTEST_COMPARE(error.statusCode(), 429); }
        token.cancel(); CTEST_COMPARE(pending.get(), ErrorCode::Cancelled); CTEST_WAIT_DEFAULT(stopped.load());
        CTEST_COMPARE(client.request("test/identity")["principal"].toString(), "alpha");
    }
    void shutdownAndRelistenJoinCallbacksWithoutHoldingTheSessionMap() {
        std::atomic_int closed = 0;
        m::HttpServer* exposed = nullptr;
        m::HttpServer server([&](const QString& principal) {
            auto options = protocolOptions(principal);
            options.onClosed = [&](const auto&) { exposed->sessionIds(); ++closed; };
            return options;
        }, transportOptions());
        exposed = &server;
        m::HttpServer occupied(protocolOptions, transportOptions()); CTEST_CHECK(occupied.listen());
        CTEST_CHECK(!server.listen(occupied.port()));
        for (int n = 0; n < 8; ++n) { CTEST_CHECK(server.listen()); server.close(); }
        for (int n = 0; n < 3; ++n) {
            CTEST_CHECK(server.listen());
            auto options = clientOptions(server); options.listenForNotifications = false;
            m::HttpClient client(options);
            CTEST_COMPARE(client.request("test/identity")["principal"].toString(), "alpha");
            server.close(); client.close();
            CTEST_COMPARE(closed.load(), n + 1); CTEST_CHECK(server.sessionIds().isEmpty()); CTEST_COMPARE(server.port(), 0);
        }
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    McpHttpServerTests tests;
    runner.run("registeredControlsHaveCapacityWhenNormalStreamsAndRetentionAreFull", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.registeredControlsHaveCapacityWhenNormalStreamsAndRetentionAreFull(); });
    runner.run("authenticatedSessionsAndNotifications", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.authenticatedSessionsAndNotifications(); });
    runner.run("concurrentReverseRequestsStayOnTheirOriginalStreams", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.concurrentReverseRequestsStayOnTheirOriginalStreams(); });
    runner.run("streamCapacityKeepsCancellationAvailable", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.streamCapacityKeepsCancellationAvailable(); });
    runner.run("shutdownAndRelistenJoinCallbacksWithoutHoldingTheSessionMap", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.shutdownAndRelistenJoinCallbacksWithoutHoldingTheSessionMap(); });
    return runner.result();
}

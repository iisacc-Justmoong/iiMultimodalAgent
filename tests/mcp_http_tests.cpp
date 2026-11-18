#include <iiMultimodalAgent/mcp/HttpClient.h>
#include <iiMultimodalAgent/agent/McpTools.h>
#include <QtCore/QProcess>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QSslConfiguration>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QCoreApplication>
#include <chrono>
#include <thread>

using namespace iiLocalLLM; using namespace iiMultimodalAgent;
using namespace std::chrono_literals;
namespace m = iiMultimodalAgent::mcp;
namespace {
struct Peer {
    QProcess process;
    QUrl url;
    explicit Peer(QString mode = "normal", QStringList extra = {}) {
        process.start(MCP_TEST_PYTHON, QStringList{"-B", MCP_HTTP_PEER, mode} + extra);
        if (!process.waitForStarted(3000) || !process.waitForReadyRead(3000))
            throw std::runtime_error("HTTP peer did not start");
        url = QUrl(QString::fromUtf8(process.readLine()).trimmed());
        if (!url.isValid()) throw std::runtime_error("Invalid HTTP peer URL");
    }
    ~Peer() { process.kill(); process.waitForFinished(3000); }
    m::HttpOptions options() const {
        m::HttpOptions o; o.endpoint = url; o.requestTimeoutMs = 3000;
        o.initializeTimeoutMs = 3000; o.shutdownTimeoutMs = 100;
        o.bearerToken = [] { return QByteArray("test-credential"); }; return o;
    }
};
template<class F> void checkError(F f, ErrorCode code) {
    try { f(); agent_ctest::fail(__FILE__, __LINE__, "Expected error"); }
    catch (const Error& e) { CTEST_COMPARE(e.code(), code); }
}
}
class McpHttpTests : public QObject {
public:
    void initTestCase() {
        CTEST_CHECK_MESSAGE(QFileInfo(QString::fromUtf8(MCP_TEST_PYTHON)).isExecutable(), MCP_TEST_PYTHON);
    }
    void jsonLifecycleAndToolAdapter() {
        Peer peer; auto client = std::make_shared<m::HttpClient>(peer.options());
        CTEST_CHECK(client->isConnected()); CTEST_COMPARE(client->protocolVersion(), "2025-11-25");
        CTEST_COMPARE(client->listTools().size(), 1);
        const auto result = client->callTool("echo", {{"value", "로컬 앱"}});
        CTEST_COMPARE(result["structuredContent"].toObject()["value"].toString(), "로컬 앱");
        auto tools = agent::mcpTools(client, {"app", "com.iisacc.fixture"});
        CTEST_COMPARE(tools.size(), 1);
        const auto imported = tools[0].execute({{"value", "adapter"}}, {});
        CTEST_COMPARE(imported.data["value"].toString(), "adapter");
        CTEST_CHECK(imported.text.contains("\"value\":\"adapter\""));
        auto state = client->request("test/state");
        CTEST_COMPARE(state["initializations"].toInt(), 1); CTEST_CHECK(state["headersValid"].toBool());
        client->close(); CTEST_CHECK(!client->isConnected()); CTEST_CHECK(client->isClosed());
    }
    void sseReverseRequestsProgressAndConcurrency() {
        Peer peer;
        m::HttpClient* connectedClient = nullptr;
        m::ClientOptions c; c.capabilities = {{"sampling", QJsonObject{}}};
        c.requestHandlers["sampling/createMessage"] = [&](const auto&, const auto&) {
            connectedClient->request("ping");
            return QJsonObject{{"model", "host"}, {"role", "assistant"},
                {"content", QJsonObject{{"type", "text"}, {"text", "host-result"}}}};
        };
        c.roots = {QJsonObject{{"uri", "file:///workspace"}}};
        auto http = peer.options(); http.maxServerRequests = 16;
        m::HttpClient client(http, c); connectedClient = &client;
        for (int round = 0; round < 5; ++round) {
        std::vector<std::future<QJsonObject>> calls;
        for (int n = 0; n < 8; ++n) calls.push_back(std::async(std::launch::async, [&, n] {
            return client.request("test/reverse", {{"value", n}});
        }));
        try { for (int n = 0; n < 8; ++n) {
            const auto r = calls[n].get(); CTEST_COMPARE(r["value"].toInt(), n);
            CTEST_COMPARE(r["sample"].toString(), "host-result");
            CTEST_COMPARE(r["roots"].toArray().size(), 1);
        } } catch (...) {
            peer.process.waitForReadyRead(1);
            qWarning().noquote() << peer.process.readAllStandardError(); throw;
        }
        }
        QList<double> progress;
        CTEST_COMPARE(client.request("test/progress", {}, {}, [&](const auto& p) {
            progress.append(p["progress"].toDouble());
        })["text"].toString(), "분할된 UTF-8");
        CTEST_COMPARE(progress, (QList<double>{1, 2}));
        client.request("test/notify");
        QList<QJsonObject> received;
        CTEST_WAIT_DEFAULT(([&] { received.append(client.takeNotifications()); return !received.isEmpty(); })());
        CTEST_COMPARE(received.first()["method"].toString(), "notifications/tools/list_changed");
    }
    void resumeWithoutReposting() {
        Peer peer; m::HttpClient client(peer.options());
        const auto start = std::chrono::steady_clock::now();
        CTEST_COMPARE(client.request("test/resume")["resumed"].toBool(), true);
        CTEST_CHECK(std::chrono::steady_clock::now() - start >= 70ms);
        auto state = client.request("test/state");
        CTEST_COMPARE(state["resumePosts"].toInt(), 1); CTEST_COMPARE(state["resumeGets"].toInt(), 1);
    }
    void initializationCanResumeThroughSse() {
        Peer peer("resume-initialize"); m::HttpClient client(peer.options());
        CTEST_CHECK(client.isConnected()); CTEST_COMPARE(client.listTools().size(), 1);
        const auto state = client.request("test/state");
        CTEST_COMPARE(state["initializations"].toInt(), 1); CTEST_CHECK(state["headersValid"].toBool());
    }
    void largeRetryDoesNotReconnectEarly() {
        Peer peer; auto o = peer.options(); o.reconnectDelayMs = 0;
        m::HttpClient client(o);
        checkError([&] { client.request("test/resume-long", {}, {}, {}, 150); }, ErrorCode::Timeout);
        CTEST_COMPARE(client.request("test/state")["resumeGets"].toInt(), 0);
    }
    void postHasItsOwnDeadline() {
        Peer peer; auto o = peer.options(); o.initializeTimeoutMs = 150;
        m::HttpClient client(o);
        CTEST_CHECK(client.request("test/slow-headers", {}, {}, {}, 1000)["ok"].toBool());
    }
    void cancellationTimeoutAndNoReplay() {
        Peer peer; m::HttpClient client(peer.options()); CancellationToken token;
        std::promise<void> started; auto ready = started.get_future();
        auto work = std::async(std::launch::async, [&] {
            checkError([&] { client.request("test/wait", {}, token, [&](const auto&) { started.set_value(); }); }, ErrorCode::Cancelled);
        });
        CTEST_CHECK(ready.wait_for(2s) == std::future_status::ready); token.cancel(); work.get();
        try { client.request("test/wait", {}, {}, {}, 100); agent_ctest::fail(__FILE__, __LINE__, "Expected HTTP request timeout"); }
        catch (const Error& error) {
            const auto* timeout = dynamic_cast<const m::RequestTimeoutError*>(&error);
            CTEST_CHECK(timeout); CTEST_COMPARE(timeout->code(), ErrorCode::Timeout);
            CTEST_COMPARE(timeout->method(), "test/wait"); CTEST_COMPARE(timeout->timeoutMs(), 100);
            CTEST_CHECK(timeout->elapsedMs() >= 100); CTEST_CHECK(timeout->submitted());
        }
        CTEST_WAIT_COMPARE(client.request("test/state")["cancelled"].toInt(), 2);
        checkError([&] { client.request("test/disconnect"); }, ErrorCode::RuntimeFailure);
        CTEST_COMPARE(client.request("test/state")["disconnectPosts"].toInt(), 1);
        checkError([&] { client.request("test/drop"); }, ErrorCode::RuntimeFailure);
        CTEST_COMPARE(client.request("test/state")["droppedPosts"].toInt(), 1);
    }
    void expiredSessionReinitializesWithoutReplayingTools() {
        Peer peer; auto client = std::make_shared<m::HttpClient>(peer.options());
        auto tools = agent::mcpTools(client, {"app", {}});
        std::promise<void> accepted; auto waiting = accepted.get_future();
        auto oldRequest = std::async(std::launch::async, [&] {
            try { client->request("test/wait", {}, {}, [&](const auto&) { accepted.set_value(); }); return 0; }
            catch (const m::HttpError& e) { return e.statusCode(); }
        });
        CTEST_CHECK(waiting.wait_for(2s) == std::future_status::ready);
        try { client->request("test/expire"); agent_ctest::fail(__FILE__, __LINE__, "Expected expired session"); }
        catch (const m::HttpError& e) { CTEST_COMPARE(e.statusCode(), 404); }
        CTEST_COMPARE(oldRequest.get(), 404);
        CTEST_CHECK(!client->isClosed());
        CTEST_WAIT_DEFAULT(client->isConnected() && client->connectionGeneration() == 2);
        auto state = client->request("test/state");
        CTEST_COMPARE(state["initializations"].toInt(), 2); CTEST_COMPARE(state["expirePosts"].toInt(), 1);
        checkError([&] { tools[0].execute({{"value", "stale-schema"}}, {}); }, ErrorCode::RuntimeFailure);
        CTEST_COMPARE(client->callTool("echo", {{"value", "new-session"}})["structuredContent"].toObject()["value"].toString(), "new-session");
    }
    void authenticationAndRedirectIsolation() {
        Peer peer; auto o = peer.options(); o.bearerToken = [] { return QByteArray("wrong-secret"); };
        try { m::HttpClient bad(o); agent_ctest::fail(__FILE__, __LINE__, "Expected HTTP 401"); }
        catch (const m::HttpError& e) {
            CTEST_COMPARE(e.statusCode(), 401); CTEST_CHECK(e.wwwAuthenticate().contains("Bearer"));
            CTEST_CHECK(!QByteArray(e.what()).contains("wrong-secret"));
        }
        m::HttpClient client(peer.options());
        try { client.request("test/redirect"); agent_ctest::fail(__FILE__, __LINE__, "Redirect must not be followed"); }
        catch (const m::HttpError& e) { CTEST_COMPARE(e.statusCode(), 307); }
        CTEST_COMPARE(client.request("test/state")["redirectTargetHits"].toInt(), 0);
    }
    void separateClientsAndNoEventChannel() {
        Peer peer; auto options = peer.options(); options.listenForNotifications = false;
        m::HttpClient first(options), second(options);
        const auto one = first.request("test/session")["session"].toString();
        const auto two = second.request("test/session")["session"].toString();
        CTEST_CHECK(!one.isEmpty() && !two.isEmpty() && one != two);
        first.close();
        CTEST_COMPARE(second.request("test/session")["session"].toString(), two);
    }
    void tlsRejectsAnUntrustedCertificate() {
        const auto openssl = QStandardPaths::findExecutable("openssl");
        if (openssl.isEmpty()) agent_ctest::skip("TLS certificate fixture requires the OpenSSL command line tool");
        QTemporaryDir dir; CTEST_CHECK(dir.isValid());
        const auto cert = dir.filePath("test-cert.pem"), key = dir.filePath("test-key.pem");
        QProcess generator;
        generator.start(openssl, {"req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1", "-keyout", key, "-out", cert});
        CTEST_CHECK(generator.waitForFinished(10000)); CTEST_COMPARE(generator.exitCode(), 0);
        Peer peer("tls", {cert, key});
        struct Restore {
            QSslConfiguration saved = QSslConfiguration::defaultConfiguration();
            ~Restore() { QSslConfiguration::setDefaultConfiguration(saved); }
        } restore;
        auto insecure = restore.saved; insecure.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslConfiguration::setDefaultConfiguration(insecure);
        checkError([&] { m::HttpClient client(peer.options()); }, ErrorCode::RuntimeFailure);
    }
    void invalidInputAndBounds() {
        Peer peer; auto o = peer.options(); o.endpoint = QUrl("http://example.com/mcp");
        checkError([&] { m::HttpClient c(o); }, ErrorCode::InvalidArgument);
        o = peer.options(); o.headers.insert("Mcp-Session-Id", "injected");
        checkError([&] { m::HttpClient c(o); }, ErrorCode::InvalidArgument);
        o = peer.options(); o.headers.insert("X-Host", "bad\r\nheader");
        checkError([&] { m::HttpClient c(o); }, ErrorCode::InvalidArgument);
        o = peer.options(); o.headers.insert("X-Host\n", "value");
        checkError([&] { m::HttpClient c(o); }, ErrorCode::InvalidArgument);
        o = peer.options(); o.maxMessageBytes = 1024; o.maxQueuedBytes = 2048;
        m::HttpClient limited(o);
        checkError([&] { limited.request("test/oversize"); }, ErrorCode::ResourceLimit);
        m::HttpClient invalidEvent(peer.options());
        checkError([&] { invalidEvent.request("test/invalid-event-id"); }, ErrorCode::ProtocolError);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    McpHttpTests tests;
    try { tests.initTestCase(); }
    catch (const std::exception& error) { std::cerr << "initTestCase: " << error.what() << '\n'; return 1; }
    catch (...) { std::cerr << "initTestCase: unexpected exception\n"; return 1; }
    runner.run("jsonLifecycleAndToolAdapter", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.jsonLifecycleAndToolAdapter(); });
    runner.run("sseReverseRequestsProgressAndConcurrency", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.sseReverseRequestsProgressAndConcurrency(); });
    runner.run("resumeWithoutReposting", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.resumeWithoutReposting(); });
    runner.run("initializationCanResumeThroughSse", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.initializationCanResumeThroughSse(); });
    runner.run("largeRetryDoesNotReconnectEarly", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.largeRetryDoesNotReconnectEarly(); });
    runner.run("postHasItsOwnDeadline", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.postHasItsOwnDeadline(); });
    runner.run("cancellationTimeoutAndNoReplay", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.cancellationTimeoutAndNoReplay(); });
    runner.run("expiredSessionReinitializesWithoutReplayingTools", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.expiredSessionReinitializesWithoutReplayingTools(); });
    runner.run("authenticationAndRedirectIsolation", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.authenticationAndRedirectIsolation(); });
    runner.run("separateClientsAndNoEventChannel", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.separateClientsAndNoEventChannel(); });
    runner.run("tlsRejectsAnUntrustedCertificate", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.tlsRejectsAnUntrustedCertificate(); });
    runner.run("invalidInputAndBounds", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.invalidInputAndBounds(); });
    return runner.result();
}

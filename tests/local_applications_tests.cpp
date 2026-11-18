#include "native_link.h"
#include "iiMultimodalAgent/mcp/LocalApplications.h"
#include "iiMultimodalAgent/mcp/HttpClient.h"
#include "iiMultimodalAgent/agent/McpConnections.h"
#include "iiMultimodalAgent/agent/McpServer.h"
#include "iiMultimodalAgent/agent/ObjectTools.h"
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QThread>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <future>
#include <thread>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a = iiMultimodalAgent::agent;
namespace m = iiMultimodalAgent::mcp;
using namespace std::chrono_literals;
namespace {
QJsonObject schema() { return {{"type", "object"}, {"additionalProperties", false}}; }
a::ToolDefinition definition() { return {"status", "Read actual application state", schema(), {}, true, true}; }
m::ServerOptions echoServer() {
    m::ServerOptions server;
    server.lists["tools/list"] = [](const auto&) { return QJsonArray{a::toJson(definition())}; };
    server.handlers["tools/call"] = [](const auto&, const auto&) {
        return QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "app response"}}}}};
    };
    return server;
}
void write(const QString& path, const QJsonObject& value) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) throw std::runtime_error("Fixture write failed");
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(value).toJson());
}
QJsonObject read(const QString& path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Fixture read failed");
    return QJsonDocument::fromJson(file.readAll()).object();
}
m::HttpOptions clientOptions(const m::LocalApplicationEndpoint& endpoint) {
    m::HttpOptions options; options.endpoint = endpoint.endpoint;
    options.bearerToken = [token = endpoint.bearerToken] { return token; };
    options.initializeTimeoutMs = 2000; options.requestTimeoutMs = 2000; return options;
}
}
class LocalApplicationTests : public QObject {
public:
    void initTestCase() {
#if !defined(Q_OS_UNIX) || defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
        agent_ctest::skip("Private local application registration requires desktop POSIX");
#endif
    }
    void authenticatedRegistrationAndRemoval();
    void privateDirectoryAndRecords();
    void rejectRemoteCommandsMalformedAndStaleRecords();
    void automaticRefreshAndCredentialRedaction();
    void appRefreshDoesNotRereadCommandConfiguration();
    void objectThreadAndOwnerLifetime();
    void timedOutAndCancelledQueuedActionsAreDiscarded();
    void timeoutAfterDispatchReportsPossibleEffects();
    void shutdownDoesNotWaitForTheUiThread();
};
void LocalApplicationTests::authenticatedRegistrationAndRemoval() {
    QTemporaryDir root; const auto directory = root.filePath("apps");
    CTEST_CHECK(m::discoverLocalApplications(directory).applications.isEmpty());
    m::LocalApplicationServer server({"com.iisacc.test", "Actual fixture app", "1"}, echoServer(), {directory});
    CTEST_CHECK_MESSAGE(server.listen(), qPrintable(server.errorString()));
    const auto found = m::discoverLocalApplications(directory);
    CTEST_COMPARE(found.error, ErrorCode::None); CTEST_COMPARE(found.applications.size(), 1);
    const auto endpoint = found.applications.first();
    CTEST_COMPARE(endpoint.application.id, QString("com.iisacc.test"));
    CTEST_COMPARE(endpoint.serverName, server.serverName()); CTEST_COMPARE(endpoint.endpoint, server.endpoint());
    CTEST_CHECK(endpoint.bearerToken.size() >= 32);
    m::HttpClient client(clientOptions(endpoint));
    CTEST_COMPARE(client.serverInfo()["name"].toString(), QString("Actual fixture app"));
    CTEST_COMPARE(client.request("tools/call", {{"name", "status"}, {"arguments", QJsonObject{}}})["content"].toArray().size(), 1);
    auto wrong = clientOptions(endpoint); wrong.bearerToken = [] { return QByteArray(43, 'x'); };
    CTEST_THROWS(Error, m::HttpClient(wrong));
    const auto oldInstance = endpoint.instanceId; const auto oldPath = server.registrationPath();
    server.close(); CTEST_CHECK(!QFileInfo::exists(oldPath));
    CTEST_CHECK(m::discoverLocalApplications(directory).applications.isEmpty());
    CTEST_CHECK(server.listen()); CTEST_CHECK(m::discoverLocalApplications(directory).applications.first().instanceId != oldInstance);
}
void LocalApplicationTests::privateDirectoryAndRecords() {
#ifdef Q_OS_UNIX
    QTemporaryDir root; const auto directory = root.filePath("apps");
    m::LocalApplicationServer server({"com.iisacc.test", "Fixture", "1"}, echoServer(), {directory});
    CTEST_CHECK(server.listen());
    struct stat info{}; CTEST_CHECK(!stat(QFile::encodeName(directory).constData(), &info)); CTEST_COMPARE(info.st_mode & 0777, mode_t(0700));
    CTEST_CHECK(!stat(QFile::encodeName(server.registrationPath()).constData(), &info)); CTEST_COMPARE(info.st_mode & 0777, mode_t(0600));
    const auto alias = root.filePath("alias"); CTEST_CHECK(createNativeTestLink(directory, alias));
    CTEST_CHECK(m::discoverLocalApplications(alias).applications.isEmpty());
    CTEST_CHECK(QFile::setPermissions(server.registrationPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadOther));
    CTEST_COMPARE(m::discoverLocalApplications(directory).rejectedRecords, 1);
    CTEST_CHECK(QFile::setPermissions(server.registrationPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    const auto recordAlias = root.filePath("hardlink.json");
    CTEST_CHECK(!link(QFile::encodeName(server.registrationPath()).constData(), QFile::encodeName(recordAlias).constData()));
    CTEST_COMPARE(m::discoverLocalApplications(directory).rejectedRecords, 1);
    CTEST_CHECK(QFile::remove(recordAlias));
    CTEST_CHECK(!chmod(QFile::encodeName(directory).constData(), 0777));
    CTEST_COMPARE(m::discoverLocalApplications(directory).error, ErrorCode::Unauthorized);
    m::LocalApplicationServer refused({"com.iisacc.test", "Fixture", "1"}, echoServer(), {directory});
    CTEST_CHECK(!refused.listen());
    CTEST_CHECK(!chmod(QFile::encodeName(directory).constData(), 0700));
#endif
}
void LocalApplicationTests::rejectRemoteCommandsMalformedAndStaleRecords() {
    QTemporaryDir root; const auto directory = root.filePath("apps");
    m::LocalApplicationServer server({"com.iisacc.test", "Fixture", "1"}, echoServer(), {directory}); CTEST_CHECK(server.listen());
    const auto good = read(server.registrationPath());
    auto bad = good; bad["endpoint"] = "https://example.com/mcp"; write(server.registrationPath(), bad);
    CTEST_COMPARE(m::discoverLocalApplications(directory).rejectedRecords, 1);
    bad = good; bad["endpoint"] = "http://127.0.0.1:123/mcp?token=private"; write(server.registrationPath(), bad);
    CTEST_COMPARE(m::discoverLocalApplications(directory).rejectedRecords, 1);
    bad = good; bad["command"] = "/never/execute/this"; write(server.registrationPath(), bad);
    CTEST_COMPARE(m::discoverLocalApplications(directory).rejectedRecords, 1);
    bad = good; bad["pid"] = 1.5; write(server.registrationPath(), bad);
    CTEST_COMPARE(m::discoverLocalApplications(directory).rejectedRecords, 1);
#ifdef Q_OS_UNIX
    const auto child = fork(); CTEST_CHECK(child >= 0);
    if (!child) _exit(0);
    int status = 0; CTEST_COMPARE(waitpid(child, &status, 0), child);
    bad = good; bad["pid"] = qint64(child); write(server.registrationPath(), bad);
    CTEST_COMPARE(m::discoverLocalApplications(directory).staleRecords, 1);
    CTEST_CHECK(QFile::remove(server.registrationPath()));
    CTEST_CHECK(!mkfifo(QFile::encodeName(server.registrationPath()).constData(), 0600));
    CTEST_COMPARE(m::discoverLocalApplications(directory).rejectedRecords, 1);
    CTEST_CHECK(QFile::remove(server.registrationPath()));
#endif
    write(server.registrationPath(), good);
    m::LocalApplicationServer other({"com.iisacc.test", "Second instance", "1"}, echoServer(), {directory}); CTEST_CHECK(other.listen());
    CTEST_COMPARE(m::discoverLocalApplications(directory).applications.size(), 2);
    const auto limited = m::discoverLocalApplications(directory, 1);
    CTEST_COMPARE(limited.applications.size(), 1); CTEST_CHECK(limited.truncated);
}
void LocalApplicationTests::automaticRefreshAndCredentialRedaction() {
    QTemporaryDir root; const auto directory = root.filePath("apps");
    auto registry = std::make_shared<a::ToolRegistry>();
    a::McpConnectionOptions options; options.workingDirectory = root.path(); options.localApplicationsDirectory = directory;
    options.refreshIntervalMs = 20; options.retryDelayMs = 20;
    a::McpConnections connections(registry, options);
    m::LocalApplicationServer server({"com.iisacc.test", "Fixture", "1"}, echoServer(), {directory}); CTEST_CHECK(server.listen());
    CTEST_WAIT_COMPARE_TIMEOUT(registry->definitions().size(), 1, 5000);
    const auto first = registry->definitions().first(); CTEST_CHECK(first.deferred);
    CTEST_COMPARE(first.metadata["app_id"].toString(), QString("com.iisacc.test"));
    CTEST_COMPARE(first.metadata["discovery_source"].toString(), QString("local_application"));
    const auto endpoint = m::discoverLocalApplications(directory).applications.first();
    CTEST_CHECK(!QJsonDocument(connections.status()).toJson().contains(endpoint.bearerToken));
    CTEST_CHECK(!QJsonDocument(a::toJson(first)).toJson().contains(endpoint.bearerToken));
    const auto old = registry->get(first.name);
    CTEST_COMPARE(old.execute({}, {}).text, QString("app response"));
    auto oldClient = connections.client(server.serverName()); CTEST_CHECK(oldClient);
    server.close();
    CTEST_WAIT_VERIFY(registry->definitions().isEmpty(), 5000);
    // Automatic refresh publishes the registry before closing old transports
    // outside its locks. Observe both completion conditions independently.
    CTEST_WAIT_VERIFY(oldClient->isClosed(), 5000);
    CTEST_THROWS(Error, old.execute({}, {}));
    CTEST_CHECK(server.listen()); CTEST_WAIT_COMPARE_TIMEOUT(registry->definitions().size(), 1, 5000);
    CTEST_CHECK(registry->definitions().first().metadata["connection_id"] != first.metadata["connection_id"]);
}
void LocalApplicationTests::appRefreshDoesNotRereadCommandConfiguration() {
    QTemporaryDir root; const auto directory = root.filePath("apps");
    m::LocalApplicationServer server({"com.iisacc.test", "Fixture", "1"}, echoServer(), {directory}); CTEST_CHECK(server.listen());
    const auto endpoint = m::discoverLocalApplications(directory).applications.first();
    const auto config = root.filePath("mcp.json");
    write(config, {{"mcpServers", QJsonObject{{"fixed", QJsonObject{{"type", "http"}, {"url", endpoint.endpoint.toString()},
        {"headers", QJsonObject{{"Authorization", "Bearer " + QString::fromLatin1(endpoint.bearerToken)}}}}}}}});
    a::McpConnectionOptions options; options.workingDirectory = root.path(); options.configFiles = {config};
    options.localApplicationsDirectory = directory; options.refreshIntervalMs = 0;
    auto registry = std::make_shared<a::ToolRegistry>(); a::McpConnections connections(registry, options);
    CTEST_COMPARE(connections.status().size(), 2); CTEST_CHECK(QFile::remove(config));
    connections.refresh(); CTEST_COMPARE(connections.status().size(), 2);
    CTEST_CHECK(connections.client("fixed")->isConnected());
    CTEST_THROWS(Error, connections.reload());
    CTEST_CHECK(connections.client("fixed")->isConnected());
}
void LocalApplicationTests::objectThreadAndOwnerLifetime() {
    auto owner = std::make_unique<QObject>(); owner->setProperty("answer", 42);
    auto tool = a::objectTool(owner.get(), definition(), [](QObject& object, const auto&, const auto&) {
        if (QThread::currentThread() != object.thread()) throw std::runtime_error("Wrong thread");
        return a::ToolResult{"owner state", {{"answer", QJsonValue::fromVariant(object.property("answer"))}}};
    });
    auto result = std::async(std::launch::async, [&] { return tool.execute({}, {}); });
    CTEST_WAIT_VERIFY(result.wait_for(0ms) == std::future_status::ready, 3000);
    CTEST_COMPARE(result.get().data["answer"].toInt(), 42);
    owner.reset();
    auto destroyed = std::async(std::launch::async, [&] { return tool.execute({}, {}); });
    CTEST_WAIT_VERIFY(destroyed.wait_for(0ms) == std::future_status::ready, 3000);
    CTEST_THROWS(Error, (void)destroyed.get());
}
void LocalApplicationTests::timedOutAndCancelledQueuedActionsAreDiscarded() {
    QObject owner; int effects = 0;
    auto tool = a::objectTool(&owner, definition(), [&](auto&, const auto&, const auto&) { ++effects; return a::ToolResult{"changed"}; }, 30);
    auto timeout = std::async(std::launch::async, [&] { return tool.execute({}, {}); });
    CTEST_CHECK(timeout.wait_for(2000ms) == std::future_status::ready); // Deliberately do not pump Qt.
    const auto result = timeout.get(); CTEST_CHECK(result.isError); CTEST_COMPARE(result.data["dispatch_started"].toBool(), false);
    QCoreApplication::processEvents(); CTEST_COMPARE(effects, 0);
    a::ToolContext context;
    tool = a::objectTool(&owner, definition(), [&](auto&, const auto&, const auto&) { ++effects; return a::ToolResult{"changed"}; }, 1000);
    auto cancelled = std::async(std::launch::async, [&] { return tool.execute({}, context); });
    std::this_thread::sleep_for(30ms); context.cancellation.cancel();
    CTEST_CHECK(cancelled.wait_for(2000ms) == std::future_status::ready);
    CTEST_THROWS(Error, (void)cancelled.get());
    QCoreApplication::processEvents(); CTEST_COMPARE(effects, 0);
}
void LocalApplicationTests::timeoutAfterDispatchReportsPossibleEffects() {
    QObject owner; int effects = 0;
    auto tool = a::objectTool(&owner, definition(), [&](auto&, const auto&, const auto&) {
        std::this_thread::sleep_for(200ms); ++effects; return a::ToolResult{"changed"};
    }, 40);
    auto call = std::async(std::launch::async, [&] { return tool.execute({}, {}); });
    CTEST_WAIT_VERIFY(call.wait_for(0ms) == std::future_status::ready, 3000);
    const auto result = call.get(); CTEST_CHECK(result.isError); CTEST_CHECK(result.data["dispatch_started"].toBool()); CTEST_COMPARE(effects, 1);
}
void LocalApplicationTests::shutdownDoesNotWaitForTheUiThread() {
    QTemporaryDir root; QObject owner; int effects = 0;
    auto registry = std::make_shared<a::ToolRegistry>();
    registry->add(a::objectTool(&owner, definition(), [&](auto&, const auto&, const auto&) { ++effects; return a::ToolResult{"changed"}; }, 10000));
    a::McpServerOptions bridge; bridge.workingDirectory = root.path();
    m::LocalApplicationServer server({"com.iisacc.test", "Fixture", "1"},
        a::mcpServerOptions(registry, std::make_shared<a::RulePolicy>(), bridge), {root.filePath("apps")});
    CTEST_CHECK(server.listen());
    m::HttpClient client(clientOptions(m::discoverLocalApplications(root.filePath("apps")).applications.first()));
    auto call = std::async(std::launch::async, [&] { return client.request("tools/call", {{"name", "status"}, {"arguments", QJsonObject{}}}); });
    std::this_thread::sleep_for(100ms);
    QElapsedTimer elapsed; elapsed.start(); server.close(); CTEST_CHECK(elapsed.elapsed() < 2000);
    CTEST_CHECK(call.wait_for(2000ms) == std::future_status::ready);
    try { (void)call.get(); } catch (const Error&) {}
    QCoreApplication::processEvents(); CTEST_COMPARE(effects, 0);
}
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    LocalApplicationTests tests;
    try { tests.initTestCase(); }
    catch (const agent_ctest::SkippedCase& skipped) { std::cerr << "initTestCase: skipped: " << skipped.reason << '\n'; return 77; }
    catch (const std::exception& error) { std::cerr << "initTestCase: " << error.what() << '\n'; return 1; }
    catch (...) { std::cerr << "initTestCase: unexpected exception\n"; return 1; }
    runner.run("authenticatedRegistrationAndRemoval", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.authenticatedRegistrationAndRemoval(); });
    runner.run("privateDirectoryAndRecords", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.privateDirectoryAndRecords(); });
    runner.run("rejectRemoteCommandsMalformedAndStaleRecords", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.rejectRemoteCommandsMalformedAndStaleRecords(); });
    runner.run("automaticRefreshAndCredentialRedaction", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.automaticRefreshAndCredentialRedaction(); });
    runner.run("appRefreshDoesNotRereadCommandConfiguration", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.appRefreshDoesNotRereadCommandConfiguration(); });
    runner.run("objectThreadAndOwnerLifetime", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.objectThreadAndOwnerLifetime(); });
    runner.run("timedOutAndCancelledQueuedActionsAreDiscarded", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.timedOutAndCancelledQueuedActionsAreDiscarded(); });
    runner.run("timeoutAfterDispatchReportsPossibleEffects", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.timeoutAfterDispatchReportsPossibleEffects(); });
    runner.run("shutdownDoesNotWaitForTheUiThread", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.shutdownDoesNotWaitForTheUiThread(); });
    return runner.result();
}

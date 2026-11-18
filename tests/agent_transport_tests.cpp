#include <iiMultimodalAgent/agent/Api.h>
#include <HttpApiServer.h>
#include <LocalIpcServer.h>
#include <QtCore/QDir>
#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QLocalSocket>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QCoreApplication>
#include <chrono>
#include <stdexcept>
#include <thread>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
using namespace std::chrono_literals;
namespace a = iiMultimodalAgent::agent;
namespace {
const QString credential(48, 'a'), otherCredential(48, 'b');
QByteArray bytes(QJsonObject value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
class Model final : public a::Model {
public:
    std::atomic_int entered = 0, cancelled = 0;
    a::ModelReply generate(const a::ModelRequest& r, const CancellationToken& token, const TextCallback& delta) override {
        ++entered; const auto prompt = r.messages.last().text;
        if (prompt == "wait") {
            while (!token.isCancelled()) std::this_thread::sleep_for(1ms);
            ++cancelled; token.throwIfCancelled();
        }
        const auto value = prompt == "large" ? QString(8192, 'x') : prompt;
        if (delta) delta(value); return {value, {}, {1, 1, 0, 0}};
    }
};
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("rpc-XXXXXX")};
    QTemporaryDir ipcDirectory{QStringLiteral(IIMULTIMODALAGENT_TEST_RESOURCE_ROOT "/ipc-XXXXXX")};
    std::shared_ptr<Model> model = std::make_shared<Model>();
    std::unique_ptr<Service> service;
    std::shared_ptr<a::Api> api;
    std::unique_ptr<LocalIpcServer> ipc;
    std::unique_ptr<HttpApiServer> http;
    explicit Fixture(int timeout = 3000, int buffer = 65536,QStringList intercept = {},bool decision = false) {
        if (!root.isValid() || !ipcDirectory.isValid())
            throw std::runtime_error("could not create isolated agent transport workspaces");
        qInfo().noquote() << "agent transport IPC workspace:" << ipcDirectory.path();
        ServiceOptions serviceOptions; serviceOptions.modelsDirectory = root.filePath("Models");
        service = std::make_unique<Service>(serviceOptions);
        a::ApiOptions options{.engine={.decision={.enabled=false}}}; options.workingDirectory = root.filePath("work"); QDir().mkpath(options.workingDirectory);
        options.engine.decision.enabled=decision;
        options.stateDirectory = root.filePath("private"); options.clientTokens = {{"society", credential}, {"dreamscapes", otherCredential}};
        options.procedures.intercept=std::move(intercept);
        api = std::make_shared<a::Api>(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), options);
        ipc = std::make_unique<LocalIpcServer>(*service); ipc->setRpcHandler(api);
        if (!ipc->listen(ipcDirectory.filePath("s")))
            throw std::runtime_error((ipcDirectory.filePath("s") + ": " + ipc->errorString()).toStdString());
        qInfo().noquote() << "agent transport IPC listening:" << ipc->serverName();
        HttpOptions h; h.requestTimeoutMs = timeout; h.maxBufferedOutputBytes = buffer;
        http = std::make_unique<HttpApiServer>(*service, h); http->setRpcHandler(api);
        if (!http->listen()) throw std::runtime_error(http->errorString().toStdString());
    }
};
struct Response { int status = 0; QByteArray body, authenticate; };
Response post(Fixture& f, QJsonObject value, QString token = credential,
    std::function<void(QNetworkReply*, const QByteArray&)> received = {}, QByteArray origin = {}) {
    QNetworkAccessManager network;
    QNetworkRequest request(QUrl(QString("http://127.0.0.1:%1/v1/rpc").arg(f.http->port())));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    if (!origin.isEmpty()) request.setRawHeader("Origin", origin);
    auto reply = network.post(request, bytes(value)); Response result; QElapsedTimer timer; timer.start();
    QObject::connect(reply, &QNetworkReply::readyRead, [&] { const auto chunk = reply->readAll(); result.body += chunk; if (received) received(reply, chunk); });
    while (!reply->isFinished() && timer.elapsed() < 5000) agent_ctest::processFor(1);
    if (!reply->isFinished()) reply->abort();
    result.body += reply->readAll(); result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.authenticate = reply->rawHeader("WWW-Authenticate"); return result;
}
QJsonObject request(QString method, QJsonObject params = {}, bool stream = false) {
    return {{"id", "client-request"}, {"method", method}, {"params", params}, {"stream", stream}};
}
QJsonObject object(const Response& r) { return QJsonDocument::fromJson(r.body).object(); }
QList<QJsonObject> events(const QByteArray& body) {
    QList<QJsonObject> result;
    for (const auto& line : body.split('\n')) if (line.startsWith("data: ") && line != "data: [DONE]")
        result.append(QJsonDocument::fromJson(line.mid(6)).object());
    return result;
}
class Native {
public:
    QLocalSocket socket; QByteArray input; QList<QJsonObject> frames;
    explicit Native(const Fixture& f) { socket.connectToServer(f.ipc->serverName()); if (!socket.waitForConnected(1000)) throw std::runtime_error("IPC connect failed"); }
    void send(QString id, QString method, QJsonObject params = {}, QString auth = credential) {
        socket.write(bytes({{"id", id}, {"method", method}, {"params", params}, {"auth", auth}}) + '\n'); socket.flush();
    }
    void receive() {
        agent_ctest::processFor(1); input += socket.readAll();
        while (input.contains('\n')) { const auto end = input.indexOf('\n'); frames.append(QJsonDocument::fromJson(input.first(end)).object()); input.remove(0, end + 1); }
    }
    QJsonObject until(QString id, QString field) {
        QElapsedTimer timer; timer.start();
        do { receive(); for (const auto& f : frames) if (f["id"] == id && f.contains(field)) return f; }
        while (timer.elapsed() < 4000 && socket.state() == QLocalSocket::ConnectedState);
        throw std::runtime_error("IPC response did not arrive");
    }
};
}
class AgentTransportTests : public QObject {
public:
    void hostQuantitiesCrossHttpAndDecisionResultCrossesIpc() {
        Fixture f(5000,1024*1024,{"decision_input"},true);Native client(f);
        const auto sid=object(post(f,request("agent.sessions.create",{{"model","fixture"}})))["result"].toObject().value("session_id");
        client.send("run","agent.run",{{"session_id",sid},{"prompt","work"}});
        for(int pass=0;pass<2;++pass){
            QJsonObject pending;CTEST_WAIT_VERIFY(([&]{const auto records=object(post(f,request("agent.procedures.list",{{"session_id",sid}})))["result"].toObject()["procedures"].toArray();for(const auto& r:records)if(r.toObject()["kind"]=="decision_input"&&r.toObject()["phase"]=="waiting"){pending=r.toObject();return true;}return false;})(),3000);
            QJsonObject output{{"value_unit","USD"},{"estimates",QJsonArray{QJsonObject{{"candidate_id","run"},{"success_gain","100"},{"failure_loss","10"},{"cost","1"},{"success_probability","0.9"}}}},{"evidence",QJsonArray{}}};
            const auto response=post(f,request("agent.procedures.respond",{{"procedure_id",pending["procedure_id"]},{"response",QJsonObject{{"action","replace"},{"output",output}}}}));CTEST_COMPARE(response.status,200);
        }
        const auto result=client.until("run","result")["result"].toObject();CTEST_COMPARE(result["status"],"completed");CTEST_COMPARE(result["decision"].toObject()["action"],"execute");CTEST_COMPARE(f.model->entered.load(),1);
    }
    void procedureReturnsCrossIpcAndHttpWithoutBlockingControls() {
        Fixture f(5000,1024*1024,{"model"});Native client(f);
        const auto id=object(post(f,request("agent.sessions.create",{{"model","fixture"}})))["result"].toObject().value("session_id");
        client.send("run","agent.run",{{"session_id",id},{"prompt","original"}});
        QJsonObject waiting;
        CTEST_WAIT_VERIFY(([&]{client.receive();for(const auto& frame:client.frames){const auto event=frame["data"].toObject();if(event["event"]=="procedure"&&event["data"].toObject()["phase"]=="waiting"){waiting=event["data"].toObject();return true;}}return false;})(),3000);
        const auto listing=post(f,request("agent.procedures.list",{{"session_id",id}}));CTEST_COMPARE(listing.status,200);
        CTEST_CHECK(!object(listing)["result"].toObject()["procedures"].toArray().isEmpty());
        CTEST_COMPARE(post(f,request("agent.procedures.respond",{{"procedure_id",waiting["procedure_id"]},{"response",QJsonObject{{"action","continue"}}}}),otherCredential).status,404);
        const QJsonObject response{{"action","replace"},{"output",QJsonObject{{"text","host via HTTP"},{"tool_calls",QJsonArray{}}}}};
        CTEST_COMPARE(post(f,request("agent.procedures.respond",{{"procedure_id",waiting["procedure_id"]},{"response",response}})).status,200);
        CTEST_COMPARE(client.until("run","result")["result"].toObject()["text"],"host via HTTP");
        bool responded=false, resumeAccepted=false;
        auto stream=post(f,request("agent.run",{{"session_id",id},{"prompt","next"}},true),credential,[&](QNetworkReply*,const QByteArray& chunk){
            if(responded||!chunk.contains("waiting"))return;
            const auto list=object(post(f,request("agent.procedures.list",{{"session_id",id}})))["result"].toObject()["procedures"].toArray();
            for(const auto& value:list)if(value.toObject()["phase"]=="waiting"){
                responded=true;client.send("resume","agent.procedures.respond",{{"procedure_id",value.toObject()["procedure_id"]},{"response",QJsonObject{{"action","continue"}}}});
                resumeAccepted=client.until("resume","result")["result"].toObject()["accepted"].toBool();break;
            }
        });
        CTEST_COMPARE(stream.status,200);CTEST_CHECK(responded);CTEST_CHECK(resumeAccepted);CTEST_CHECK(stream.body.contains("procedure"));CTEST_CHECK(stream.body.contains("data: [DONE]"));
    }
    void authSessionsForkAndEventOrder() {
        Fixture f;
        const auto unauth = post(f, request("agent.info"), {}); CTEST_COMPARE(unauth.status, 401); CTEST_CHECK(unauth.authenticate.startsWith("Bearer"));
        CTEST_COMPARE(object(unauth)["error"].toObject()["code"].toString(), "unauthorized");
        CTEST_COMPARE(post(f, request("agent.info"), "invalid").status, 401);
        CTEST_COMPARE(post(f, request("agent.info"), credential, {}, "https://example.com").status, 403);
        auto invalid = request("agent.info"); invalid["auth"] = credential; CTEST_COMPARE(post(f, invalid).status, 400);
        Native client(f); client.send("bad", "agent.info", {}, "wrong");
        CTEST_COMPARE(client.until("bad", "error")["error"].toObject()["code"].toString(), "unauthorized");
        const auto created = post(f, request("agent.sessions.create", {{"model", "fixture"}})); CTEST_COMPARE(created.status, 200);
        const auto id = object(created)["result"].toObject().value("session_id"); CTEST_CHECK(id.isString());
        client.send("run", "agent.run", {{"session_id", id}, {"prompt", "안녕 🌍"}});
        CTEST_COMPARE(client.until("run", "result")["result"].toObject()["text"].toString(), "안녕 🌍");
        QList<QJsonObject> runFrames; for (const auto& frame : client.frames) if (frame["id"] == "run") runFrames.append(frame);
        CTEST_COMPARE(runFrames.first()["event"].toString(), "accepted"); CTEST_CHECK(runFrames.last().contains("result"));
        CTEST_COMPARE(runFrames[runFrames.size() - 2]["data"].toObject()["event"].toString(), "finished");
        const auto get = post(f, request("agent.sessions.get", {{"session_id", id}})); CTEST_COMPARE(get.status, 200);
        CTEST_COMPARE(object(get)["result"].toObject()["message_count"].toInt(), 2);
        CTEST_COMPARE(post(f, request("agent.sessions.get", {{"session_id", id}}), otherCredential).status, 404);
        const auto fork = post(f, request("agent.sessions.fork", {{"session_id", id}})); CTEST_COMPARE(fork.status, 200);
        const auto forkId = object(fork)["result"].toObject().value("session_id"); CTEST_CHECK(forkId != id);
        const auto stream = post(f, request("agent.run", {{"session_id", forkId}, {"prompt", "continued"}}, true));
        CTEST_COMPARE(stream.status, 200); CTEST_CHECK(stream.body.endsWith("data: [DONE]\n\n"));
        const auto frames = events(stream.body); CTEST_COMPARE(frames.first()["event"].toString(), "accepted");
        CTEST_COMPARE(frames.last()["event"].toString(), "done"); CTEST_COMPARE(frames.last()["result"].toObject()["text"].toString(), "continued");
        CTEST_CHECK(!stream.body.contains(credential.toUtf8()));
    }
    void crossTransportCancellationAndDisconnect() {
        Fixture f; Native client(f);
        const auto create = post(f, request("agent.sessions.create", {{"model", "fixture"}}));
        const auto id = object(create)["result"].toObject().value("session_id");
        client.send("run", "agent.run", {{"session_id", id}, {"prompt", "wait"}});
        const auto accepted = client.until("run", "request_id"); CTEST_WAIT_COMPARE(f.model->entered.load(), 1);
        CTEST_COMPARE(post(f, request("agent.status", {{"request_id", accepted.value("request_id")}}), otherCredential).status, 404);
        const auto cancelled = post(f, request("agent.cancel", {{"request_id", accepted.value("request_id")}})); CTEST_COMPARE(cancelled.status, 200);
        CTEST_COMPARE(client.until("run", "result")["result"].toObject()["status"].toString(), "cancelled");
        bool aborted = false;
        post(f, request("agent.run", {{"session_id", id}, {"prompt", "wait"}}, true), credential,
            [&](QNetworkReply* reply, const QByteArray&) { if (f.model->entered.load() >= 2) { aborted = true; reply->abort(); } });
        CTEST_CHECK(aborted);
        CTEST_WAIT_DEFAULT(f.model->cancelled.load() >= 2);
        client.send("disconnect", "agent.run", {{"session_id", id}, {"prompt", "wait"}});
        client.until("disconnect", "request_id"); CTEST_WAIT_COMPARE(f.model->entered.load(), 3);
        client.socket.abort(); CTEST_WAIT_COMPARE(f.model->cancelled.load(), 3);
    }
    void timeoutAndShutdown() {
        // The model deliberately waits for cancellation. Allow persisted run
        // admission to finish under disk load before testing its HTTP deadline.
        Fixture f(3000, 2048);
        // Session creation writes to disk and is setup, not the operation
        // whose deliberately short HTTP deadline this test exercises.
        Native client(f);client.send("create","agent.sessions.create",{{"model","fixture"}});
        const auto created=client.until("create","result");
        const auto id=created["result"].toObject().value("session_id");CTEST_CHECK(id.isString());
        const auto timed = post(f, request("agent.run", {{"session_id", id}, {"prompt", "wait"}})); CTEST_COMPARE(timed.status, 504);
        CTEST_WAIT_COMPARE(f.model->cancelled.load(), 1);
        client.send("run", "agent.run", {{"session_id", id}, {"prompt", "wait"}});
        client.until("run", "request_id"); CTEST_WAIT_COMPARE(f.model->entered.load(), 2);
        f.ipc->close(); f.http->close(); f.api->close(); CTEST_COMPARE(f.model->cancelled.load(), 2);
    }
    void streamOverflowHasIndependentDeadline() {
        Fixture f(3000, 2048);
        const auto create = post(f, request("agent.sessions.create", {{"model", "fixture"}}));
        CTEST_CHECK_MESSAGE(create.status == 200, create.body.constData());
        const auto id = object(create)["result"].toObject().value("session_id");
        const auto large = post(f, request("agent.run", {{"session_id", id}, {"prompt", "large"}}, true));
        const auto frames = events(large.body); CTEST_CHECK(!frames.isEmpty());
        CTEST_COMPARE(frames.last()["error"].toObject()["code"].toString(), "resource_limit");
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    AgentTransportTests tests;
    runner.run("hostQuantitiesCrossHttpAndDecisionResultCrossesIpc", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.hostQuantitiesCrossHttpAndDecisionResultCrossesIpc(); });
    runner.run("procedureReturnsCrossIpcAndHttpWithoutBlockingControls", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.procedureReturnsCrossIpcAndHttpWithoutBlockingControls(); });
    runner.run("authSessionsForkAndEventOrder", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.authSessionsForkAndEventOrder(); });
    runner.run("crossTransportCancellationAndDisconnect", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.crossTransportCancellationAndDisconnect(); });
    runner.run("timeoutAndShutdown", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.timeoutAndShutdown(); });
    runner.run("streamOverflowHasIndependentDeadline", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.streamOverflowHasIndependentDeadline(); });
    return runner.result();
}

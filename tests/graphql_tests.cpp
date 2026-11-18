#include <iiMultimodalAgent/agent/GraphQL.h>
#include <iiMultimodalAgent/agent/McpServer.h>
#include <iiMultimodalAgent/mcp/Server.h>
#include <iiFileProviderPresentation.h>
#include "../tools/GraphQLConfig.h"
#include "../third_party/cpp-httplib/httplib.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#ifdef Q_OS_UNIX
#include <sys/un.h>
#endif

namespace a=iiMultimodalAgent::agent;
namespace m=iiMultimodalAgent::mcp;
namespace {
struct Cleanup {
    std::filesystem::path root;
    ~Cleanup(){try{iiFileProvider::native::Presentation::cancelPending(root);}catch(...){}}
};
struct Endpoint {
    httplib::Server server;std::thread thread;int port;std::atomic_int calls=0;
    Endpoint() {
        server.Post("/graphql",[this](const auto& request,auto& response) {
            ++calls;
            if(request.get_header_value("Authorization")!="Bearer test-secret") {response.status=401;return;}
            const auto payload=QJsonDocument::fromJson(QByteArray::fromStdString(request.body)).object();
            const auto query=payload["query"].toString();
            if(query=="errors")response.set_content("{\"data\":null,\"errors\":[{\"message\":\"failed\"}]}","application/graphql-response+json");
            else if(query=="invalid") {response.status=400;response.set_content("{\"errors\":[{\"message\":\"invalid query\"}]}","application/graphql-response+json");}
            else if(query=="redirect")response.set_redirect("/graphql",302);
            else if(query=="slow") {std::this_thread::sleep_for(std::chrono::milliseconds(150));response.set_content("{\"data\":{}}","application/json");}
            else if(query=="oversize")response.set_content(std::string(2048,'x'),"application/json");
            else response.set_content(QJsonDocument(QJsonObject{{"data",QJsonObject{{"query",query},{"variables",payload["variables"]},{"operationName",payload["operationName"]}}}}).toJson(QJsonDocument::Compact).toStdString(),"application/json");
        });
        port=server.bind_to_any_port("127.0.0.1");if(port<1)throw std::runtime_error("Cannot bind test endpoint");
        thread=std::thread([this]{server.listen_after_bind();});server.wait_until_ready();
    }
    ~Endpoint(){server.stop();thread.join();}
    QString url()const{return QString("http://127.0.0.1:%1/graphql").arg(port);}
    a::GraphQLOptions options()const {a::GraphQLOptions o;o.endpoints.insert("fixture",{QUrl(url()),"Bearer test-secret"});o.maxResponseBytes=1024;return o;}
};
QJsonObject arguments(QString query="query Test($v: String) { echo(v: $v) }") {
    return {{"endpoint","fixture"},{"query",query},{"variables",QJsonObject{{"v","hello"}}},{"operation_name","Test"}};
}
class WorkflowModel final:public a::Model {
public:
    a::ModelReply generate(const a::ModelRequest& request,const iiLocalLLM::CancellationToken& token,
        const iiLocalLLM::TextCallback&) override {
        token.throwIfCancelled();
        if(!request.messages.isEmpty() && request.messages.last().role==a::MessageRole::Tool)
            return {"WORKFLOW_COMPLETE "+request.messages.last().text};
        for(const auto& message:request.messages)
            if(message.role==a::MessageRole::User && message.text.contains("WORKFLOW_SKILL"))
                return {{},{{"skill-graphql","GraphQL",arguments()}}};
        return {"unexpected"};
    }
};
QJsonObject mcpRequest(int id,const QString& method,const QJsonObject& params={}) {
    return {{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}};
}
QJsonObject mcpCall(m::ServerSession& server,int id,const QString& name,const QJsonObject& args={}) {
    server.receive(mcpRequest(id,"tools/call",{{"name",name},{"arguments",args}}));
    const auto messages=server.takeMessages(30000);
    if(messages.size()!=1)throw std::runtime_error("MCP workflow response missing");
    return messages.first().toObject()["result"].toObject();
}
QJsonObject receive(QProcess& process,int expected) {
    QElapsedTimer clock;clock.start();
    while(clock.elapsed()<60000) {
        while(process.canReadLine()) {
            const auto document=QJsonDocument::fromJson(process.readLine());
            if(document.isObject() && document.object()["id"]==expected)return document.object();
        }
        if(process.state()==QProcess::NotRunning)break;
        process.waitForReadyRead(100);
    }
    throw std::runtime_error("CLI MCP response missing");
}
void send(QProcess& process,const QJsonObject& value) {
    process.write(QJsonDocument(value).toJson(QJsonDocument::Compact)+"\n");
    if(!process.waitForBytesWritten(10000))throw std::runtime_error("CLI MCP input failed");
}
void save(const QString& path,const QByteArray& data) {
    QFile file(path);if(!file.open(QIODevice::WriteOnly) || file.write(data)!=data.size())throw std::runtime_error("Cannot write test configuration");
    file.close();if(!QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner))throw std::runtime_error("Cannot protect test configuration");
}
QJsonObject cli(const QStringList& args) {
    QProcess process;process.setProgram(QCoreApplication::applicationDirPath()+"/iimagent");process.setArguments(args);
    process.start();if(!process.waitForStarted(30000) || !process.waitForFinished(60000) || process.exitCode()!=0)
        throw std::runtime_error(("CLI failed: "+process.readAllStandardError()).toStdString());
    const auto result=QJsonDocument::fromJson(process.readAllStandardOutput());
    if(!result.isObject())throw std::runtime_error("CLI returned invalid JSON");return result.object();
}
}
class GraphQLTests:public QObject {
public:
    void nativeTransportAndPermission() {
        Endpoint endpoint;auto registry=std::make_shared<a::ToolRegistry>();registry->add(a::graphQLTool(endpoint.options()));
        a::ToolContext context;context.sessionId="test";
        a::ToolRunner denied(registry,std::make_shared<a::RulePolicy>());
        CTEST_CHECK(denied.run({"denied","GraphQL",arguments()},context).isError);CTEST_COMPARE(endpoint.calls.load(),0);
        a::ToolRunner allowed(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
        const auto result=allowed.run({"query","GraphQL",arguments()},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
        CTEST_COMPARE(result.data["data"].toObject()["variables"].toObject()["v"],"hello");
        CTEST_COMPARE(result.data["data"].toObject()["operationName"],"Test");CTEST_COMPARE(endpoint.calls.load(),1);
        const auto failed=allowed.run({"error","GraphQL",arguments("errors")},context);
        CTEST_CHECK(failed.isError);CTEST_COMPARE(failed.data["errors"].toArray().first().toObject()["message"],"failed");
        const auto invalid=allowed.run({"invalid","GraphQL",arguments("invalid")},context);
        CTEST_CHECK(invalid.isError);CTEST_COMPARE(invalid.data["errors"].toArray().first().toObject()["message"],"invalid query");
        CTEST_CHECK(allowed.run({"large","GraphQL",arguments("oversize")},context).isError);
        const auto redirectBefore=endpoint.calls.load();
        CTEST_CHECK(allowed.run({"redirect","GraphQL",arguments("redirect")},context).isError);
        CTEST_COMPARE(endpoint.calls.load(),redirectBefore+1);
        CTEST_CHECK(allowed.run({"bad","GraphQL",{{"endpoint","http://127.0.0.1"},{"query","x"}}},context).isError);
        const auto before=endpoint.calls.load();context.cancellation.cancel();
        CTEST_THROWS(iiLocalLLM::Error,allowed.run({"cancel","GraphQL",arguments()},context));
        CTEST_COMPARE(endpoint.calls.load(),before);
        auto slowOptions=endpoint.options();slowOptions.timeoutMs=25;
        auto slowRegistry=std::make_shared<a::ToolRegistry>();slowRegistry->add(a::graphQLTool(slowOptions));
        a::ToolRunner slow(slowRegistry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
        a::ToolContext active;active.sessionId="slow";
        CTEST_CHECK(slow.run({"slow","GraphQL",arguments("slow")},active).isError);
    }
    void workflowUsesNativeGraphqlWithCrudShellAndSkill() {
#ifdef Q_OS_UNIX
        Endpoint endpoint;QTemporaryDir root(QDir::tempPath()+"/g-XXXXXX");
        Cleanup cleanup{std::filesystem::path(root.path().toStdU16String())};
        const auto work=root.filePath("work");CTEST_CHECK(QDir().mkpath(work+"/.claude/skills/workflow"));
        save(work+"/.claude/skills/workflow/SKILL.md","---\ndescription: Workflow\ndisable-model-invocation: true\n---\nWORKFLOW_SKILL $ARGUMENTS");
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work);registry->add(a::graphQLTool(endpoint.options()));
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
        a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=root.filePath("sessions");options.toolSearch.enabled=false;
        auto engine=std::make_shared<a::Engine>(std::make_shared<WorkflowModel>(),registry,policy,options);
        a::McpServerOptions config;config.workingDirectory=work;config.engine=engine;config.model="fixture";
        m::ServerSession server(a::mcpServerOptions(registry,policy,config));
        server.receive(mcpRequest(1,"initialize",{{"protocolVersion","2025-11-25"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","test"},{"version","1"}}}}));
        const auto initialized=server.takeMessages(30000);CTEST_CHECK(initialized.size()==1 && initialized.first().toObject().contains("result"));
        server.receive({{"jsonrpc","2.0"},{"method","notifications/initialized"}});
        CTEST_CHECK(!mcpCall(server,2,"Write",{{"path","file.txt"},{"content","before"}})["isError"].toBool());
        CTEST_CHECK(!mcpCall(server,3,"Read",{{"path","file.txt"}})["isError"].toBool());
        CTEST_CHECK(!mcpCall(server,4,"Edit",{{"path","file.txt"},{"old_string","before"},{"new_string","after"}})["isError"].toBool());
        const auto gql=mcpCall(server,5,"GraphQL",arguments());
        CTEST_CHECK_MESSAGE(!gql["isError"].toBool(),qPrintable(QJsonDocument(gql).toJson()));
        CTEST_COMPARE(gql["structuredContent"].toObject()["data"].toObject()["variables"].toObject()["v"],"hello");
        CTEST_CHECK(!mcpCall(server,6,"Bash",{{"command","printf workflow"}})["isError"].toBool());
        const auto skill=mcpCall(server,7,"iiLocalLLM.agent.run",{{"skill","workflow"},{"skill_arguments","go"}});
        CTEST_CHECK_MESSAGE(skill["structuredContent"].toObject()["text"].toString().contains("WORKFLOW_COMPLETE"),qPrintable(QJsonDocument(skill).toJson()));
        CTEST_COMPARE(endpoint.calls.load(),2);
        CTEST_CHECK(!mcpCall(server,8,"Delete",{{"path","file.txt"}})["isError"].toBool());
        CTEST_CHECK(!QFileInfo::exists(work+"/file.txt"));server.close();
#endif
    }
    void configAndMcpExecutable() {
#ifdef Q_OS_UNIX
        Endpoint endpoint;QTemporaryDir root(QDir::tempPath()+"/g-XXXXXX");Cleanup cleanup{std::filesystem::path(root.path().toStdU16String())};
        CTEST_CHECK(root.isValid());const auto work=root.filePath("work");CTEST_CHECK(QDir().mkpath(work));
        const auto config=root.filePath("graphql.json");QFile file(config);CTEST_CHECK(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{{"endpoints",QJsonObject{{"fixture",QJsonObject{{"url",endpoint.url()},{"authorization","Bearer test-secret"}}}}}}).toJson());
        file.close();CTEST_CHECK(QFile::setPermissions(config,QFileDevice::ReadOwner|QFileDevice::WriteOwner));
        const auto parsed=iiLocalLLMClient::graphQLConfig(config,work);CTEST_COMPARE(parsed.endpoints.size(),1);
        QProcess process;process.setProgram(QCoreApplication::applicationDirPath()+"/iimagent-mcp");
        process.setArguments({"--workspace",work,"--graphql-config",config,"--allow","GraphQL","--no-apps"});
        process.start();CTEST_CHECK(process.waitForStarted(30000));
        send(process,{{"jsonrpc","2.0"},{"id",1},{"method","initialize"},{"params",QJsonObject{{"protocolVersion","2025-11-25"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","fixture"},{"version","1"}}}}}});
        CTEST_CHECK(receive(process,1).contains("result"));
        send(process,{{"jsonrpc","2.0"},{"method","notifications/initialized"}});
        send(process,{{"jsonrpc","2.0"},{"id",2},{"method","tools/call"},{"params",QJsonObject{{"name","GraphQL"},{"arguments",arguments()}}}});
        const auto response=receive(process,2);const auto result=response["result"].toObject();
        CTEST_CHECK_MESSAGE(!result["isError"].toBool(),qPrintable(QJsonDocument(response).toJson()));
        CTEST_COMPARE(result["structuredContent"].toObject()["data"].toObject()["variables"].toObject()["v"],"hello");
        CTEST_COMPARE(endpoint.calls.load(),1);process.closeWriteChannel();CTEST_CHECK(process.waitForFinished(60000));
#endif
    }
    void daemonAndCliCallGraphQLDirectly() {
#ifdef Q_OS_UNIX
        Endpoint endpoint;QTemporaryDir root(QDir::tempPath()+"/g-XXXXXX");Cleanup cleanup{std::filesystem::path(root.path().toStdU16String())};CTEST_CHECK(root.isValid());
        const auto work=root.filePath("work"),models=root.filePath("models");CTEST_CHECK(QDir().mkpath(work));CTEST_CHECK(QDir().mkpath(models));
        QTemporaryDir ipcDirectory(QStringLiteral(IIMULTIMODALAGENT_TEST_RESOURCE_ROOT "/ipc-XXXXXX"));
        CTEST_CHECK(ipcDirectory.isValid());
        const auto socket=ipcDirectory.filePath("s");
        CTEST_CHECK_MESSAGE(socket.toLocal8Bit().size() < qsizetype(sizeof(sockaddr_un{}.sun_path)), qPrintable(socket));
        const auto config=root.filePath("graphql.json"),credentials=root.filePath("clients.json"),token=root.filePath("token"),decision=root.filePath("decision.json");
        const auto secret=QByteArray(48,'a');
        save(token,secret);save(credentials,QJsonDocument(QJsonObject{{"fixture",QString::fromLatin1(secret)}}).toJson());
        save(config,QJsonDocument(QJsonObject{{"endpoints",QJsonObject{{"fixture",QJsonObject{{"url",endpoint.url()},{"authorization","Bearer test-secret"}}}}}}).toJson());
        save(decision,"{\"enabled\":false}");
        const auto sessionFile=root.filePath("session.json"),queryFile=root.filePath("query.json");
        save(sessionFile,QJsonDocument(QJsonObject{{"model","fixture"}}).toJson());
        save(queryFile,QJsonDocument(arguments()).toJson());
        QProcess daemon;daemon.setProgram(QCoreApplication::applicationDirPath()+"/iiMultimodalAgentD");
        daemon.setArguments({"--socket",socket,"--models-root",models,"--agent-workspace",work,"--agent-state",root.filePath("state"),
            "--agent-credentials",credentials,"--agent-graphql-config",config,"--agent-decision",decision,"--agent-allow","GraphQL","--agent-no-apps","--agent-no-web-fetch"});
        daemon.start();CTEST_CHECK(daemon.waitForStarted(30000));
        QElapsedTimer waiting;waiting.start();
        while(!QFileInfo::exists(socket) && daemon.state()!=QProcess::NotRunning && waiting.elapsed()<60000)QThread::msleep(20);
        CTEST_CHECK_MESSAGE(QFileInfo::exists(socket),daemon.readAllStandardError().constData());
        const QStringList prefix{"--socket",socket,"--auth-file",token};
        const auto session=cli(prefix+QStringList{"rpc","agent.sessions.create",sessionFile})["session_id"].toString();
        CTEST_CHECK(!session.isEmpty());
        const auto result=cli(prefix+QStringList{"agent","graphql","execute",session,queryFile});
        CTEST_CHECK_MESSAGE(result["is_error"].isBool() && !result["is_error"].toBool(),qPrintable(QJsonDocument(result).toJson()));
        CTEST_CHECK_MESSAGE(result["result"].toObject()["data"].toObject()["variables"].toObject()["v"]=="hello",qPrintable(QJsonDocument(result).toJson()));
        CTEST_COMPARE(endpoint.calls.load(),1);
        daemon.terminate();CTEST_CHECK(daemon.waitForFinished(30000));
#endif
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    GraphQLTests tests;
    runner.run("nativeTransportAndPermission", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.nativeTransportAndPermission(); });
    runner.run("workflowUsesNativeGraphqlWithCrudShellAndSkill", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.workflowUsesNativeGraphqlWithCrudShellAndSkill(); });
    runner.run("configAndMcpExecutable", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.configAndMcpExecutable(); });
    runner.run("daemonAndCliCallGraphQLDirectly", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.daemonAndCliCallGraphQLDirectly(); });
    return runner.result();
}

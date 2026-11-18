#include "native_link.h"
#include <iiMultimodalAgent/agent/Lsp.h>
#include <iiMultimodalAgent/agent/Api.h>
#include <iiMultimodalAgent/agent/McpServer.h>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QProcess>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QCoreApplication>
#include <future>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("lsp-test-XXXXXX")};
    a::ToolContext context; a::LspOptions options;
    Fixture(QString mode={}){context.sessionId="owner";context.workingDirectory=root.path();if(QProcess::execute("git",{"init","-q",root.path()})!=0)throw std::runtime_error("git init failed");
        a::LspServerOptions server;server.name="fixture";server.command=LSP_FIXTURE;server.arguments={mode,root.filePath("messages.jsonl")};server.extensions={{".cpp","cpp"}};
        server.settings={{"fixture",QJsonObject{{"enabled",true}}}};options.servers={server};write("main.cpp","int main() { return 0; }\n");}
    void write(const QString& path,const QByteArray& bytes){QFile file(root.filePath(path));if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size())throw std::runtime_error("fixture write failed");}
    QJsonObject args(QString operation="hover",QString path="main.cpp")const{return {{"operation",operation},{"filePath",path},{"line",1},{"character",5}};}
    QJsonArray messages(){QFile file(root.filePath("messages.jsonl"));if(!file.open(QIODevice::ReadOnly))return {};QJsonArray result;for(const auto& line:file.readAll().split('\n'))if(!line.isEmpty())result.append(QJsonDocument::fromJson(line).object());return result;}
};
}
class LspTests:public QObject {
public:
 void nativeOperationsUsePersistentServerAndPreserveProtocolData(){Fixture f;a::Lsp lsp(f.options);auto definition=lsp.tool().definition;CTEST_CHECK(definition.readOnly&&definition.deferred&&definition.concurrencySafe);
    for(const auto& op:{"goToDefinition","findReferences","hover","documentSymbol","workspaceSymbol","goToImplementation","prepareCallHierarchy","incomingCalls","outgoingCalls"}) {
        const auto r=lsp.query(f.args(op),f.context);CTEST_CHECK_MESSAGE(!r.isError,qPrintable(r.text));CTEST_COMPARE(r.data["operation"].toString(),QString(op));CTEST_CHECK(r.data.contains("data"));
        CTEST_CHECK(!r.text.contains("outside/secret"));CTEST_COMPARE(r.data["resultCount"].toInt(),1);}
    const auto result=lsp.query(f.args(),f.context);CTEST_CHECK(result.text.contains("한글 opened=1 changed=0"));
    CTEST_COMPARE(result.data["data"].toObject()["observed_position"].toObject()["character"].toInt(),4);
    auto status=lsp.status(f.context);CTEST_COMPARE(status["servers"].toArray().size(),1);CTEST_COMPARE(status["diagnostics"].toArray().size(),1);
    lsp.closeSession("owner");CTEST_COMPARE(lsp.status(f.context)["servers"].toArray().size(),0);
    bool shutdown=false,exit=false,settings=false;for(const auto& m:f.messages()){auto o=m.toObject();shutdown|=o["method"]=="shutdown";exit|=o["method"]=="exit";settings|=o["id"]=="server-settings"&&o["result"].toArray().first().toObject()["enabled"].toBool();}
    CTEST_CHECK(shutdown&&exit&&settings);
 }
 void changedFilesSendVersionedUtf16FullRangeReplacement(){Fixture f;f.write("main.cpp",QString::fromUtf8("// 🌐\nint value;\n").toUtf8());a::Lsp lsp(f.options);CTEST_THROWS(Error,lsp.query(f.args(),f.context));auto valid=f.args();valid["character"]=1;lsp.query(valid,f.context);
    f.write("main.cpp","int updated;\n");auto r=lsp.query(f.args(),f.context);CTEST_CHECK(r.text.contains("changed=1 version=2"));
    auto change=r.data["data"].toObject()["last_change"].toObject()["contentChanges"].toArray().first().toObject();
    CTEST_COMPARE(change["range"].toObject()["end"].toObject()["line"].toInt(),2);CTEST_COMPARE(change["text"].toString(),"int updated;\n");
    lsp.query(f.args(),f.context);CTEST_COMPARE(lsp.status(f.context)["servers"].toArray().size(),1);
 }
 void scopePermissionsAndOwnerIsolation(){Fixture f;auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Default,QList<a::PermissionRule>{{"Read(secret.cpp)",a::PermissionBehavior::Deny}});a::Lsp lsp(f.options,policy);
    f.write("secret.cpp","int secret;\n");auto registry=std::make_shared<a::ToolRegistry>();registry->add(lsp.tool());a::ToolRunner runner(registry,policy);
    CTEST_CHECK(runner.run({"denied","LSP",f.args("hover","secret.cpp")},f.context).isError);CTEST_CHECK(f.messages().isEmpty());
    CTEST_THROWS(Error,lsp.query(f.args("hover","../outside.cpp"),f.context));
    auto context=f.context;context.protectedPaths={f.root.filePath("main.cpp")};CTEST_THROWS(Error,lsp.query(f.args(),context));
    lsp.query(f.args(),f.context);auto other=f.context;other.sessionId="second";lsp.query(f.args(),other);
    auto s1=lsp.status(f.context)["servers"].toArray().first().toObject();auto s2=lsp.status(other)["servers"].toArray().first().toObject();CTEST_CHECK(s1["pid"]!=s2["pid"]);
 }
 void cancellationTimeoutAndProtocolErrorsReleaseProcesses(){Fixture f("slow");f.options.requestTimeoutMs=150;a::Lsp lsp(f.options);QElapsedTimer timer;timer.start();CTEST_THROWS(Error,lsp.query(f.args(),f.context));CTEST_CHECK(timer.elapsed()<2000);
    auto c=f.context;auto pending=std::async(std::launch::async,[&]{try{lsp.query(f.args(),c);return false;}catch(const Error& e){return e.code()==ErrorCode::Cancelled;}});QThread::msleep(30);c.cancellation.cancel();CTEST_CHECK(pending.get());
    for(const auto& mode:{"bad-frame","utf8","unsupported","crash","invalid-result"}){Fixture bad(mode);a::Lsp invalid(bad.options);CTEST_THROWS(Error,invalid.query(bad.args(),bad.context));}
 }
 void schemaAndBoundsRejectBeforeStartingServer(){Fixture f;a::Lsp lsp(f.options);auto r=std::make_shared<a::ToolRegistry>();r->add(lsp.tool());
    auto args=f.args();args["command"]="evil";CTEST_THROWS(Error,r->validateInput("LSP",args));
    args=f.args();args["line"]=0;CTEST_THROWS(Error,lsp.query(args,f.context));
    args=f.args();args["character"]=1000;CTEST_THROWS(Error,lsp.query(args,f.context));
    f.write("main.cpp",QByteArray("a\0b",3));CTEST_THROWS(Error,lsp.query(f.args(),f.context));CTEST_CHECK(f.messages().isEmpty());
    auto options=f.options;options.maxDocumentBytes=5;a::Lsp small(options);f.write("main.cpp","int main();\n");CTEST_THROWS(Error,small.query(f.args(),f.context));
    options=f.options;options.maxInstances=1;a::Lsp bounded(options);bounded.query(f.args(),f.context);auto other=f.context;other.sessionId="other";CTEST_THROWS(Error,bounded.query(f.args(),other));
 }
 void transientContentModifiedRetriesWithinTheSameBudget(){Fixture f("retry");f.options.requestTimeoutMs=4000;a::Lsp lsp(f.options);CTEST_CHECK(lsp.query(f.args(),f.context).text.contains("한글"));}
 void hostPolicyReceivesTheSameNativeDefinitionDuringRevalidation(){
    class Policy:public a::PermissionPolicy {public:a::PermissionDecision decide(const a::ToolDefinition& tool,const QJsonObject& args,const a::ToolContext& context)const override {
        if(tool.name=="LSP"&&(tool.deferred||tool.metadata["source"]!="builtin.lsp"||tool.metadata["canonical_path"].toString().isEmpty()||!tool.inputSchema["properties"].toObject().contains("operation")))return {a::PermissionBehavior::Deny,"Missing native LSP identity or canonical path"};
        return a::RulePolicy().decide(tool,args,context);}};
    Fixture f;auto policy=std::make_shared<Policy>();a::Lsp lsp(f.options,policy);auto registry=std::make_shared<a::ToolRegistry>();registry->add(lsp.tool(false));
    const auto result=a::ToolRunner(registry,policy).run({"native","LSP",f.args()},f.context);CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
 }
 void preparedPathCannotBeRedirectedAfterPermissionPreview(){Fixture f;f.write("secret.cpp","int private_value;\n");CTEST_CHECK(createNativeTestLink(f.root.filePath("main.cpp"),f.root.filePath("alias.cpp")));
    a::Lsp lsp(f.options);const auto prepared=lsp.tool().prepare(f.args("hover","alias.cpp"),f.context);
    CTEST_COMPARE(prepared.definition.metadata["canonical_path"].toString(),f.root.filePath("main.cpp"));CTEST_CHECK(QFile::remove(f.root.filePath("alias.cpp")));CTEST_CHECK(createNativeTestLink(f.root.filePath("secret.cpp"),f.root.filePath("alias.cpp")));
    CTEST_THROWS(Error,prepared.execute());CTEST_CHECK(f.messages().isEmpty());
 }
 void initializationErrorsArePublicErrors(){Fixture f("init-error");a::Lsp lsp(f.options);CTEST_THROWS(Error,lsp.query(f.args(),f.context));}
 void ignoredLocationsAndHostConfigAreValidated(){Fixture f;a::Lsp lsp(f.options);f.write(".gitignore","main.cpp\n");auto result=lsp.query(f.args("goToDefinition"),f.context);CTEST_COMPARE(result.data["resultCount"].toInt(),0);CTEST_CHECK(result.data["filtered_results"].toInt()>=2);
    const QJsonObject server{{"command",LSP_FIXTURE},{"args",QJsonArray{"",f.root.filePath("json.log")}},{"extensionToLanguage",QJsonObject{{".cpp","cpp"}}}};
    auto config=QJsonObject{{"servers",QJsonObject{{"fixture",server}}}};CTEST_COMPARE(a::lspOptionsFromJson(config).servers.size(),1);
    config["command"]="bad";CTEST_THROWS(Error,a::lspOptionsFromJson(config));config.remove("command");config["request_timeout_ms"]=1.5;CTEST_THROWS(Error,a::lspOptionsFromJson(config));
 }
 void queuedCancellationDoesNotWaitForActiveAnalysis(){Fixture f("slow");f.options.requestTimeoutMs=3000;a::Lsp lsp(f.options);auto first=f.context;
    auto active=std::async(std::launch::async,[&]{try{lsp.query(f.args(),first);}catch(const Error&){};});
    for(int i=0;i<100;++i){if(QJsonDocument(f.messages()).toJson().contains("textDocument/hover"))break;QThread::msleep(10);}
    auto second=f.context;auto queued=std::async(std::launch::async,[&]{try{lsp.query(f.args(),second);return false;}catch(const Error& e){return e.code()==ErrorCode::Cancelled;}});
    QThread::msleep(30);QElapsedTimer time;time.start();second.cancellation.cancel();const auto cancelled=queued.get();first.cancellation.cancel();active.get();CTEST_CHECK(cancelled);CTEST_CHECK(time.elapsed()<500);
 }
 void engineApiAndMcpShareOwnedLanguageServices(){
    class Model:public a::Model {public:a::ModelReply generate(const a::ModelRequest& request,const CancellationToken&,const std::function<bool(const QString&)>&)override{
        if(request.messages.last().role==a::MessageRole::Tool)return {"DONE",{},{1,1}};
        return {{},{{"lsp-call","LSP",{{"operation","documentSymbol"},{"filePath","main.cpp"},{"line",1},{"character",1}}}},{1,1}};}};
    Fixture f;QTemporaryDir state{QDir::current().filePath("lsp-owned-XXXXXX")};auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>();
    a::EngineOptions options{.decision={.enabled=false}};options.lsp=f.options;options.lsp.deferred=false;options.skills.enabled=false;options.projectContext.enabled=false;options.toolSearch.enabled=false;options.compaction.automatic=false;
    options.sessionsDirectory=state.filePath("engine");a::Engine engine(model,registry,policy,options);auto session=engine.createSession("fixture",f.root.path());
    const auto answer=engine.run({session.id,"Analyze"}).result.get();CTEST_COMPARE(answer.status,a::RunStatus::Completed);CTEST_COMPARE(answer.text,"DONE");CTEST_COMPARE(engine.session(session.id).messages.size(),4);
    CTEST_CHECK(!engine.runLsp(session.id,f.args()).isError);CTEST_COMPARE(engine.session(session.id).messages.size(),4);CTEST_COMPARE(engine.lspStatus(session.id)["servers"].toArray().size(),1);
    engine.endSession(session.id);CTEST_CHECK(engine.lspStatus(session.id)["servers"].toArray().isEmpty());
    options.sessionsDirectory.clear();a::ApiOptions apiOptions{.engine={.decision={.enabled=false}}};apiOptions.engine=options;apiOptions.workingDirectory=f.root.path();apiOptions.stateDirectory=state.filePath("api");apiOptions.clientTokens={{"society",QString(48,'a')},{"dreamscapes",QString(48,'b')}};
    a::Api api(model,registry,policy,apiOptions);auto apiCall=[&](QString method,QJsonObject params={},QString token=QString(48,'a')){return api.dispatch(method,params,token).result.get().toObject();};
    CTEST_CHECK(apiCall("agent.info")["lsp_enabled"].toBool());const auto id=apiCall("agent.sessions.create",{{"model","fixture"}})["session_id"].toString();auto params=f.args();params["session_id"]=id;
    CTEST_CHECK(!apiCall("agent.lsp.query",params)["is_error"].toBool());CTEST_THROWS(Error,apiCall("agent.lsp.query",params,QString(48,'b')));CTEST_COMPARE(apiCall("agent.sessions.get",{{"session_id",id}})["message_count"].toInt(),0);
    params["command"]="bad";CTEST_CHECK(apiCall("agent.lsp.query",params)["is_error"].toBool());CTEST_COMPARE(apiCall("agent.lsp.status",{{"session_id",id}})["servers"].toArray().size(),1);
    options.sessionsDirectory=state.filePath("mcp");auto host=std::make_shared<a::Engine>(model,registry,policy,options);a::McpServerOptions mo;mo.engine=host;mo.model="fixture";mo.workingDirectory=f.root.path();mcp::ServerSession server(a::mcpServerOptions(registry,policy,mo));
    auto rpc=[&](int id,QString method,QJsonObject params){server.receive(QJsonObject{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}});for(int i=0;i<400;++i)for(const auto& value:server.takeMessages(10))if(value.toObject()["id"]==id)return value.toObject();throw std::runtime_error("MCP timeout");};
    const auto init=rpc(1,"initialize",{{"protocolVersion","2025-11-25"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","test"},{"version","1"}}}});CTEST_CHECK(QJsonDocument(init).toJson().contains("iisacc/lsp"));server.receive(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/initialized"}});
    auto result=rpc(2,"tools/call",{{"name","LSP"},{"arguments",f.args()}})["result"].toObject();CTEST_CHECK_MESSAGE(!result["isError"].toBool(),QJsonDocument(result).toJson().constData());CTEST_CHECK(result["structuredContent"].toObject()["result"].toString().contains("한글"));
    result=rpc(3,"tools/call",{{"name","iiLocalLLM.agent.lsp.status"},{"arguments",QJsonObject{}}})["result"].toObject();CTEST_COMPARE(result["structuredContent"].toObject()["servers"].toArray().size(),1);
 }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) { std::cerr << "Expected a case name" << std::endl; return 1; }
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    LspTests tests;
    runner.run("nativeOperationsUsePersistentServerAndPreserveProtocolData", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.nativeOperationsUsePersistentServerAndPreserveProtocolData(); });
    runner.run("changedFilesSendVersionedUtf16FullRangeReplacement", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.changedFilesSendVersionedUtf16FullRangeReplacement(); });
    runner.run("scopePermissionsAndOwnerIsolation", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.scopePermissionsAndOwnerIsolation(); });
    runner.run("cancellationTimeoutAndProtocolErrorsReleaseProcesses", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.cancellationTimeoutAndProtocolErrorsReleaseProcesses(); });
    runner.run("schemaAndBoundsRejectBeforeStartingServer", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.schemaAndBoundsRejectBeforeStartingServer(); });
    runner.run("transientContentModifiedRetriesWithinTheSameBudget", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.transientContentModifiedRetriesWithinTheSameBudget(); });
    runner.run("hostPolicyReceivesTheSameNativeDefinitionDuringRevalidation", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.hostPolicyReceivesTheSameNativeDefinitionDuringRevalidation(); });
    runner.run("preparedPathCannotBeRedirectedAfterPermissionPreview", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.preparedPathCannotBeRedirectedAfterPermissionPreview(); });
    runner.run("initializationErrorsArePublicErrors", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.initializationErrorsArePublicErrors(); });
    runner.run("ignoredLocationsAndHostConfigAreValidated", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.ignoredLocationsAndHostConfigAreValidated(); });
    runner.run("queuedCancellationDoesNotWaitForActiveAnalysis", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.queuedCancellationDoesNotWaitForActiveAnalysis(); });
    runner.run("engineApiAndMcpShareOwnedLanguageServices", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.engineApiAndMcpShareOwnedLanguageServices(); });
    return runner.result();
}

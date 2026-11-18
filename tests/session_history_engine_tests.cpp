#include <iiMultimodalAgent/agent/Engine.h>
#include <iiMultimodalAgent/agent/Api.h>
#include <iiMultimodalAgent/agent/McpServer.h>
#include <iiMultimodalAgent/mcp/Server.h>
#include <iiFileProviderDevice.h>
#include "QtEventWait.h"
#include <QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QThread>
#include <atomic>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
class Model final:public a::Model {
public:
    std::atomic<bool> search=false,slow=false,started=false;QJsonObject seen;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const TextCallback&)override {
        started=true;while(slow&&!token.isCancelled())QThread::msleep(1);token.throwIfCancelled();
        if(!search)return {"Acknowledged."};
        if(request.messages.last().role==a::MessageRole::Tool){seen=request.messages.last().data;return {"History found."};}
        if(std::none_of(request.tools.begin(),request.tools.end(),[](const auto& t){return t.name=="SessionSearch";}))throw std::runtime_error("Missing eager SessionSearch");
        return {{},{{"search","SessionSearch",{{"query","DEPLOY_LABEL"}}}}};
    }
};
struct Fixture {
    iiFileProvider::TemporaryDirectory root{QDir::temp().filePath("history-engine-XXXXXX")};QString work=root.filePath("work");
    std::shared_ptr<Model> model=std::make_shared<Model>();std::shared_ptr<a::ToolRegistry> tools=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>();a::EngineOptions options{.decision={.enabled=false}};
    Fixture(){QDir().mkpath(work);a::registerWorkspaceTools(*tools,work);options.sessionsDirectory=root.filePath("sessions");
        options.sessionHistoryEnabled=true;options.skills.enabled=false;options.projectContext.enabled=false;options.toolSearch.enabled=false;options.compaction.automatic=false;}
};
QJsonObject api(a::Api& host,const QString& method,QJsonObject params={},const QString& credential=QString(48,'a')) {
    return host.dispatch(method,params,credential).result.get().toObject();
}
QJsonObject rpc(iiMultimodalAgent::mcp::ServerSession& server,int id,const QString& method,QJsonObject params={}) {
    server.receive(QJsonObject{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}});
    for(int i=0;i<300;++i)for(const auto& item:server.takeMessages(10))if(item.toObject()["id"]==id)return item.toObject();
    throw std::runtime_error("MCP response timed out");
}
}
class SessionHistoryEngineTests final {
public:
    void modelSearchFindsSavedSessionAndDirectSearchDoesNotMutateHistory() {
        Fixture f;a::Engine engine(f.model,f.tools,f.policy,f.options);
        auto old=engine.createSession("fixture",f.work).id;CTEST_COMPARE(engine.run({old,"DEPLOY_LABEL is ORCHID"}).result.get().status,a::RunStatus::Completed);
        const auto current=engine.createSession("fixture",f.work).id;f.model->search=true;
        const auto result=engine.run({current,"Find the previous deployment label."}).result.get();CTEST_COMPARE(result.status,a::RunStatus::Completed);
        const auto found=f.model->seen["matches"].toArray();CTEST_COMPARE(found.size(),1);CTEST_COMPARE(found.first().toObject()["session_id"].toString(),old);
        const auto count=engine.session(current).messages.size();CTEST_CHECK(!engine.runSessionSearch(current,{{"query","ORCHID"}}).isError);
        CTEST_COMPARE(engine.session(current).messages.size(),count);CTEST_COMPARE(engine.session(old).messages.size(),2);
    }
    void activeRunAllowsReadAndExplicitDenyWins() {
        Fixture f;a::Engine engine(f.model,f.tools,f.policy,f.options);const auto id=engine.createSession("fixture",f.work).id;
        f.model->slow=true;auto handle=engine.run({id,"active needle"});CTEST_WAIT_VERIFY(f.model->started,3000);
        const auto searched=engine.runSessionSearch(id,{{"query","active needle"},{"session_ids",QJsonArray{id}}});CTEST_CHECK(!searched.isError);CTEST_COMPARE(searched.data["matches"].toArray().size(),1);
        handle.cancel();CTEST_COMPARE(handle.result.get().status,a::RunStatus::Cancelled);f.model->slow=false;
        auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"SessionSearch",a::PermissionBehavior::Deny}});
        a::Engine denied(f.model,f.tools,policy,f.options);CTEST_CHECK(denied.runSessionSearch(id,{{"query","active needle"},{"session_ids",QJsonArray{id}}}).isError);
        f.options.sessionHistoryEnabled=false;a::Engine disabled(f.model,f.tools,f.policy,f.options);CTEST_CHECK(!disabled.sessionSearchTool());
        CTEST_THROWS(Error,disabled.runSessionSearch(id,{{"query","needle"}}));
    }
    void apiSeparatesClientsAndRejectsHostScopeArguments() {
        Fixture f;a::ApiOptions options{.engine={.decision={.enabled=false}}};options.stateDirectory=f.root.filePath("api");options.workingDirectory=f.work;
        options.engine=f.options;options.engine.sessionsDirectory.clear();options.clientTokens={{"society",QString(48,'a')},{"dreamscapes",QString(48,'b')}};
        a::Api host(f.model,f.tools,f.policy,options);CTEST_CHECK(api(host,"agent.info")["session_history_enabled"].toBool());
        const auto id=api(host,"agent.sessions.create",{{"model","fixture"}})["session_id"].toString();
        const auto other=api(host,"agent.sessions.create",{{"model","fixture"}},QString(48,'b'))["session_id"].toString();
        api(host,"agent.run",{{"session_id",id},{"prompt","SOC_SECRET"}});api(host,"agent.run",{{"session_id",other},{"prompt","DREAM_SECRET"}},QString(48,'b'));
        const auto page=api(host,"agent.sessions.search",{{"session_id",id},{"query","SECRET"},{"session_ids",QJsonArray{id}}});CTEST_CHECK(!page["is_error"].toBool());CTEST_COMPARE(page["result"].toObject()["matches"].toArray().size(),1);
        CTEST_THROWS(Error,api(host,"agent.sessions.search",{{"session_id",id},{"query","SECRET"}},QString(48,'b')));
        CTEST_CHECK(api(host,"agent.sessions.search",{{"session_id",id},{"query","SECRET"},{"session_ids",QJsonArray{other}}})["is_error"].toBool());
        CTEST_CHECK(api(host,"agent.sessions.search",{{"session_id",id},{"query","SECRET"},{"workspace",f.work}})["is_error"].toBool());
    }
    void mcpBindsSearchToConnectionAndAdvertisesDefinition() {
        Fixture f;int beforeTools=0;f.options.hooks.append([&](const a::HookInput& input,const auto&) {
            if(input.kind==a::HookKind::BeforeTool&&input.call.name=="SessionSearch")++beforeTools;return a::HookResult{};
        });auto engine=std::make_shared<a::Engine>(f.model,f.tools,f.policy,f.options);
        const auto old=engine->createSession("fixture",f.work).id;CTEST_COMPARE(engine->run({old,"MCP_HISTORY_NEEDLE"}).result.get().status,a::RunStatus::Completed);
        a::McpServerOptions options;options.engine=engine;options.model="fixture";options.workingDirectory=f.work;
        options.tools.hooks=f.options.hooks;
        iiMultimodalAgent::mcp::ServerSession server(a::mcpServerOptions(f.tools,f.policy,options));
        const auto init=rpc(server,1,"initialize",{{"protocolVersion","2025-11-25"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","test"},{"version","1"}}}});
        CTEST_CHECK(QJsonDocument(init).toJson().contains("iisacc/sessionHistory"));server.receive(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/initialized"}});
        const auto found=rpc(server,2,"tools/call",{{"name","SessionSearch"},{"arguments",QJsonObject{{"query","MCP_HISTORY_NEEDLE"}}}});
        CTEST_CHECK_MESSAGE(!found.contains("error")&&!found["result"].toObject()["isError"].toBool(),QJsonDocument(found).toJson().constData());
        CTEST_CHECK_MESSAGE(found["result"].toObject()["structuredContent"].toObject()["matches"].toArray().size()==1,QJsonDocument(found).toJson().constData());
        CTEST_COMPARE(beforeTools,1);
        const auto forged=rpc(server,3,"tools/call",{{"name","SessionSearch"},{"arguments",QJsonObject{{"query","needle"},{"session_id",old}}}});
        CTEST_CHECK(forged.contains("error")||forged["result"].toObject()["isError"].toBool());
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner;
    SessionHistoryEngineTests tests;
    runner.run("modelSearchFindsSavedSessionAndDirectSearchDoesNotMutateHistory", [&] { tests.modelSearchFindsSavedSessionAndDirectSearchDoesNotMutateHistory(); });
    runner.run("activeRunAllowsReadAndExplicitDenyWins", [&] { tests.activeRunAllowsReadAndExplicitDenyWins(); });
    runner.run("apiSeparatesClientsAndRejectsHostScopeArguments", [&] { tests.apiSeparatesClientsAndRejectsHostScopeArguments(); });
    runner.run("mcpBindsSearchToConnectionAndAdvertisesDefinition", [&] { tests.mcpBindsSearchToConnectionAndAdvertisesDefinition(); });
    return runner.result();
}

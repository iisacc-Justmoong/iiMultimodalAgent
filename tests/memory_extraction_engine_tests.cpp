#include <iiMultimodalAgent/agent/Engine.h>
#include <iiMultimodalAgent/agent/Api.h>
#include <iiMultimodalAgent/agent/McpServer.h>
#include <iiMultimodalAgent/mcp/Server.h>
#include "QtEventWait.h"
#include <QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QJsonDocument>
#include <atomic>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
class Model final:public a::Model {
public:
    std::atomic<bool> slow=false,started=false,cancelled=false;std::atomic<int> extractions=0;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const TextCallback&)override {
        for(const auto& message:request.messages)if(message.metadata.contains("iilocal.memory_extraction")) {
            started=true;++extractions;while(slow&&!token.isCancelled())QThread::msleep(1);
            if(token.isCancelled()){cancelled=true;token.throwIfCancelled();}
            if(request.messages.last().role==a::MessageRole::Tool)return {"Saved.",{},Usage{20,6}};
            const auto path=QDir(message.metadata["iilocal.memory_extraction"].toObject()["directory"].toString()).filePath("preferences.md");
            return {{},{{"save","Write",{{"path",path},{"content","---\nname: Language\ndescription: Korean reports\ntype: feedback\n---\nUse Korean for future reports."}}}},Usage{18,9}};
        }
        return {"I will use Korean for future reports.",{},Usage{10,7}};
    }
};
struct Fixture {
    QTemporaryDir root{QDir::temp().filePath("extraction-engine-XXXXXX")};QString work=root.filePath("work");
    std::shared_ptr<Model> model=std::make_shared<Model>();std::shared_ptr<a::ToolRegistry> tools=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>();a::EngineOptions options{.decision={.enabled=false}};
    Fixture(){QDir().mkpath(work);a::registerWorkspaceTools(*tools,work);options.sessionsDirectory=root.filePath("sessions");
        options.projectMemoryEnabled=true;options.memoryRecall.enabled=false;options.memoryExtraction.enabled=true;
        options.skills.enabled=false;options.projectContext.enabled=false;options.toolSearch.enabled=false;options.compaction.automatic=false;}
};
QJsonObject api(a::Api& host,const QString& method,const QJsonObject& params={},const QString& credential=QString(48,'a')) {
    return host.dispatch(method,params,credential).result.get().toObject();
}
QJsonObject exchange(iiMultimodalAgent::mcp::ServerSession& server,int id,const QString& method,QJsonObject params={}) {
    server.receive(QJsonObject{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}});
    for(int i=0;i<300;++i)for(const auto& item:server.takeMessages(10))if(item.toObject()["id"]==id)return item.toObject();
    throw std::runtime_error("MCP response timed out");
}
}
class MemoryExtractionEngineTests final {
public:
    void backgroundExtractionDoesNotDelayAnswerOrAppendItsConversation() {
        Fixture f;f.model->slow=true;a::Engine engine(f.model,f.tools,f.policy,f.options);const auto id=engine.createSession("fixture",f.work).id;
        QJsonArray receipts;const auto result=engine.run({id,"Use Korean for future reports"},[&](const a::Event& event){if(event.kind==a::EventKind::MemoryExtraction)receipts.append(event.data);}).result.get();
        CTEST_COMPARE(result.status,a::RunStatus::Completed);CTEST_CHECK(!receipts.isEmpty());CTEST_WAIT_VERIFY(f.model->started,3000);
        CTEST_COMPARE(engine.session(id).messages.size(),2);CTEST_CHECK(!engine.drainMemoryExtractions(5));f.model->slow=false;
        CTEST_CHECK(engine.drainMemoryExtractions(5000));const auto status=engine.memoryExtractionStatus(id);
        const auto record=status["records"].toArray().last().toObject();CTEST_CHECK_MESSAGE(record["status"]=="completed",QJsonDocument(record).toJson().constData());
        CTEST_COMPARE(record["saved_topics"].toArray().size(),1);CTEST_COMPARE(engine.session(id).messages.size(),2);
        CTEST_COMPARE(result.usage.generatedTokens,7);CTEST_COMPARE(record["usage"].toObject()["generated_tokens"].toInt(),15);
        CTEST_CHECK(engine.queuedInputs(id)["count"].toInt()==0);CTEST_COMPARE(engine.extractMemory(id)["status"].toString(),"up_to_date");
    }
    void offeredBeforeStopHooksAndClearCancelsWithoutReusingOldContext() {
        Fixture f;f.model->slow=true;bool offered=false,stopSawOffer=false;
        f.options.hooks.append([&](const a::HookInput& input,const auto&){if(input.kind==a::HookKind::Stop)stopSawOffer=offered;return a::HookResult{};});
        a::Engine engine(f.model,f.tools,f.policy,f.options);const auto id=engine.createSession("fixture",f.work).id;
        auto result=engine.run({id,"Use Korean"},[&](const a::Event& event){if(event.kind==a::EventKind::MemoryExtraction)offered=true;}).result.get();
        CTEST_COMPARE(result.status,a::RunStatus::Completed);CTEST_CHECK(stopSawOffer);CTEST_WAIT_VERIFY(f.model->started,3000);
        const auto cleared=engine.clearSession(id);CTEST_CHECK(cleared["complete"].toBool());CTEST_CHECK(f.model->cancelled);
        CTEST_CHECK(!engine.memoryExtractionStatus(id)["has_context"].toBool());CTEST_COMPARE(engine.extractMemory(cleared["session_id"].toString())["status"].toString(),"no_context");
    }
    void closeDrainsSuccessfulWorkAndDisabledHostNeverStartsExtraction() {
        Fixture f;std::atomic<int> hookContexts=0;
        f.options.hooks.append([&](const a::HookInput& input,const auto&) {
            if(input.kind==a::HookKind::BeforeTool) {
                if(!input.modelContext||!input.modelContext->agentExecutor||!input.modelContext->session||input.modelContext->session->parentSessionId.isEmpty())
                    throw std::runtime_error("Extraction omitted the host verifier or fork context");++hookContexts;
            }return a::HookResult{};
        });
        a::Engine engine(f.model,f.tools,f.policy,f.options);auto id=engine.createSession("fixture",f.work).id;
        CTEST_COMPARE(engine.run({id,"Use Korean"}).result.get().status,a::RunStatus::Completed);engine.close();
        const auto record=engine.memoryExtractionStatus(id)["records"].toArray().last().toObject();CTEST_COMPARE(record["status"].toString(),"completed");
        CTEST_COMPARE(record["tool_errors"].toInt(),0);CTEST_COMPARE(hookContexts.load(),1);
        f.options.memoryExtraction.enabled=false;f.options.sessionsDirectory=f.root.filePath("disabled");a::Engine disabled(f.model,f.tools,f.policy,f.options);
        const auto before=f.model->extractions.load();id=disabled.createSession("fixture",f.work).id;CTEST_COMPARE(disabled.run({id,"Use Korean"}).result.get().status,a::RunStatus::Completed);
        CTEST_COMPARE(f.model->extractions.load(),before);CTEST_COMPARE(disabled.extractMemory(id)["status"].toString(),"disabled");
    }
    void apiOwnersAndReservedControls() {
        Fixture f;f.model->slow=true;a::ApiOptions options{.engine={.decision={.enabled=false}}};options.stateDirectory=f.root.filePath("api");options.workingDirectory=f.work;
        options.engine=f.options;options.engine.sessionsDirectory.clear();options.clientTokens={{"society",QString(48,'a')},{"dreamscapes",QString(48,'b')}};
        a::Api host(f.model,f.tools,f.policy,options);CTEST_CHECK(host.isControlMethod("agent.memory.extraction.status"));CTEST_CHECK(host.isControlMethod("agent.memory.extraction.cancel"));
        const auto id=api(host,"agent.sessions.create",{{"model","fixture"}})["session_id"].toString();
        CTEST_COMPARE(api(host,"agent.memory.extract",{{"session_id",id}})["status"].toString(),"no_context");
        api(host,"agent.run",{{"session_id",id},{"prompt","Use Korean for reports"}});CTEST_WAIT_VERIFY(f.model->started,3000);
        CTEST_CHECK(api(host,"agent.memory.extraction.status",{{"session_id",id}})["active"].toBool());
        CTEST_THROWS(Error,api(host,"agent.memory.extraction.status",{{"session_id",id}},QString(48,'b')));
        CTEST_THROWS(Error,api(host,"agent.memory.extraction.cancel",{{"session_id",id}},QString(48,'b')));
        CTEST_THROWS(Error,api(host,"agent.memory.extract",{{"session_id",id},{"messages",QJsonArray{}}}));
        api(host,"agent.memory.extraction.cancel",{{"session_id",id}});CTEST_WAIT_VERIFY(f.model->cancelled,3000);
        CTEST_CHECK(api(host,"agent.info")["memory_extraction_enabled"].toBool());
    }
    void mcpBindsControlsToConnectionAndAdvertisesCapability() {
        Fixture f;auto engine=std::make_shared<a::Engine>(f.model,f.tools,f.policy,f.options);
        a::McpServerOptions options;options.engine=engine;options.model="fixture";options.workingDirectory=f.work;
        iiMultimodalAgent::mcp::ServerSession server(a::mcpServerOptions(f.tools,f.policy,options));
        const auto initialized=exchange(server,1,"initialize",{{"protocolVersion","2025-03-26"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","test"},{"version","1"}}}});
        CTEST_CHECK(QJsonDocument(initialized).toJson().contains("extractionTools"));server.receive(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/initialized"}});
        const auto receipt=exchange(server,2,"tools/call",{{"name","iiLocalLLM.agent.memory.extract"},{"arguments",QJsonObject{}}});
        CTEST_CHECK_MESSAGE(QJsonDocument(receipt).toJson().contains("no_context"),QJsonDocument(receipt).toJson().constData());
        const auto forged=exchange(server,3,"tools/call",{{"name","iiLocalLLM.agent.memory.extraction.status"},{"arguments",QJsonObject{{"session_id","foreign"}}}});
        CTEST_CHECK(forged.contains("error")||forged["result"].toObject()["isError"].toBool());
        const auto status=exchange(server,4,"iisacc/memory/extraction/status",{{"offset",0},{"limit",1}});
        CTEST_CHECK(status["result"].toObject()["enabled"].toBool());
        CTEST_CHECK(exchange(server,5,"iisacc/memory/extraction/cancel")["result"].toObject()["enabled"].toBool());
        CTEST_CHECK(exchange(server,6,"iisacc/memory/extraction/status",{{"session_id","foreign"}}).contains("error"));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner;
    MemoryExtractionEngineTests tests;
    runner.run("backgroundExtractionDoesNotDelayAnswerOrAppendItsConversation", [&] { tests.backgroundExtractionDoesNotDelayAnswerOrAppendItsConversation(); });
    runner.run("offeredBeforeStopHooksAndClearCancelsWithoutReusingOldContext", [&] { tests.offeredBeforeStopHooksAndClearCancelsWithoutReusingOldContext(); });
    runner.run("closeDrainsSuccessfulWorkAndDisabledHostNeverStartsExtraction", [&] { tests.closeDrainsSuccessfulWorkAndDisabledHostNeverStartsExtraction(); });
    runner.run("apiOwnersAndReservedControls", [&] { tests.apiOwnersAndReservedControls(); });
    runner.run("mcpBindsControlsToConnectionAndAdvertisesCapability", [&] { tests.mcpBindsControlsToConnectionAndAdvertisesCapability(); });
    return runner.result();
}

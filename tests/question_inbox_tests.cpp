#include <iiMultimodalAgent/agent/QuestionInbox.h>
#include <iiMultimodalAgent/agent/NativeQuestionBridge.h>
#include <iiMultimodalAgent/agent/UserQuestions.h>
#include <iiMultimodalAgent/agent/McpServer.h>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include "SignalRecorder.h"
#include <QCoreApplication>
#include <future>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
QJsonObject question() {return {{"questions",QJsonArray{QJsonObject{{"question","Which renderer?"},{"header","Renderer"},
    {"options",QJsonArray{QJsonObject{{"label","Qt"},{"description","Native"}},QJsonObject{{"label","Web"},{"description","Browser"}}}}}}}};}
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("question-inbox-XXXXXX")};std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    Fixture(){registry->add(a::userQuestionTool({false,"markdown"}));}
    std::future<a::ToolResult> run(a::Hook hook,QString id="owner",CancellationToken token={}) {
        return std::async(std::launch::async,[=,this] {
            a::ToolRunnerOptions options;options.hooks={hook};
            return a::ToolRunner(registry,policy,options).run({id,"AskUserQuestion",question()},{id,"run",root.path(),{},token});
        });
    }
};
QString id(a::QuestionInbox& box){return box.requests().first().toMap()["request_id"].toString();}
}
class QuestionInboxTests:public QObject {
public:
    void nativeViewModelReceivesDeltasAndSendsValidatedReplies() {
        Fixture f; a::QuestionInbox box;
        auto model=std::make_shared<a::presentation::NativeQuestionViewModel>();
        a::presentation::NativeQuestionBridge bridge(&box,model);
        auto first=f.run(box.hook(),"native-first");
        CTEST_WAIT_COMPARE(model->rows.get().size(),std::size_t(1));
        auto row=model->rows.get().front();
        row->editOther("Direct answer");
        auto second=f.run(box.hook(),"native-second");
        CTEST_WAIT_DEFAULT(model->hasWaiting.get());
        CTEST_COMPARE(model->rows.get().front(),row);
        CTEST_COMPARE(row->other.get(),std::string("Direct answer"));
        model->submit();
        CTEST_WAIT_COMPARE(box.requests().size(),1);
        CTEST_COMPARE(first.get().data["answers"].toObject()["Which renderer?"],"Direct answer");
        CTEST_WAIT_COMPARE(model->rows.get().size(),std::size_t(1));
        CTEST_CHECK(!row->active());
        model->rows.get().front()->options.front()->toggled(true);
        model->submit();
        CTEST_WAIT_DEFAULT(box.requests().isEmpty());
        CTEST_COMPARE(second.get().data["answers"].toObject()["Which renderer?"],"Qt");
        CTEST_CHECK(!model->open.get());
    }
    void nativeViewModelRecoversAMissedRequestEvent() {
        Fixture f; a::QuestionInbox box;
        auto model=std::make_shared<a::presentation::NativeQuestionViewModel>();
        a::presentation::NativeQuestionBridge bridge(&box,model);
        auto first=f.run(box.hook(),"recovery-first");
        CTEST_WAIT_COMPARE(model->rows.get().size(),std::size_t(1));
        const auto firstId=box.requests().first().toMap()["request_id"].toString().toStdString();
        CTEST_CHECK(model->change.send({firstId,std::nullopt,0})); // Simulate a missed notification.
        auto second=f.run(box.hook(),"recovery-second");
        CTEST_WAIT_DEFAULT(model->hasWaiting.get());
        CTEST_COMPARE(model->read.send({})->requests.size(),std::size_t(2));
        model->skip(); CTEST_WAIT_COMPARE(box.requests().size(),1);
        model->decline(); CTEST_WAIT_DEFAULT(box.requests().isEmpty());
        CTEST_CHECK(!first.get().isError); CTEST_CHECK(second.get().isError);
    }
    void retiredNativeBridgeCannotSubmit() {
        Fixture f; a::QuestionInbox box;
        auto model=std::make_shared<a::presentation::NativeQuestionViewModel>();
        auto bridge=std::make_unique<a::presentation::NativeQuestionBridge>(&box,model);
        auto pending=f.run(box.hook(),"retired");
        CTEST_WAIT_COMPARE(model->rows.get().size(),std::size_t(1));
        bridge.reset();
        model->submit(); CTEST_COMPARE(box.requests().size(),1);
        CTEST_CHECK(box.reject(id(box))); CTEST_CHECK(pending.get().isError);
    }
    void changedRequestCanBeQueriedWithoutReadingTheWholeQueue() {
        Fixture f; a::QuestionInbox box;
        agent_ctest::SignalRecorder deltas(&box, &a::QuestionInbox::requestDelta);
        agent_ctest::SignalRecorder reconciled(&box, &a::QuestionInbox::requestsReconciled);
        auto first=f.run(box.hook(),"first");
        CTEST_WAIT_COMPARE(deltas.size(),1);
        const auto firstId=deltas.last().at(0).toString();
        const auto selected=box.requestSnapshot(firstId);
        CTEST_COMPARE(selected.position,0);
        CTEST_COMPARE(selected.request["request_id"].toString(),firstId);
        auto second=f.run(box.hook(),"second");
        CTEST_WAIT_COMPARE(deltas.size(),2);
        const auto secondId=deltas.last().at(0).toString();
        CTEST_COMPARE(box.requestSnapshot(secondId).position,1);
        CTEST_CHECK(box.submit(firstId,{}));
        CTEST_WAIT_COMPARE(deltas.size(),3);
        CTEST_COMPARE(box.requestSnapshot(firstId).position,-1);
        CTEST_COMPARE(box.requestSnapshot(secondId).position,0);
        box.refresh();
        CTEST_COMPARE(reconciled.size(),1); // Explicit recovery is reported without a fake change.
        CTEST_CHECK(box.reject(secondId));
        CTEST_CHECK(!first.get().isError); CTEST_CHECK(second.get().isError);
    }
    void idleInboxDoesNotPoll() {
        a::QuestionInbox box;
        CTEST_CHECK(box.findChildren<QTimer*>().isEmpty());
        agent_ctest::SignalRecorder changed(&box,&a::QuestionInbox::requestsChanged);
        agent_ctest::processFor(150);
        CTEST_COMPARE(changed.size(),0);
    }
    void mcpQuestionWaitDoesNotLockAppMutations() {
        Fixture f;a::QuestionInbox box;
        a::Tool mutation;mutation.definition={"navigate","Mutate app state",{{"type","object"}},{},false,false};
        mutation.execute=[](const auto&,const auto&){return a::ToolResult{"navigation completed"};};f.registry->add(mutation);
        a::McpServerOptions options;options.workingDirectory=f.root.path();options.tools.hooks={box.hook()};
        auto server=a::mcpServerOptions(f.registry,f.policy,options);iiMultimodalAgent::mcp::ServerRequestContext context;context.sessionId="connection";
        bool marked=false;for(const auto& row:server.lists.at("tools/list")(context))if(row.toObject()["name"]=="AskUserQuestion")
            marked=row.toObject()["_meta"].toObject()["iisacc/userInteraction"]==true;CTEST_CHECK(marked);
        auto call=server.handlers.at("tools/call");
        auto pending=std::async(std::launch::async,[&]{return call({{"name","AskUserQuestion"},{"arguments",question()}},context);});
        CTEST_WAIT_COMPARE(box.requests().size(),1);
        auto navigating=std::async(std::launch::async,[&]{return call({{"name","navigate"},{"arguments",QJsonObject{}}},context);});
        const auto independent=navigating.wait_for(std::chrono::seconds(1))==std::future_status::ready;
        box.close();CTEST_CHECK(pending.get()["isError"].toBool());CTEST_CHECK(!navigating.get()["isError"].toBool());CTEST_CHECK(independent);
    }
    void workerQuestionAppearsAndReceivesGuiAnswer() {
        Fixture f;a::QuestionInbox box;auto pending=f.run(box.hook());CTEST_WAIT_COMPARE(box.requests().size(),1);
        const auto request=box.requests().first().toMap();CTEST_COMPARE(request["session_id"].toString(),"owner");
        CTEST_CHECK(box.submit(id(box),{{"Which renderer?","Native Qt"}},{{"Which renderer?",QVariantMap{{"notes","사용자 메모"}}}}));
        auto result=pending.get();CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(result.data["answers"].toObject()["Which renderer?"],"Native Qt");
        CTEST_COMPARE(result.data["annotations"].toObject()["Which renderer?"].toObject()["notes"],"사용자 메모");CTEST_CHECK(box.requests().isEmpty());
    }
    void stableSnapshotsDoNotResetTheUi() {
        Fixture f;a::QuestionInbox box;auto pending=f.run(box.hook());CTEST_WAIT_COMPARE(box.requests().size(),1);
        agent_ctest::SignalRecorder changed(&box,&a::QuestionInbox::requestsChanged);for(int i=0;i<10;++i)box.refresh();CTEST_COMPARE(changed.size(),0);
        const auto first=id(box);CTEST_CHECK(box.submit(first,{}));CTEST_CHECK(!pending.get().isError);CTEST_COMPARE(changed.size(),1);
        CTEST_CHECK(!box.submit(first,{{"Which renderer?","late"}}));CTEST_CHECK(!box.errorString().isEmpty());
    }
    void malformedUiInputDoesNotConsumeTheRequest() {
        Fixture f;a::QuestionInbox box;auto pending=f.run(box.hook());CTEST_WAIT_COMPARE(box.requests().size(),1);const auto key=id(box);
        CTEST_CHECK(!box.submit(key,{{"unasked","fake"}}));CTEST_COMPARE(box.requests().size(),1);
        CTEST_CHECK(!box.submit(key,{{"Which renderer?",42}}));CTEST_COMPARE(box.requests().size(),1);
        CTEST_CHECK(box.submit(key,{{"Which renderer?","Qt"}}));CTEST_CHECK(box.errorString().isEmpty());CTEST_CHECK(!pending.get().isError);
    }
    void concurrentOwnersAreAnsweredIndependently() {
        Fixture f;a::QuestionInbox box;auto first=f.run(box.hook(),"first");CTEST_WAIT_COMPARE(box.requests().size(),1);const auto firstId=id(box);
        auto second=f.run(box.hook(),"second");CTEST_WAIT_COMPARE(box.requests().size(),2);CTEST_COMPARE(id(box),firstId);
        const auto secondId=box.requests().last().toMap()["request_id"].toString();
        CTEST_CHECK(box.submit(secondId,{{"Which renderer?","SECOND"}}));CTEST_COMPARE(second.get().data["answers"].toObject()["Which renderer?"],"SECOND");
        CTEST_COMPARE(box.requests().size(),1);CTEST_COMPARE(id(box),firstId);CTEST_CHECK(box.reject(firstId,"Declined"));CTEST_CHECK(first.get().isError);
    }
    void cancellationExpiryAndDestructionReleaseWorkers() {
        Fixture f;a::QuestionInbox box;CancellationToken token;auto pending=f.run(box.hook(),"cancelled",token);
        CTEST_WAIT_COMPARE(box.requests().size(),1);token.cancel();CTEST_THROWS(Error,(void)pending.get());box.refresh();CTEST_CHECK(box.requests().isEmpty());
        a::PermissionRequestsOptions config;config.timeoutMs=100;a::QuestionInbox timed(config);
        auto expires=f.run(timed.hook(),"expired");CTEST_CHECK(expires.get().isError);timed.refresh();CTEST_CHECK(timed.requests().isEmpty());
        auto owned=std::make_unique<a::QuestionInbox>();auto closing=f.run(owned->hook(),"closing");CTEST_WAIT_COMPARE(owned->requests().size(),1);
        owned.reset();CTEST_CHECK(closing.get().isError);
    }
    void borrowedChannelSurvivesViewDestruction() {
        auto broker=std::make_shared<a::PermissionRequests>();{
            a::QuestionInbox box(broker);CTEST_CHECK(box.requests().isEmpty());
        }CTEST_CHECK(!broker->pending()["closed"].toBool());
    }
    void sharedChannelPublishesChangesToEachView() {
        Fixture f;auto broker=std::make_shared<a::PermissionRequests>();
        auto first=std::make_unique<a::QuestionInbox>(broker);a::QuestionInbox second(broker);
        auto pending=f.run(first->hook());
        CTEST_WAIT_COMPARE(first->requests().size(),1);CTEST_WAIT_COMPARE(second.requests().size(),1);
        a::QuestionInbox late(broker);CTEST_COMPARE(late.requests().size(),1);
        const auto key=id(second);CTEST_COMPARE(id(*first),key);
        first.reset();
        CTEST_CHECK(second.submit(key,{{"Which renderer?","Native"}}));
        CTEST_COMPARE(second.requests().size(),0);CTEST_CHECK(!pending.get().isError);
        CTEST_WAIT_COMPARE(late.requests().size(),0);agent_ctest::processFor(20);CTEST_COMPARE(second.requests().size(),0);
        CTEST_CHECK(!broker->pending()["closed"].toBool());
    }
    void viewCanRetireDuringChangeNotification() {
        Fixture f;auto box=std::make_unique<a::QuestionInbox>();auto pending=f.run(box->hook());
        CTEST_WAIT_COMPARE(box->requests().size(),1);const auto key=id(*box);
        QObject::connect(box.get(),&a::QuestionInbox::requestsChanged,box.get(),[&]{box.reset();});
        CTEST_CHECK(box->submit(key,{}));CTEST_CHECK(!box);CTEST_CHECK(!pending.get().isError);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) { std::cerr << "Expected a case name" << std::endl; return 1; }
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    QuestionInboxTests tests;
    runner.run("nativeViewModelReceivesDeltasAndSendsValidatedReplies", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.nativeViewModelReceivesDeltasAndSendsValidatedReplies(); });
    runner.run("nativeViewModelRecoversAMissedRequestEvent", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.nativeViewModelRecoversAMissedRequestEvent(); });
    runner.run("retiredNativeBridgeCannotSubmit", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.retiredNativeBridgeCannotSubmit(); });
    runner.run("changedRequestCanBeQueriedWithoutReadingTheWholeQueue", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.changedRequestCanBeQueriedWithoutReadingTheWholeQueue(); });
    runner.run("idleInboxDoesNotPoll", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.idleInboxDoesNotPoll(); });
    runner.run("mcpQuestionWaitDoesNotLockAppMutations", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.mcpQuestionWaitDoesNotLockAppMutations(); });
    runner.run("workerQuestionAppearsAndReceivesGuiAnswer", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.workerQuestionAppearsAndReceivesGuiAnswer(); });
    runner.run("stableSnapshotsDoNotResetTheUi", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.stableSnapshotsDoNotResetTheUi(); });
    runner.run("malformedUiInputDoesNotConsumeTheRequest", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.malformedUiInputDoesNotConsumeTheRequest(); });
    runner.run("concurrentOwnersAreAnsweredIndependently", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.concurrentOwnersAreAnsweredIndependently(); });
    runner.run("cancellationExpiryAndDestructionReleaseWorkers", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.cancellationExpiryAndDestructionReleaseWorkers(); });
    runner.run("borrowedChannelSurvivesViewDestruction", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.borrowedChannelSurvivesViewDestruction(); });
    runner.run("sharedChannelPublishesChangesToEachView", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.sharedChannelPublishesChangesToEachView(); });
    runner.run("viewCanRetireDuringChangeNotification", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.viewCanRetireDuringChangeNotification(); });
    return runner.result();
}

#include <iiMultimodalAgent/agent/QuestionInbox.h>
#include <iiMultimodalAgent/agent/NativeQuestionBridge.h>
#include <iiMultimodalAgent/agent/UserQuestions.h>
#include <iiMultimodalAgent/agent/McpServer.h>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtTest/QtTest>
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
    Q_OBJECT
private slots:
    void nativeViewModelReceivesDeltasAndSendsValidatedReplies() {
        Fixture f; a::QuestionInbox box;
        auto model=std::make_shared<a::presentation::NativeQuestionViewModel>();
        a::presentation::NativeQuestionBridge bridge(&box,model);
        auto first=f.run(box.hook(),"native-first");
        QTRY_COMPARE(model->rows.get().size(),std::size_t(1));
        auto row=model->rows.get().front();
        row->editOther("Direct answer");
        auto second=f.run(box.hook(),"native-second");
        QTRY_VERIFY(model->hasWaiting.get());
        QCOMPARE(model->rows.get().front(),row);
        QCOMPARE(row->other.get(),std::string("Direct answer"));
        model->submit();
        QTRY_COMPARE(box.requests().size(),1);
        QCOMPARE(first.get().data["answers"].toObject()["Which renderer?"],"Direct answer");
        QTRY_COMPARE(model->rows.get().size(),std::size_t(1));
        QVERIFY(!row->active());
        model->rows.get().front()->options.front()->toggled(true);
        model->submit();
        QTRY_VERIFY(box.requests().isEmpty());
        QCOMPARE(second.get().data["answers"].toObject()["Which renderer?"],"Qt");
        QVERIFY(!model->open.get());
    }
    void nativeViewModelRecoversAMissedRequestEvent() {
        Fixture f; a::QuestionInbox box;
        auto model=std::make_shared<a::presentation::NativeQuestionViewModel>();
        a::presentation::NativeQuestionBridge bridge(&box,model);
        auto first=f.run(box.hook(),"recovery-first");
        QTRY_COMPARE(model->rows.get().size(),std::size_t(1));
        const auto firstId=box.requests().first().toMap()["request_id"].toString().toStdString();
        QVERIFY(model->change.send({firstId,std::nullopt,0})); // Simulate a missed notification.
        auto second=f.run(box.hook(),"recovery-second");
        QTRY_VERIFY(model->hasWaiting.get());
        QCOMPARE(model->read.send({})->requests.size(),std::size_t(2));
        model->skip(); QTRY_COMPARE(box.requests().size(),1);
        model->decline(); QTRY_VERIFY(box.requests().isEmpty());
        QVERIFY(!first.get().isError); QVERIFY(second.get().isError);
    }
    void retiredNativeBridgeCannotSubmit() {
        Fixture f; a::QuestionInbox box;
        auto model=std::make_shared<a::presentation::NativeQuestionViewModel>();
        auto bridge=std::make_unique<a::presentation::NativeQuestionBridge>(&box,model);
        auto pending=f.run(box.hook(),"retired");
        QTRY_COMPARE(model->rows.get().size(),std::size_t(1));
        bridge.reset();
        model->submit(); QCOMPARE(box.requests().size(),1);
        QVERIFY(box.reject(id(box))); QVERIFY(pending.get().isError);
    }
    void changedRequestCanBeQueriedWithoutReadingTheWholeQueue() {
        Fixture f; a::QuestionInbox box;
        QSignalSpy deltas(&box, &a::QuestionInbox::requestDelta);
        QSignalSpy reconciled(&box, &a::QuestionInbox::requestsReconciled);
        auto first=f.run(box.hook(),"first");
        QTRY_COMPARE(deltas.size(),1);
        const auto firstId=deltas.last().at(0).toString();
        const auto selected=box.requestSnapshot(firstId);
        QCOMPARE(selected.position,0);
        QCOMPARE(selected.request["request_id"].toString(),firstId);
        auto second=f.run(box.hook(),"second");
        QTRY_COMPARE(deltas.size(),2);
        const auto secondId=deltas.last().at(0).toString();
        QCOMPARE(box.requestSnapshot(secondId).position,1);
        QVERIFY(box.submit(firstId,{}));
        QTRY_COMPARE(deltas.size(),3);
        QCOMPARE(box.requestSnapshot(firstId).position,-1);
        QCOMPARE(box.requestSnapshot(secondId).position,0);
        box.refresh();
        QCOMPARE(reconciled.size(),1); // Explicit recovery is reported without a fake change.
        QVERIFY(box.reject(secondId));
        QVERIFY(!first.get().isError); QVERIFY(second.get().isError);
    }
    void idleInboxDoesNotPoll() {
        a::QuestionInbox box;
        QVERIFY(box.findChildren<QTimer*>().isEmpty());
        QSignalSpy changed(&box,&a::QuestionInbox::requestsChanged);
        QTest::qWait(150);
        QCOMPARE(changed.size(),0);
    }
    void mcpQuestionWaitDoesNotLockAppMutations() {
        Fixture f;a::QuestionInbox box;
        a::Tool mutation;mutation.definition={"navigate","Mutate app state",{{"type","object"}},{},false,false};
        mutation.execute=[](const auto&,const auto&){return a::ToolResult{"navigation completed"};};f.registry->add(mutation);
        a::McpServerOptions options;options.workingDirectory=f.root.path();options.tools.hooks={box.hook()};
        auto server=a::mcpServerOptions(f.registry,f.policy,options);iiMultimodalAgent::mcp::ServerRequestContext context;context.sessionId="connection";
        bool marked=false;for(const auto& row:server.lists.at("tools/list")(context))if(row.toObject()["name"]=="AskUserQuestion")
            marked=row.toObject()["_meta"].toObject()["iisacc/userInteraction"]==true;QVERIFY(marked);
        auto call=server.handlers.at("tools/call");
        auto pending=std::async(std::launch::async,[&]{return call({{"name","AskUserQuestion"},{"arguments",question()}},context);});
        QTRY_COMPARE(box.requests().size(),1);
        auto navigating=std::async(std::launch::async,[&]{return call({{"name","navigate"},{"arguments",QJsonObject{}}},context);});
        const auto independent=navigating.wait_for(std::chrono::seconds(1))==std::future_status::ready;
        box.close();QVERIFY(pending.get()["isError"].toBool());QVERIFY(!navigating.get()["isError"].toBool());QVERIFY(independent);
    }
    void workerQuestionAppearsAndReceivesGuiAnswer() {
        Fixture f;a::QuestionInbox box;auto pending=f.run(box.hook());QTRY_COMPARE(box.requests().size(),1);
        const auto request=box.requests().first().toMap();QCOMPARE(request["session_id"].toString(),"owner");
        QVERIFY(box.submit(id(box),{{"Which renderer?","Native Qt"}},{{"Which renderer?",QVariantMap{{"notes","사용자 메모"}}}}));
        auto result=pending.get();QVERIFY2(!result.isError,qPrintable(result.text));QCOMPARE(result.data["answers"].toObject()["Which renderer?"],"Native Qt");
        QCOMPARE(result.data["annotations"].toObject()["Which renderer?"].toObject()["notes"],"사용자 메모");QVERIFY(box.requests().isEmpty());
    }
    void stableSnapshotsDoNotResetTheUi() {
        Fixture f;a::QuestionInbox box;auto pending=f.run(box.hook());QTRY_COMPARE(box.requests().size(),1);
        QSignalSpy changed(&box,&a::QuestionInbox::requestsChanged);for(int i=0;i<10;++i)box.refresh();QCOMPARE(changed.size(),0);
        const auto first=id(box);QVERIFY(box.submit(first,{}));QVERIFY(!pending.get().isError);QCOMPARE(changed.size(),1);
        QVERIFY(!box.submit(first,{{"Which renderer?","late"}}));QVERIFY(!box.errorString().isEmpty());
    }
    void malformedUiInputDoesNotConsumeTheRequest() {
        Fixture f;a::QuestionInbox box;auto pending=f.run(box.hook());QTRY_COMPARE(box.requests().size(),1);const auto key=id(box);
        QVERIFY(!box.submit(key,{{"unasked","fake"}}));QCOMPARE(box.requests().size(),1);
        QVERIFY(!box.submit(key,{{"Which renderer?",42}}));QCOMPARE(box.requests().size(),1);
        QVERIFY(box.submit(key,{{"Which renderer?","Qt"}}));QVERIFY(box.errorString().isEmpty());QVERIFY(!pending.get().isError);
    }
    void concurrentOwnersAreAnsweredIndependently() {
        Fixture f;a::QuestionInbox box;auto first=f.run(box.hook(),"first");QTRY_COMPARE(box.requests().size(),1);const auto firstId=id(box);
        auto second=f.run(box.hook(),"second");QTRY_COMPARE(box.requests().size(),2);QCOMPARE(id(box),firstId);
        const auto secondId=box.requests().last().toMap()["request_id"].toString();
        QVERIFY(box.submit(secondId,{{"Which renderer?","SECOND"}}));QCOMPARE(second.get().data["answers"].toObject()["Which renderer?"],"SECOND");
        QCOMPARE(box.requests().size(),1);QCOMPARE(id(box),firstId);QVERIFY(box.reject(firstId,"Declined"));QVERIFY(first.get().isError);
    }
    void cancellationExpiryAndDestructionReleaseWorkers() {
        Fixture f;a::QuestionInbox box;CancellationToken token;auto pending=f.run(box.hook(),"cancelled",token);
        QTRY_COMPARE(box.requests().size(),1);token.cancel();QVERIFY_THROWS_EXCEPTION(Error,(void)pending.get());box.refresh();QVERIFY(box.requests().isEmpty());
        a::PermissionRequestsOptions config;config.timeoutMs=100;a::QuestionInbox timed(config);
        auto expires=f.run(timed.hook(),"expired");QVERIFY(expires.get().isError);timed.refresh();QVERIFY(timed.requests().isEmpty());
        auto owned=std::make_unique<a::QuestionInbox>();auto closing=f.run(owned->hook(),"closing");QTRY_COMPARE(owned->requests().size(),1);
        owned.reset();QVERIFY(closing.get().isError);
    }
    void borrowedChannelSurvivesViewDestruction() {
        auto broker=std::make_shared<a::PermissionRequests>();{
            a::QuestionInbox box(broker);QVERIFY(box.requests().isEmpty());
        }QVERIFY(!broker->pending()["closed"].toBool());
    }
    void sharedChannelPublishesChangesToEachView() {
        Fixture f;auto broker=std::make_shared<a::PermissionRequests>();
        auto first=std::make_unique<a::QuestionInbox>(broker);a::QuestionInbox second(broker);
        auto pending=f.run(first->hook());
        QTRY_COMPARE(first->requests().size(),1);QTRY_COMPARE(second.requests().size(),1);
        a::QuestionInbox late(broker);QCOMPARE(late.requests().size(),1);
        const auto key=id(second);QCOMPARE(id(*first),key);
        first.reset();
        QVERIFY(second.submit(key,{{"Which renderer?","Native"}}));
        QCOMPARE(second.requests().size(),0);QVERIFY(!pending.get().isError);
        QTRY_COMPARE(late.requests().size(),0);QTest::qWait(20);QCOMPARE(second.requests().size(),0);
        QVERIFY(!broker->pending()["closed"].toBool());
    }
    void viewCanRetireDuringChangeNotification() {
        Fixture f;auto box=std::make_unique<a::QuestionInbox>();auto pending=f.run(box->hook());
        QTRY_COMPARE(box->requests().size(),1);const auto key=id(*box);
        QObject::connect(box.get(),&a::QuestionInbox::requestsChanged,box.get(),[&]{box.reset();});
        QVERIFY(box->submit(key,{}));QVERIFY(!box);QVERIFY(!pending.get().isError);
    }
};
QTEST_GUILESS_MAIN(QuestionInboxTests)
#include "question_inbox_tests.moc"

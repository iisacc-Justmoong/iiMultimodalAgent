#include "native_link.h"
#include <iiMultimodalAgent/agent/MemoryDream.h>
#include <iiMultimodalAgent/agent/SessionStore.h>
#include "QtEventWait.h"
#include <QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QThread>
#include <QtCore/QLockFile>
#include <QtCore/QProcess>
#include <atomic>
#include <iostream>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
constexpr int fixtureWaitMs = 30000; // Process startup and file publication can be slow on external storage.
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
class Model final:public a::Model {
public:
    std::atomic<int> calls=0;std::function<a::ModelReply(const a::ModelRequest&,const CancellationToken&)> action;
    a::ModelReply generate(const a::ModelRequest& input,const CancellationToken& token,const std::function<bool(const QString&)>&)override {
        ++calls;return action?action(input,token):a::ModelReply{"The existing memories need no changes.",{},Usage{21,9}};
    }
};
struct Fixture {
    QTemporaryDir root{QDir::temp().filePath("dream-XXXXXX")};QString work=root.filePath("work"),sessions=root.filePath("sessions"),owner;
    a::SessionStore store{sessions};std::shared_ptr<Model> model=std::make_shared<Model>();
    std::shared_ptr<a::ProjectMemory> memory=std::make_shared<a::ProjectMemory>(a::ProjectMemoryOptions{root.filePath("memory")});
    std::shared_ptr<a::SessionHistory> history=std::make_shared<a::SessionHistory>(sessions);
    std::shared_ptr<a::ToolRegistry> tools=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>();a::MemoryDreamOptions options;
    Fixture(){QDir().mkpath(work);owner=store.create("fixture","system",work).id;a::registerWorkspaceTools(*tools,work,{},QStringList{sessions});
        memory->bindWorkspaceTools(*tools);tools->add(history->tool());options.automatic=true;}
    QString prior(QString text="Durable prior context") {auto id=store.create("fixture","system",work).id;store.acquire(id)->append({{},a::MessageRole::User,text});return id;}
    a::MemoryContext snapshot(int turn=1) {
        a::MemoryContext s;s.sessionId=owner;s.workspace=work;s.registry=tools;s.context={owner,{},work};s.context.protectedPaths={sessions};
        s.request.model="fixture";s.request.systemPrompt="Parent system";s.request.contextId=owner;s.request.generation.temperature=.31;s.request.tools=tools->definitions();
        s.messages={{"u"+QString::number(turn),a::MessageRole::User,"Continue our work"},{"a"+QString::number(turn),a::MessageRole::Assistant,"Done"}};s.request.messages=s.messages;return s;
    }
    QJsonObject latest(a::MemoryDream& dream) {const auto list=dream.status(owner)["records"].toArray();require(!list.isEmpty(),"No dream record");return list.last().toObject();}
};
}
class MemoryDreamTests final {
public:
    void automaticGatesAndSuccessfulTimeSurviveRestart() {
        Fixture f;for(int i=0;i<5;++i)f.prior();
        {
            a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);dream.offer(f.snapshot());CTEST_CHECK(dream.drain(fixtureWaitMs));
            CTEST_COMPARE(f.latest(dream)["status"].toString(),"completed");CTEST_COMPARE(f.latest(dream)["sessions_reviewing"].toInt(),5);
            const auto stamp=f.latest(dream)["last_consolidated_ms"].toInteger();CTEST_CHECK(stamp>0);
            dream.offer(f.snapshot(2));CTEST_CHECK(dream.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(dream)["status"].toString(),"time_gate");CTEST_COMPARE(f.model->calls.load(),1);
        }
        a::MemoryDream resumed(f.memory,f.history,f.model,f.policy,f.options);resumed.offer(f.snapshot(3));CTEST_CHECK(resumed.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(resumed)["status"].toString(),"time_gate");CTEST_COMPARE(f.model->calls.load(),1);
    }
    void sessionGateScanThrottleAndExplicitRequest() {
        Fixture f;f.prior();a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);
        dream.offer(f.snapshot());CTEST_CHECK(dream.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(dream)["status"].toString(),"session_gate");
        dream.offer(f.snapshot(2));CTEST_CHECK(dream.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(dream)["status"].toString(),"scan_throttled");
        dream.request(f.owner);CTEST_CHECK(dream.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(dream)["status"].toString(),"completed");CTEST_COMPARE(f.model->calls.load(),1);
        f.options.automatic=false;a::MemoryDream manual(f.memory,f.history,f.model,f.policy,f.options);
        CTEST_COMPARE(manual.offer(f.snapshot())["status"].toString(),"retained");CTEST_COMPARE(f.model->calls.load(),1);
        manual.request(f.owner);CTEST_CHECK(manual.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(manual)["status"].toString(),"completed");
    }
    void sameParentPrefixHistorySearchAndNativeConsolidation() {
        Fixture f;const auto old=f.prior("A durable preference uses ORCHID");f.options.automatic=false;
        const auto parent=f.snapshot();const auto path=QDir(f.memory->directory(f.work)).filePath("preferences.md");int round=0;
        f.model->action=[&](const a::ModelRequest& request,const auto&)->a::ModelReply {
            require(request.model==parent.request.model&&request.systemPrompt==parent.request.systemPrompt&&request.contextId==parent.request.contextId,"Changed parent configuration");
            require(request.generation.temperature==parent.request.generation.temperature&&request.tools.size()==parent.request.tools.size(),"Changed parent sampling or tools");
            require(request.messages.mid(0,parent.request.messages.size())==parent.request.messages,"Changed parent prefix");
            if(++round==1)return {{},{{"search","SessionSearch",{{"query","ORCHID"}}}}};
            if(round==2){require(!request.messages.last().isError,"History search failed");require(request.messages.last().data["matches"].toArray().first().toObject()["session_id"]==old,"Search used another owner");
                return {{},{{"write","Write",{{"path",path},{"content","---\nname: Preference\ndescription: Durable preference\ntype: feedback\n---\nUse ORCHID.\n"}}}}};}
            require(!request.messages.last().isError,"Memory write failed");return {"Consolidated preference."};
        };
        a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);dream.offer(parent);dream.request(f.owner);CTEST_CHECK(dream.drain(fixtureWaitMs));
        const auto result=f.latest(dream);CTEST_CHECK_MESSAGE(result["status"]=="completed",QJsonDocument(result).toJson().constData());
        CTEST_COMPARE(result["written_paths"].toArray(),QJsonArray{path});CTEST_COMPARE(result["phase"].toString(),"updating");CTEST_COMPARE(result["recent_turns"].toArray().size(),3);
        CTEST_CHECK(f.store.load(f.owner).messages.isEmpty());
    }
    void cancellationAndOtherOwnerLockDoNotAdvanceSuccessfulTime() {
        Fixture f;for(int i=0;i<5;++i)f.prior();std::atomic<bool> started=false;
        f.model->action=[&](const auto&,const CancellationToken& token)->a::ModelReply{started=true;while(!token.isCancelled())QThread::msleep(1);token.throwIfCancelled();return {};};
        a::MemoryDream first(f.memory,f.history,f.model,f.policy,f.options);first.offer(f.snapshot());CTEST_WAIT_VERIFY(started,3000);
        a::MemoryDream other(f.memory,f.history,f.model,f.policy,f.options);other.offer(f.snapshot());CTEST_CHECK(other.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(other)["status"].toString(),"locked");
        first.cancel(f.owner);CTEST_CHECK(first.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(first)["status"].toString(),"cancelled");CTEST_COMPARE(f.latest(first)["last_consolidated_ms"].toInteger(),0);
        f.model->action={};other.request(f.owner);CTEST_CHECK(other.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(other)["status"].toString(),"completed");
    }
    void capabilityFailureAndModelFailureAreRetryable() {
        Fixture f;f.options.automatic=false;int callback=0;auto evil=f.tools->get("Read");evil.definition.name="ExternalRead";evil.isMcp=true;
        evil.validate=[&](const auto&,const auto&){++callback;};f.tools->add(evil);int round=0;
        f.model->action=[&](const auto&,const auto&)->a::ModelReply {
            if(++round==1)return {{},{{"external","ExternalRead",{{"path",f.work}}},{"write","Write",{{"path",QDir(f.work).filePath("bad.md")},{"content","bad"}}}}};return {"Unable to save."};
        };
        a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);dream.offer(f.snapshot());dream.request(f.owner);CTEST_CHECK(dream.drain(fixtureWaitMs));
        CTEST_COMPARE(f.latest(dream)["status"].toString(),"tool_error");CTEST_COMPARE(callback,0);CTEST_CHECK(!QFileInfo::exists(QDir(f.work).filePath("bad.md")));CTEST_COMPARE(f.latest(dream)["last_consolidated_ms"].toInteger(),0);
        f.model->action=[](const auto&,const auto&)->a::ModelReply{throw std::runtime_error("model failed");};dream.request(f.owner);CTEST_CHECK(dream.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(dream)["status"].toString(),"failed");
        f.model->action={};dream.request(f.owner);CTEST_CHECK(dream.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(dream)["status"].toString(),"completed");
    }
    void liveProcessLockAndDeadHolderRecovery() {
        Fixture f;f.options.automatic=false;const auto directory=QFileInfo(f.memory->directory(f.work)).absolutePath();
        QProcess child;child.start(QCoreApplication::applicationFilePath(),{"--hold-dream-lock",directory+"/.dream.lock"});
        CTEST_CHECK(child.waitForStarted(fixtureWaitMs));CTEST_CHECK(child.waitForReadyRead(fixtureWaitMs));CTEST_CHECK(child.readAllStandardOutput().contains("LOCKED"));
        a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);dream.offer(f.snapshot());dream.request(f.owner);
        CTEST_CHECK(dream.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(dream)["status"].toString(),"locked");CTEST_COMPARE(f.model->calls.load(),0);
        CTEST_CHECK(!QFileInfo::exists(directory+"/.dream-state.json"));
        child.kill();CTEST_CHECK(child.waitForFinished(fixtureWaitMs));CTEST_CHECK(QFileInfo::exists(directory+"/.dream.lock"));
        dream.request(f.owner);CTEST_CHECK(dream.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(dream)["status"].toString(),"completed");
        CTEST_CHECK(!QFileInfo::exists(directory+"/.dream.lock"));CTEST_CHECK(QFileInfo::exists(directory+"/.dream-state.json"));
    }
    void corruptStateAndSymlinkFailWithoutRunningModel() {
        Fixture f;f.options.automatic=false;const auto directory=QFileInfo(f.memory->directory(f.work)).absolutePath();
        const auto state=directory+"/.dream-state.json";QFile file(state);CTEST_CHECK(file.open(QIODevice::WriteOnly));file.write("{broken");file.close();
        a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);dream.offer(f.snapshot());dream.request(f.owner);
        CTEST_CHECK(dream.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(dream)["status"].toString(),"failed");CTEST_COMPARE(f.model->calls.load(),0);
        CTEST_CHECK(file.remove());QFile outside(f.root.filePath("other.json"));CTEST_CHECK(outside.open(QIODevice::WriteOnly));
        const QByteArray bytes="{\"version\":1,\"last_consolidated_ms\":0}";outside.write(bytes);outside.close();
#ifdef Q_OS_UNIX
        CTEST_CHECK(createNativeTestLink(outside.fileName(),state));dream.request(f.owner);CTEST_CHECK(dream.drain(fixtureWaitMs));
        CTEST_COMPARE(f.latest(dream)["status"].toString(),"failed");CTEST_COMPARE(f.model->calls.load(),0);CTEST_CHECK(QFile::remove(state));
        CTEST_CHECK(createNativeTestLink(outside.fileName(),directory+"/.dream.lock"));dream.request(f.owner);CTEST_CHECK(dream.drain(fixtureWaitMs));
        CTEST_COMPARE(f.latest(dream)["status"].toString(),"failed");CTEST_COMPARE(f.model->calls.load(),0);CTEST_CHECK(QFile::remove(directory+"/.dream.lock"));
#endif
        CTEST_CHECK(outside.open(QIODevice::ReadOnly));CTEST_COMPARE(outside.readAll(),bytes);outside.close();
        dream.request(f.owner);CTEST_CHECK(dream.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(dream)["status"].toString(),"completed");
    }
    void timeoutRetryAndWorkerCallbackLifecycle() {
        Fixture f;f.options.automatic=false;f.options.timeoutMs=40;std::atomic<bool> callbackGuard=false;
        f.model->action=[](const auto&,const CancellationToken& token)->a::ModelReply {while(!token.isCancelled())QThread::msleep(1);token.throwIfCancelled();return {};};
        a::MemoryDream* owner=nullptr;f.options.completed=[&](const auto&){try{owner->drain(1);}catch(const Error&){callbackGuard=true;}};
        a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);owner=&dream;dream.offer(f.snapshot());dream.request(f.owner);
        CTEST_CHECK(dream.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(dream)["status"].toString(),"timeout");CTEST_CHECK(callbackGuard);
        CTEST_COMPARE(f.latest(dream)["last_consolidated_ms"].toInteger(),0);f.model->action={};dream.request(f.owner);
        CTEST_CHECK(dream.drain(fixtureWaitMs));CTEST_COMPARE(f.latest(dream)["status"].toString(),"completed");
    }
    void queuedWorkCoalescesAndIdleContextsEvict() {
        Fixture f;f.options.automatic=false;f.options.maxSessions=1;f.options.maxRecords=2;
        std::atomic<bool> started=false,release=false;QStringList seen;
        f.model->action=[&](const a::ModelRequest& request,const CancellationToken& token)->a::ModelReply {
            started=true;while(!release&&!token.isCancelled())QThread::msleep(1);token.throwIfCancelled();
            seen.append(request.messages[1].id);return {"Done."};
        };
        a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);dream.offer(f.snapshot());dream.request(f.owner);CTEST_WAIT_VERIFY(started,3000);
        auto other=f.snapshot();other.sessionId=f.prior();CTEST_THROWS(Error,dream.offer(other));
        dream.offer(f.snapshot(2));dream.request(f.owner);dream.offer(f.snapshot(3));dream.request(f.owner);
        release=true;CTEST_CHECK(dream.drain(fixtureWaitMs));CTEST_COMPARE(seen,(QStringList{"a1","a3"}));CTEST_CHECK(dream.status(f.owner)["count"].toInt()<=2);
        CTEST_COMPARE(dream.offer(other)["status"].toString(),"retained");CTEST_CHECK(!dream.status(f.owner)["has_context"].toBool());
        CTEST_COMPARE(dream.request(f.owner)["status"].toString(),"no_context");dream.close();CTEST_THROWS(Error,dream.request(other.sessionId));
    }
    void recentTurnWindowAndUnicodeRemainBounded() {
        Fixture f;f.options.automatic=false;f.options.maxTurns=35;int turn=0;
        const auto path=QDir(f.work).filePath("note.txt");QFile file(path);CTEST_CHECK(file.open(QIODevice::WriteOnly));file.write("fixture");file.close();
        f.model->action=[&](const auto&,const auto&)->a::ModelReply {
            return {QString(1023,'x')+QString::fromUtf8("😀")+QString(500,'y'),{{QString::number(++turn),"Read",{{"path",path}}}}};
        };
        a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);dream.offer(f.snapshot());dream.request(f.owner);CTEST_CHECK(dream.drain(fixtureWaitMs));
        const auto result=f.latest(dream);CTEST_COMPARE(result["status"].toString(),"turn_limit");CTEST_COMPARE(result["tool_errors"].toInt(),0);
        CTEST_COMPARE(result["last_consolidated_ms"].toInteger(),0);CTEST_CHECK(result["turns_truncated"].toBool());
        const auto recent=result["recent_turns"].toArray();CTEST_CHECK(!recent.isEmpty()&&recent.size()<=30);
        CTEST_CHECK(QJsonDocument(recent).toJson(QJsonDocument::Compact).size()<=32768);
        for(const auto& value:recent){const auto item=value.toObject();CTEST_CHECK(item["text_truncated"].toBool());CTEST_COMPARE(item["text"].toString(),QString(1023,'x'));}
    }
    void historyMetadataFiltersCurrentWorkspaceAndTimestamp() {
        Fixture f;const auto previous=f.prior();f.store.acquire(f.owner)->append({{},a::MessageRole::User,"Current conversation"});
        const auto elsewhere=f.root.filePath("elsewhere");QDir().mkpath(elsewhere);f.store.create("fixture","system",elsewhere);
        const auto recent=f.history->recent(f.owner,f.work,0);CTEST_COMPARE(recent.size(),1);const auto item=recent.first().toObject();
        CTEST_COMPARE(item["session_id"].toString(),previous);CTEST_CHECK(item["modified_ms"].toInteger()>0);CTEST_CHECK(item["size_bytes"].toInteger()>0);
        CTEST_CHECK(f.history->recent(f.owner,f.work,item["modified_ms"].toInteger()).isEmpty());
        CTEST_THROWS(Error,f.history->recent(f.owner,elsewhere,0));CancellationToken cancelled;cancelled.cancel();
        CTEST_THROWS(Error,f.history->recent(f.owner,f.work,0,cancelled));
    }
    void completionRequiresAUniqueBoundedIndex() {
        Fixture f;f.options.automatic=false;const auto index=QDir(f.memory->directory(f.work)).filePath("MEMORY.md");
        const QByteArray duplicated="- [First](preferences.md)\n- [Second](./preferences.md)\n";
        QFile file(index);CTEST_CHECK(file.open(QIODevice::WriteOnly));file.write(duplicated);file.close();int turn=0;
        f.model->action=[&](const a::ModelRequest& request,const auto&)->a::ModelReply {
            if(++turn==1)return {"No changes."};
            if(turn==2){require(request.messages.last().text.contains("duplicate"),"Missing host index review");
                require(request.messages.last().text.contains("Use Read to read")&&request.messages.last().text.contains(index),"Missing concrete current-index read instruction");
                require(request.messages.last().text.contains("[\"preferences.md\"]"),"Missing observed normalized duplicate target");
                return {{},{{"read-index","Read",{{"path",index}}}}};}
            if(turn==3)return {{},{{"fix-index","Write",{{"path",index},{"content","- [Preferences](preferences.md)\n"}}}}};
            return {"Removed the duplicate link."};
        };
        a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);dream.offer(f.snapshot());dream.request(f.owner);CTEST_CHECK(dream.drain(fixtureWaitMs));
        const auto result=f.latest(dream);CTEST_COMPARE(result["status"].toString(),"completed");CTEST_COMPARE(result["validation_retries"].toInt(),1);
        CTEST_CHECK(result["completion_validated"].toBool());CTEST_COMPARE(result["turns"].toInt(),4);CTEST_CHECK(result["last_consolidated_ms"].toInteger()>0);
        CTEST_CHECK(file.open(QIODevice::ReadOnly));CTEST_COMPARE(file.readAll(),QByteArray("- [Preferences](preferences.md)\n"));file.close();

        Fixture stubborn;stubborn.options.automatic=false;stubborn.options.maxTurns=2;
        QFile longIndex(QDir(stubborn.memory->directory(stubborn.work)).filePath("MEMORY.md"));CTEST_CHECK(longIndex.open(QIODevice::WriteOnly));
        longIndex.write(QByteArray(25001,'x'));longIndex.close();
        a::MemoryDream bounded(stubborn.memory,stubborn.history,stubborn.model,stubborn.policy,stubborn.options);bounded.offer(stubborn.snapshot());bounded.request(stubborn.owner);
        CTEST_CHECK(bounded.drain(fixtureWaitMs));const auto failure=stubborn.latest(bounded);CTEST_COMPARE(failure["status"].toString(),"turn_limit");
        CTEST_COMPARE(failure["validation_retries"].toInt(),2);CTEST_CHECK(!failure["completion_validated"].toBool());CTEST_COMPARE(failure["last_consolidated_ms"].toInteger(),0);
    }
};
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    if(argc==3&&QString::fromLocal8Bit(argv[1])=="--hold-dream-lock") {
        QLockFile lock(QString::fromLocal8Bit(argv[2]));lock.setStaleLockTime(0);if(!lock.tryLock(0))return 2;
        std::cout<<"LOCKED"<<std::endl;return app.exec();
    }
    agent_ctest::CTestRunner runner;
    MemoryDreamTests tests;
    runner.run("automaticGatesAndSuccessfulTimeSurviveRestart", [&] { tests.automaticGatesAndSuccessfulTimeSurviveRestart(); });
    runner.run("sessionGateScanThrottleAndExplicitRequest", [&] { tests.sessionGateScanThrottleAndExplicitRequest(); });
    runner.run("sameParentPrefixHistorySearchAndNativeConsolidation", [&] { tests.sameParentPrefixHistorySearchAndNativeConsolidation(); });
    runner.run("cancellationAndOtherOwnerLockDoNotAdvanceSuccessfulTime", [&] { tests.cancellationAndOtherOwnerLockDoNotAdvanceSuccessfulTime(); });
    runner.run("capabilityFailureAndModelFailureAreRetryable", [&] { tests.capabilityFailureAndModelFailureAreRetryable(); });
    runner.run("liveProcessLockAndDeadHolderRecovery", [&] { tests.liveProcessLockAndDeadHolderRecovery(); });
    runner.run("corruptStateAndSymlinkFailWithoutRunningModel", [&] { tests.corruptStateAndSymlinkFailWithoutRunningModel(); });
    runner.run("timeoutRetryAndWorkerCallbackLifecycle", [&] { tests.timeoutRetryAndWorkerCallbackLifecycle(); });
    runner.run("queuedWorkCoalescesAndIdleContextsEvict", [&] { tests.queuedWorkCoalescesAndIdleContextsEvict(); });
    runner.run("recentTurnWindowAndUnicodeRemainBounded", [&] { tests.recentTurnWindowAndUnicodeRemainBounded(); });
    runner.run("historyMetadataFiltersCurrentWorkspaceAndTimestamp", [&] { tests.historyMetadataFiltersCurrentWorkspaceAndTimestamp(); });
    runner.run("completionRequiresAUniqueBoundedIndex", [&] { tests.completionRequiresAUniqueBoundedIndex(); });
    return runner.result();
}

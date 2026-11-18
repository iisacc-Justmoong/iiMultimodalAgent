#include <iiMultimodalAgent/agent/MemoryExtraction.h>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QThread>
#include <atomic>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
class Model final:public a::Model {
public:
    std::function<a::ModelReply(const a::ModelRequest&,const CancellationToken&)> action;
    std::atomic<int> calls=0;
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken& token,const std::function<bool(const QString&)>&)override {
        ++calls;return action?action(request,token):a::ModelReply{"No durable facts to store.",{},Usage{12,8}};
    }
};
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("extraction-test-XXXXXX")};QString work=root.filePath("work");
    std::shared_ptr<Model> model=std::make_shared<Model>();
    std::shared_ptr<a::ToolRegistry> tools=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>();
    std::shared_ptr<a::ProjectMemory> memory;
    a::MemoryExtractionOptions options;
    Fixture(){QDir().mkpath(work);memory=std::make_shared<a::ProjectMemory>(a::ProjectMemoryOptions{root.filePath("memory")});
        a::registerWorkspaceTools(*tools,work,{},QStringList{root.filePath("private")});memory->bindWorkspaceTools(*tools);options.enabled=true;}
    a::MemoryExtractionSnapshot snapshot(QString session="parent",int turns=1) {
        a::MemoryExtractionSnapshot value;value.sessionId=session;value.workspace=work;value.registry=tools->snapshot();
        value.request.model="fixture";value.request.contextId=session;value.request.systemPrompt="Host system stays identical";
        value.request.generation.maxTokens=700;value.request.generation.temperature=0.42;value.request.enableThinking=true;
        value.request.tools=tools->definitions();value.request.messages.append({"host",a::MessageRole::User,"Host context"});
        for(int i=0;i<turns;++i){value.messages.append({"u"+QString::number(i),a::MessageRole::User,"Use Korean in future reports."});
            value.messages.append({"a"+QString::number(i),a::MessageRole::Assistant,"Understood."});}
        value.request.messages.append(value.messages);value.context={session,{},work};return value;
    }
    QJsonObject latest(a::MemoryExtraction& extraction,QString id="parent") {
        const auto records=extraction.status(id)["records"].toArray();require(!records.isEmpty(),"No extraction record");return records.last().toObject();
    }
    QString note(QString name="preference.md"){return QDir(memory->directory(work)).filePath(name);}
};
}
class MemoryExtractionTests:public QObject {
public:
    void identicalParentPrefixNativeWriteAndNoForkTranscript() {
        Fixture f;const auto parent=f.snapshot();const auto path=f.note();std::atomic<int> round=0;
        f.model->action=[&](const a::ModelRequest& input,const CancellationToken&)->a::ModelReply {
            require(input.model==parent.request.model&&input.systemPrompt==parent.request.systemPrompt,"Changed model or system");
            require(input.enableThinking==parent.request.enableThinking&&input.generation.maxTokens==700&&input.generation.temperature==parent.request.generation.temperature,"Changed generation settings");
            require(input.messages.mid(0,parent.request.messages.size())==parent.request.messages,"Changed parent message prefix");
            require(input.tools.size()==parent.request.tools.size(),"Changed advertised tool prefix");
            require(input.messages[parent.request.messages.size()].metadata.contains("iilocal.memory_extraction"),"Missing extraction provenance");
            if(++round==1)return {{},{{"save","Write",{{"path",path},{"content","---\nname: Report language\ndescription: Use Korean for reports\ntype: feedback\n---\nWrite reports in Korean.\n"}}}},Usage{23,10}};
            require(!input.messages.last().isError,"Native memory write failed");return {"Saved the report language preference.",{},Usage{27,8}};
        };
        a::MemoryExtraction extraction(f.memory,f.model,f.policy,f.options);
        CTEST_COMPARE(extraction.offer(parent)["status"].toString(),"queued");CTEST_CHECK(extraction.drain(5000));
        const auto result=f.latest(extraction);CTEST_CHECK_MESSAGE(result["status"]=="completed",QJsonDocument(result).toJson().constData());CTEST_COMPARE(result["new_message_count"].toInt(),2);
        CTEST_COMPARE(result["written_paths"].toArray(),QJsonArray{path});CTEST_COMPARE(result["saved_topics"].toArray(),QJsonArray{path});
        CTEST_COMPARE(result["usage"].toObject()["generated_tokens"].toInt(),18);CTEST_CHECK(QFileInfo::exists(path));
        CTEST_COMPARE(extraction.status("parent")["cursor"].toString(),"a0");CTEST_COMPARE(extraction.request("parent")["status"].toString(),"up_to_date");
        CTEST_CHECK(!QFileInfo::exists(f.root.filePath("sessions")));
    }
    void latestPendingCoalescesAndRecalculatesCursor() {
        Fixture f;std::atomic<bool> entered=false,release=false;QList<int> counts;
        f.model->action=[&](const a::ModelRequest& input,const CancellationToken& token)->a::ModelReply {
            for(const auto& m:input.messages)if(m.metadata.contains("iilocal.memory_extraction"))counts.append(m.metadata["iilocal.memory_extraction"].toObject()["new_message_count"].toInt());
            if(!entered.exchange(true))while(!release){token.throwIfCancelled();QThread::msleep(1);}
            return {"No new durable fact.",{}};
        };
        a::MemoryExtraction extraction(f.memory,f.model,f.policy,f.options);extraction.offer(f.snapshot());CTEST_WAIT_VERIFY(entered,3000);
        const auto old=extraction.offer(f.snapshot("parent",2));const auto newest=extraction.offer(f.snapshot("parent",3));
        CTEST_CHECK(old["job_id"]!=newest["job_id"]);CTEST_CHECK(!extraction.drain(5));release=true;CTEST_CHECK(extraction.drain(5000));
        CTEST_COMPARE(f.model->calls.load(),2);CTEST_COMPARE(counts,QList<int>({2,4}));CTEST_COMPARE(extraction.status("parent")["cursor"].toString(),"a2");
        const auto records=extraction.status("parent")["records"].toArray();CTEST_CHECK(std::any_of(records.begin(),records.end(),[](auto v){return v.toObject()["status"]=="superseded";}));
    }
    void failedAttemptCanRetryAndMissingCursorAfterCompactionUsesVisibleRange() {
        Fixture f;f.model->action=[](const auto&,const auto&)->a::ModelReply{throw std::runtime_error("Transient runtime failure");};
        a::MemoryExtraction extraction(f.memory,f.model,f.policy,f.options);extraction.offer(f.snapshot());CTEST_CHECK(extraction.drain(5000));
        CTEST_COMPARE(f.latest(extraction)["status"].toString(),"failed");CTEST_CHECK(extraction.status("parent")["cursor"].toString().isEmpty());
        f.model->action={};extraction.request("parent");CTEST_CHECK(extraction.drain(5000));CTEST_COMPARE(f.latest(extraction)["status"].toString(),"completed");
        auto compacted=f.snapshot();compacted.messages={{"new-u",a::MessageRole::User,"A new durable preference"},{"new-a",a::MessageRole::Assistant,"Done"}};
        compacted.request.messages=compacted.messages;extraction.offer(compacted);CTEST_CHECK(extraction.drain(5000));
        CTEST_COMPARE(f.latest(extraction)["new_message_count"].toInt(),2);CTEST_COMPARE(extraction.status("parent")["cursor"].toString(),"new-a");
    }
    void attemptedDirectMemoryWriteSkipsEvenWhenItsToolResultFailed() {
        Fixture f;auto parent=f.snapshot();parent.messages.insert(1,{"attempt",a::MessageRole::Assistant,{},{{"write","Write",{{"path",f.note()},{"content","fact"}}}}});
        a::Message failure{"result",a::MessageRole::Tool,"Denied"};failure.toolCallId="write";failure.isError=true;parent.messages.insert(2,failure);
        parent.request.messages=parent.messages;a::MemoryExtraction extraction(f.memory,f.model,f.policy,f.options);
        CTEST_COMPARE(extraction.offer(parent)["status"].toString(),"skipped_direct_write");CTEST_COMPARE(f.model->calls.load(),0);
        CTEST_COMPARE(extraction.status("parent")["cursor"].toString(),"a0");
    }
    void capabilitiesDenyMcpBeforeValidationAndWritesOutsideOwnedMemory() {
        Fixture f;std::atomic<int> foreign=0;auto mcp=f.tools->get("Read");mcp.definition.name="RemoteRead";mcp.isMcp=true;
        mcp.validate=[&](const auto&,const auto&){++foreign;};mcp.execute=[&](const auto&,const auto&){++foreign;return a::ToolResult{};};f.tools->add(mcp);
        int round=0;f.model->action=[&](const a::ModelRequest& input,const CancellationToken&)->a::ModelReply {
            if(++round==1)return {{},{{"remote","RemoteRead",{{"path",f.note()}}},{"escape","Write",{{"path",QDir(f.work).filePath("changed.txt")},{"content","bad"}}}}};
            require(input.messages.last().isError&&input.messages[input.messages.size()-2].isError,"Capability escaped extraction boundary");return {"Nothing saved.",{}};
        };
        a::MemoryExtraction extraction(f.memory,f.model,f.policy,f.options);extraction.offer(f.snapshot());CTEST_CHECK(extraction.drain(5000));
        CTEST_COMPARE(foreign.load(),0);CTEST_CHECK(!QFileInfo::exists(QDir(f.work).filePath("changed.txt")));CTEST_CHECK(f.latest(extraction)["saved_topics"].toArray().isEmpty());
    }
    void cancellationIsIndependentFromParentAndCloseJoins() {
        Fixture f;std::atomic<bool> started=false,cancelled=false;
        f.model->action=[&](const auto&,const CancellationToken& token)->a::ModelReply {started=true;while(!token.isCancelled())QThread::msleep(1);cancelled=true;token.throwIfCancelled();return {};};
        a::MemoryExtraction extraction(f.memory,f.model,f.policy,f.options);auto parent=f.snapshot();const auto token=parent.context.cancellation;
        extraction.offer(parent);CTEST_WAIT_VERIFY(started,3000);token.cancel();CTEST_CHECK(!extraction.drain(5));CTEST_CHECK(!cancelled);
        extraction.cancel("parent");CTEST_CHECK(extraction.drain(3000));CTEST_CHECK(cancelled);CTEST_COMPARE(f.latest(extraction)["status"].toString(),"cancelled");
        extraction.close();CTEST_THROWS(Error,extraction.request("parent"));
    }
    void copiedReadObservationsAreFrozenAndCannotWriteBackToParentCache() {
        Fixture f;f.memory->bindWorkspaceTools(*f.tools);const auto path=f.note();QFile file(path);CTEST_CHECK(file.open(QIODevice::WriteOnly));file.write("old fact");file.close();
        a::ToolContext parent{"parent",{},f.work};a::ToolRunner runner(f.tools,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
        CTEST_CHECK(!runner.run({"read","Read",{{"path",path}}},parent).isError);
        int round=0;f.model->action=[&](const a::ModelRequest& request,const auto&)->a::ModelReply {
            if(++round==1)return {{},{{"edit","Edit",{{"path",path},{"old_string","old fact"},{"new_string","new fact"}}}}};
            require(!request.messages.last().isError,"Parent read observation was not inherited");return {"Done",{}};
        };
        a::MemoryExtraction extraction(f.memory,f.model,f.policy,f.options);extraction.offer(f.snapshot());CTEST_CHECK(extraction.drain(5000));
        CTEST_COMPARE(f.latest(extraction)["status"].toString(),"completed");
        // The child edit must not update the parent's original digest.
        CTEST_CHECK(runner.run({"parent-edit","Edit",{{"path",path},{"old_string","new fact"},{"new_string","bad update"}}},parent).isError);
    }
    void explicitDenyAskAndHookRewritesCannotExpandMemoryWrites() {
        for(const auto behavior:{a::PermissionBehavior::Deny,a::PermissionBehavior::Ask}) {
            Fixture f;f.policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"Write",behavior}});
            int round=0;f.model->action=[&](const a::ModelRequest& request,const auto&)->a::ModelReply {
                if(++round==1)return {{},{{"save","Write",{{"path",f.note()},{"content","fact"}}}}};
                require(request.messages.last().isError,"Explicit rule bypassed");return {"Denied",{}};
            };
            a::MemoryExtraction extraction(f.memory,f.model,f.policy,f.options);extraction.offer(f.snapshot());CTEST_CHECK(extraction.drain(5000));
            CTEST_CHECK(!QFileInfo::exists(f.note()));CTEST_COMPARE(f.latest(extraction)["tool_errors"].toInt(),1);
        }
        Fixture f;int round=0;f.model->action=[&](const a::ModelRequest& request,const auto&)->a::ModelReply {
            if(++round==1)return {{},{{"save","Write",{{"path",f.note()},{"content","fact"}}}}};
            require(request.messages.last().isError,"Hook escaped owned memory");return {"Denied",{}};
        };
        const auto outside=QDir(f.work).filePath("outside.md");a::Hook hook=[&](const a::HookInput& input,const auto&) {
            a::HookResult result;if(input.kind==a::HookKind::BeforeTool){auto args=input.call.arguments;args["path"]=outside;result.updatedArguments=args;
                result.permission=a::PermissionDecision{a::PermissionBehavior::Allow,"Host hook"};}return result;};
        a::MemoryExtraction extraction(f.memory,f.model,f.policy,f.options,{hook});extraction.offer(f.snapshot());CTEST_CHECK(extraction.drain(5000));CTEST_CHECK(!QFileInfo::exists(outside));
    }
    void boundedTurnsTimeoutCadenceAndContextRetention() {
        Fixture f;f.options.everyTurns=2;f.options.maxSessions=1;f.options.maxRecords=1;
        a::MemoryExtraction extraction(f.memory,f.model,f.policy,f.options);
        CTEST_COMPARE(extraction.offer(f.snapshot())["status"].toString(),"throttled");extraction.offer(f.snapshot("parent",2));CTEST_CHECK(extraction.drain(5000));
        CTEST_COMPARE(f.model->calls.load(),1);extraction.offer(f.snapshot("another"));CTEST_COMPARE(extraction.request("parent")["status"].toString(),"no_context");
        extraction.request("another");CTEST_CHECK(extraction.drain(5000));CTEST_CHECK(extraction.status("parent")["records"].toArray().isEmpty());
        Fixture slow;slow.options.timeoutMs=15;slow.model->action=[](const auto&,const CancellationToken& token)->a::ModelReply {while(!token.isCancelled())QThread::msleep(1);token.throwIfCancelled();return {};};
        a::MemoryExtraction timed(slow.memory,slow.model,slow.policy,slow.options);timed.offer(slow.snapshot());CTEST_CHECK(timed.drain(3000));CTEST_COMPARE(slow.latest(timed)["status"].toString(),"timeout");
        Fixture loop;loop.model->action=[&](const auto&,const auto&)->a::ModelReply{return {{},{{"glob"+QString::number(loop.model->calls),"Glob",{{"pattern","*.md"},{"path",loop.memory->directory(loop.work)}}}}};};
        a::MemoryExtraction limited(loop.memory,loop.model,loop.policy,loop.options);limited.offer(loop.snapshot());CTEST_CHECK(limited.drain(5000));
        CTEST_COMPARE(loop.latest(limited)["status"].toString(),"turn_limit");CTEST_COMPARE(loop.model->calls.load(),5);CTEST_CHECK(limited.status("parent")["cursor"].toString().isEmpty());
    }
    void nativeReadOnlyShellDoesNotExecuteStartupFilesOrUnsafeSyntax() {
#ifndef Q_OS_UNIX
        agent_ctest::skip("Native read-only Bash requires Unix");
#else
        Fixture f;const auto marker=QDir(f.work).filePath("changed.txt"),startup=QDir(f.work).filePath("startup.sh");
        QFile script(startup);CTEST_CHECK(script.open(QIODevice::WriteOnly));script.write(("echo unsafe > '"+marker+"'\n").toUtf8());script.close();
        const auto old=qgetenv("BASH_ENV");const auto had=qEnvironmentVariableIsSet("BASH_ENV");qputenv("BASH_ENV",startup.toUtf8());
        struct Restore {QByteArray old;bool had;~Restore(){if(had)qputenv("BASH_ENV",old);else qunsetenv("BASH_ENV");}} restore{old,had};
        a::ToolContext context{"child",{},f.work};context.readOnlyShell=true;
        a::ToolRunner runner(f.tools,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
        for(int repetition=0;repetition<2;++repetition)
        for(const auto& command:QStringList{"pwd","printf '%s\\n' hello | wc -c","cat startup.sh","head -n 1 startup.sh","head -n1 startup.sh","tail -c1 startup.sh","printf '%04d' 7","printf '%s' hello 1>&1","ls -l startup.sh"}) {
            const auto result=runner.run({"safe","Bash",{{"command",command}}},context);
            CTEST_CHECK_MESSAGE(!result.isError,qPrintable(command+": "+result.text+QString::fromUtf8(QJsonDocument(result.data).toJson())));
        }
        CTEST_CHECK(!QFileInfo::exists(marker));
        for(int repetition=0;repetition<2;++repetition)
        for(const auto& command:QStringList{"echo bad > changed.txt","cat $(touch changed.txt)","printf -v PATH .; cat startup.sh","printf '%10n' PATH; cat startup.sh",
            "head -n 99999999 startup.sh","tail -c99999999 startup.sh","printf '%Q' hello",
            "cat /etc/passwd","ls -R .","find . -delete","git status","cat startup.sh &","PATH=. cat startup.sh"})
            CTEST_CHECK_MESSAGE(runner.run({"unsafe","Bash",{{"command",command}}},context).isError,qPrintable(command));
        CTEST_CHECK(!QFileInfo::exists(marker));
#endif
    }
    void aFullChildReadCacheCannotEvictParentObservations() {
        Fixture f;(void)f.memory->directory(f.work);const auto original=QDir(f.work).filePath("original.txt");QFile file(original);CTEST_CHECK(file.open(QIODevice::WriteOnly));file.write("unchanged");file.close();
        for(int i=0;i<256;++i){QFile input(QDir(f.work).filePath(QString::number(i)+".txt"));CTEST_CHECK(input.open(QIODevice::WriteOnly));input.write("observation");}
        a::ToolContext parent{"parent",{},f.work};a::ToolRunner runner(f.tools,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
        CTEST_CHECK(!runner.run({"parent-read","Read",{{"path",original}}},parent).isError);
        int offset=0;f.model->action=[&](const auto&,const auto&)->a::ModelReply {
            if(offset==256)return {"No durable facts.",{}};QList<a::ToolCall> calls;
            for(int n=0;n<64;++n,++offset)calls.append({QString::number(offset),"Read",{{"path",QDir(f.work).filePath(QString::number(offset)+".txt")}}});
            return {{},calls};
        };
        a::MemoryExtraction extraction(f.memory,f.model,f.policy,f.options);extraction.offer(f.snapshot());CTEST_CHECK(extraction.drain(10000));
        const auto result=f.latest(extraction);CTEST_CHECK_MESSAGE(result["status"]=="completed",QJsonDocument(result).toJson().constData());CTEST_COMPARE(result["tool_errors"].toInt(),0);
        CTEST_CHECK(!runner.run({"parent-edit","Edit",{{"path",original},{"old_string","unchanged"},{"new_string","parent update"}}},parent).isError);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) { std::cerr << "Expected a case name or --offline" << std::endl; return 1; }
    const std::string_view selection = argc == 2 ? argv[1] : "";
    const bool offline = selection == "--offline";
    agent_ctest::CTestRunner runner(offline ? std::string_view{} : selection);
    MemoryExtractionTests tests;
    runner.run("identicalParentPrefixNativeWriteAndNoForkTranscript", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.identicalParentPrefixNativeWriteAndNoForkTranscript(); });
    runner.run("latestPendingCoalescesAndRecalculatesCursor", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.latestPendingCoalescesAndRecalculatesCursor(); });
    runner.run("failedAttemptCanRetryAndMissingCursorAfterCompactionUsesVisibleRange", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.failedAttemptCanRetryAndMissingCursorAfterCompactionUsesVisibleRange(); });
    runner.run("attemptedDirectMemoryWriteSkipsEvenWhenItsToolResultFailed", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.attemptedDirectMemoryWriteSkipsEvenWhenItsToolResultFailed(); });
    runner.run("capabilitiesDenyMcpBeforeValidationAndWritesOutsideOwnedMemory", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.capabilitiesDenyMcpBeforeValidationAndWritesOutsideOwnedMemory(); });
    runner.run("cancellationIsIndependentFromParentAndCloseJoins", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.cancellationIsIndependentFromParentAndCloseJoins(); });
    runner.run("copiedReadObservationsAreFrozenAndCannotWriteBackToParentCache", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.copiedReadObservationsAreFrozenAndCannotWriteBackToParentCache(); });
    runner.run("explicitDenyAskAndHookRewritesCannotExpandMemoryWrites", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.explicitDenyAskAndHookRewritesCannotExpandMemoryWrites(); });
    runner.run("boundedTurnsTimeoutCadenceAndContextRetention", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.boundedTurnsTimeoutCadenceAndContextRetention(); });
    if (!offline)
        runner.run("nativeReadOnlyShellDoesNotExecuteStartupFilesOrUnsafeSyntax", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.nativeReadOnlyShellDoesNotExecuteStartupFilesOrUnsafeSyntax(); });
    runner.run("aFullChildReadCacheCannotEvictParentObservations", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.aFullChildReadCacheCannotEvictParentObservations(); });
    return runner.result();
}

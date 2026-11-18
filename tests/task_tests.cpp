#include "native_link.h"
#include "iiMultimodalAgent/agent/TaskStore.h"
#include "iiMultimodalAgent/agent/Engine.h"
#include <iiFileProviderDevice.h>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QtCore/QFile>
#include <QtCore/QLockFile>
#include <QtCore/QDir>
#include <QtCore/QProcess>
#include <QtCore/QJsonDocument>
#include <iostream>
#include <atomic>
#include <future>
#include <thread>
#include <set>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a = iiMultimodalAgent::agent;
using namespace std::chrono_literals;
namespace {
QJsonObject create(a::TaskStore& s, const QString& board, const QString& subject = "Build application") {
    return s.execute(board, "TaskCreate", {{"subject", subject}, {"description", "Build and verify the application"}}).data;
}
template<class F> void fails(F fn, ErrorCode code = ErrorCode::InvalidArgument) {
    try { fn(); agent_ctest::fail(__FILE__, __LINE__, "Expected task error"); } catch (const Error& e) { CTEST_COMPARE(e.code(), code); }
}
class Model final : public a::Model {
public:
    QList<a::ModelRequest> requests;
    QList<a::ModelReply> replies;
    a::ModelReply generate(const a::ModelRequest& request, const CancellationToken&, const TextCallback&) override {
        requests.append(request); if (replies.isEmpty()) return {"done", {}}; return replies.takeFirst();
    }
};
}
class TaskTests : public QObject {
public:
#ifdef Q_OS_WIN
    void nativeLockSharingWaitsWithoutLosingTaskState() {
        iiFileProvider::TemporaryDirectory root;
        a::TaskStore store(root.filePath("tasks")); create(store,"one");
        const auto path=root.filePath("tasks/one/board.lock").toStdWString();
        // Delete on close avoids a second close/delete race with QLockFile's
        // probes while guaranteeing the native handle denies lock acquisition.
        const auto handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,
            0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        CTEST_CHECK(handle!=INVALID_HANDLE_VALUE);
        std::atomic_bool released=false;
        std::jthread release([&,handle] { std::this_thread::sleep_for(100ms); released=true; CloseHandle(handle); });
        try { create(store,"one","After deletion"); }
        catch(const Error& error) { agent_ctest::fail(__FILE__, __LINE__, error.what()); }
        CTEST_CHECK(released.load());
        release.join();
        const auto board=store.snapshot("one");
        CTEST_COMPARE(board["revision"].toInt(),2); CTEST_COMPARE(board["tasks"].toArray().size(),2);
    }
    void nativeLockSharingCanBeCancelled() {
        iiFileProvider::TemporaryDirectory root;
        a::TaskStore store(root.filePath("tasks")); create(store,"one");
        const auto path=root.filePath("tasks/one/board.lock").toStdWString();
        const auto handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,
            0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        CTEST_CHECK(handle!=INVALID_HANDLE_VALUE);
        CancellationToken token;
        std::jthread cancel([&] { std::this_thread::sleep_for(50ms); token.cancel(); });
        bool cancelled=false; QString observed="Lock acquisition completed before cancellation";
        try { store.snapshot("one",token); }
        catch(const Error& error) { cancelled=error.code()==ErrorCode::Cancelled; observed=QString::fromUtf8(error.what()); }
        cancel.join(); CloseHandle(handle); CTEST_CHECK_MESSAGE(cancelled,qPrintable(observed));
        CTEST_COMPARE(store.snapshot("one")["revision"].toInt(),1);
    }
    void atomicTaskPublicationWaitsForAReader() {
        iiFileProvider::TemporaryDirectory root;
        a::TaskStore store(root.filePath("tasks")); create(store,"one");
        const auto path=root.filePath("tasks/one/board.json").toStdWString();
        const auto handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        CTEST_CHECK(handle!=INVALID_HANDLE_VALUE);
        std::jthread release([handle] { std::this_thread::sleep_for(100ms); CloseHandle(handle); });
        try { create(store,"one","After reader"); }
        catch(const Error& error) { agent_ctest::fail(__FILE__, __LINE__, error.what()); }
        release.join(); CTEST_COMPARE(store.snapshot("one")["revision"].toInt(),2);
    }
    void persistentTaskSharingFailurePreservesTheOriginalBoard() {
        iiFileProvider::TemporaryDirectory root; a::TaskStoreOptions options; options.lockTimeoutMs=100;
        a::TaskStore store(root.filePath("tasks"),options); create(store,"one");
        const auto before=store.snapshot("one");
        const auto path=root.filePath("tasks/one/board.json").toStdWString();
        const auto handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        CTEST_CHECK(handle!=INVALID_HANDLE_VALUE);
        bool rejected=false;
        try { create(store,"one","Must not be published"); }
        catch(const Error& error) { rejected=error.code()==ErrorCode::StorageFailure; }
        CloseHandle(handle); CTEST_CHECK(rejected); CTEST_COMPARE(store.snapshot("one"),before);
    }
    void taskPublicationCanBeCancelledWithoutChangingTheBoard() {
        iiFileProvider::TemporaryDirectory root;
        a::TaskStore store(root.filePath("tasks")); create(store,"one");
        const auto before=store.snapshot("one");
        const auto path=root.filePath("tasks/one/board.json").toStdWString();
        const auto handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        CTEST_CHECK(handle!=INVALID_HANDLE_VALUE);
        CancellationToken token;
        std::jthread cancel([&] { std::this_thread::sleep_for(50ms); token.cancel(); });
        bool cancelled=false;
        try { store.execute("one","TaskCreate",{{"subject","Cancelled"},{"description","Must not publish"}},token); }
        catch(const Error& error) { cancelled=error.code()==ErrorCode::Cancelled; }
        cancel.join(); CloseHandle(handle); CTEST_CHECK(cancelled); CTEST_COMPARE(store.snapshot("one"),before);
    }
#endif
    void retiredNamespaceCannotBeRecreatedByStaleOwners() {
        iiFileProvider::TemporaryDirectory root;a::TaskStore first(root.filePath("tasks")),stale(root.filePath("tasks"));
        create(first,"team");first.retire("team");
        CTEST_CHECK(!QFileInfo::exists(root.filePath("tasks/team/board.json")));
        fails([&]{stale.snapshot("team");},ErrorCode::NotFound);
        fails([&]{create(stale,"team");},ErrorCode::NotFound);
        first.retire("team");CTEST_CHECK(stale.snapshot("new-team")["tasks"].toArray().isEmpty());
        CTEST_CHECK(QFile::remove(root.filePath("tasks/team/retired.json")));
        CTEST_CHECK(createNativeTestLink(root.filePath("absent"),root.filePath("tasks/team/retired.json")));
        fails([&]{stale.snapshot("team");},ErrorCode::StorageFailure);
    }
    void verificationCanReadTheBoardBeforePublication() {
        iiFileProvider::TemporaryDirectory root; a::TaskStoreOptions options; options.lockTimeoutMs=150;
        a::TaskStore store(root.filePath("tasks"),options);create(store,"s","EXISTING");
        bool inspected=false;
        try {
            const auto output=store.execute("s","TaskCreate",{{"subject","PENDING"},{"description","verify before commit"}}, {},
                [&](const a::TaskChange&,const CancellationToken& token) {
                    const auto list=store.execute("s","TaskList",{},token).data;
                    inspected=list["total"]==1&&list["tasks"].toArray().first().toObject()["subject"]=="EXISTING";
                });
            CTEST_COMPARE(output.data["revision"].toInt(),2);
        } catch(const Error& error) {agent_ctest::fail(__FILE__, __LINE__, error.what());}
        CTEST_CHECK(inspected);CTEST_COMPARE(store.snapshot("s")["tasks"].toArray().size(),2);
    }
    void verificationSideEffectsCannotBeOverwrittenByAStaleCommit() {
        iiFileProvider::TemporaryDirectory root;a::TaskStoreOptions options;options.lockTimeoutMs=150;
        a::TaskStore store(root.filePath("tasks"),options);create(store,"s","EXISTING");int calls=0;
        fails([&] {store.execute("s","TaskCreate",{{"subject","STALE"},{"description","must not overwrite"}}, {},
            [&](const a::TaskChange&,const CancellationToken& token) {
                ++calls;store.execute("s","TaskUpdate",{{"taskId","1"},{"subject","VERIFIED_UPDATE"}},token);
            });},ErrorCode::AlreadyExists);
        CTEST_COMPARE(calls,1);const auto board=store.snapshot("s");CTEST_COMPARE(board["revision"].toInt(),2);
        CTEST_COMPARE(board["tasks"].toArray().size(),1);CTEST_COMPARE(board["tasks"].toArray().first().toObject()["subject"],"VERIFIED_UPDATE");
    }
    void persistentLifecycleAndDependencies() {
        iiFileProvider::TemporaryDirectory root; a::TaskStore store(root.filePath("tasks"));
        const auto one = create(store, "session"); CTEST_COMPARE(one["task"].toObject()["id"], "1");
        CTEST_COMPARE(one["revision"].toInt(), 1);
        create(store, "session", "Package application");
        store.execute("session", "TaskUpdate", {{"taskId", "2"}, {"addBlockedBy", QJsonArray{"1"}},
            {"metadata", QJsonObject{{"priority", 3}, {"nested", QJsonObject{{"value", true}}}}}});
        auto board = store.snapshot("session");
        CTEST_COMPARE(board["tasks"].toArray()[0].toObject()["blocks"].toArray(), QJsonArray{"2"});
        fails([&] { store.execute("session", "TaskUpdate", {{"taskId", "1"}, {"addBlockedBy", QJsonArray{"2"}}}); });
        CTEST_COMPARE(store.snapshot("session"), board);
        const auto blocked = store.execute("session", "TaskClaim", {{"taskId", "2"}, {"owner", "builder"}});
        CTEST_CHECK(!blocked.data["success"].toBool()); CTEST_COMPARE(blocked.data["reason"], "blocked");
        store.execute("session", "TaskUpdate", {{"taskId", "1"}, {"status", "completed"}});
        CTEST_CHECK(store.execute("session", "TaskClaim", {{"taskId", "2"}, {"owner", "builder"}}).data["success"].toBool());
        auto listed = store.execute("session", "TaskList", {{"limit", 1}}).data;
        CTEST_COMPARE(listed["tasks"].toArray().size(), 1); CTEST_COMPARE(listed["nextOffset"].toInt(), 1);
        auto task = store.execute("session", "TaskGet", {{"taskId", "2"}}).data["task"].toObject();
        CTEST_COMPARE(task["status"], "in_progress"); CTEST_COMPARE(task["blockedBy"].toArray(), QJsonArray{"1"});
        CTEST_CHECK(task["unresolvedBlockedBy"].toArray().isEmpty());
        store.execute("session", "TaskUpdate", {{"taskId", "2"}, {"metadata", QJsonObject{{"priority", QJsonValue::Null}}}});
        task = store.execute("session", "TaskGet", {{"taskId", "2"}}).data["task"].toObject();
        CTEST_CHECK(!task["metadata"].toObject().contains("priority")); CTEST_CHECK(task["metadata"].toObject().contains("nested"));
        store.execute("session", "TaskUpdate", {{"taskId", "1"}, {"status", "deleted"}});
        CTEST_CHECK(store.execute("session", "TaskGet", {{"taskId", "2"}}).data["task"].toObject()["blockedBy"].toArray().isEmpty());
        a::TaskStore reopened(root.filePath("tasks")); CTEST_COMPARE(reopened.snapshot("session"), store.snapshot("session"));
        CTEST_COMPARE(create(reopened, "session")["task"].toObject()["id"], "3");
        CTEST_CHECK(reopened.snapshot("another")["tasks"].toArray().isEmpty());
    }
    void validationTransactionsAndHooks() {
        iiFileProvider::TemporaryDirectory root; a::TaskStore store(root.filePath("tasks")); create(store, "a");
        auto before = store.snapshot("a");
        for (const auto& invalid : QList<QJsonObject>{
            {{"taskId", "1"}, {"status", "bogus"}},
            {{"taskId", "1"}, {"addBlocks", QJsonArray{"missing"}}},
            {{"taskId", "1"}, {"addBlocks", QJsonArray{"1"}}},
            {{"taskId", "1"}, {"owner", 3}},
            {{"taskId", "1"}, {"status", "deleted"}, {"subject", "Ambiguous deletion"}},
            {{"taskId", "1"}, {"expectedRevision", 0}}
        }) {
            CTEST_THROWS(Error, store.execute("a", "TaskUpdate", invalid));
            CTEST_COMPARE(store.snapshot("a"), before);
        }
        fails([&] { create(store, "../escape"); });
        fails([&] { store.execute("a", "TaskList", {{"limit", 101}}); });
        fails([&] { store.execute("a", "TaskCreate", {{"subject", ""}, {"description", "x"}}); });
        int invoked = 0; bool statusMatched = false;
        a::TaskCommitCallback veto = [&](const a::TaskChange& change, const CancellationToken&) {
            ++invoked; statusMatched = change.after["status"] == "completed";
            throw Error(ErrorCode::InvalidArgument, "Verification is missing");
        };
        fails([&] { store.execute("a", "TaskUpdate", {{"taskId", "1"}, {"status", "completed"}}, {}, veto); });
        CTEST_COMPARE(invoked, 1); CTEST_CHECK(statusMatched); CTEST_COMPARE(store.snapshot("a"), before);
        store.execute("a", "TaskUpdate", {{"taskId", "1"}, {"subject", "Build application"}});
        CTEST_COMPARE(store.snapshot("a"), before); // A no-op preserves revision and persisted content.
        CancellationToken cancelled; cancelled.cancel();
        fails([&] { store.execute("a", "TaskCreate", {{"subject", "x"}, {"description", "x"}}, cancelled); }, ErrorCode::Cancelled);
        CTEST_COMPARE(store.snapshot("a"), before);
    }
    void concurrentCreationAndAtomicClaim() {
        iiFileProvider::TemporaryDirectory root;
        a::TaskStoreOptions options; options.lockTimeoutMs = 15000;
        auto store = std::make_shared<a::TaskStore>(root.filePath("tasks"), options);
        std::vector<std::future<QString>> creates;
        for (int n = 0; n < 16; ++n) creates.emplace_back(std::async(std::launch::async, [&, n] {
            a::TaskStore independent(root.filePath("tasks"), options);
            return create(independent, "shared", QString::number(n))["task"].toObject()["id"].toString();
        }));
        std::set<QString> ids; QStringList failures;
        for (auto& result : creates) {
            try { ids.insert(result.get()); }
            catch (const std::exception& error) { failures.append(QString::fromUtf8(error.what())); }
        }
        CTEST_CHECK_MESSAGE(failures.isEmpty(),qPrintable(failures.join('\n')));
        CTEST_COMPARE(ids.size(), size_t(16)); CTEST_COMPARE(store->snapshot("shared")["revision"].toInt(), 16);
        std::vector<std::future<bool>> claims;
        for (int n = 0; n < 12; ++n) claims.emplace_back(std::async(std::launch::async, [&, n] {
            return store->execute("shared", "TaskClaim", {{"taskId", "1"}, {"owner", "agent-" + QString::number(n)}}).data["success"].toBool();
        }));
        int won = 0;
        for (auto& result : claims) {
            try { if (result.get()) ++won; }
            catch (const std::exception& error) { failures.append(QString::fromUtf8(error.what())); }
        }
        CTEST_CHECK_MESSAGE(failures.isEmpty(),qPrintable(failures.join('\n'))); CTEST_COMPARE(won, 1);
        const auto owner = store->execute("shared", "TaskGet", {{"taskId", "1"}}).data["task"].toObject()["owner"].toString();
        const auto busy = store->execute("shared", "TaskClaim", {{"taskId", "2"}, {"owner", owner}, {"checkOwnerBusy", true}}).data;
        CTEST_COMPARE(busy["reason"], "owner_busy"); CTEST_CHECK(!busy["success"].toBool());
    }
    void unicodeOwnerSurvivesPersistentReload() {
        iiFileProvider::TemporaryDirectory root; a::TaskStore store(root.filePath("tasks")); create(store, "a");
        const auto owner = QString::fromUtf8("\xf0\x9f\x9b\xa0").repeated(128);
        store.execute("a", "TaskClaim", {{"taskId", "1"}, {"owner", owner}});
        a::TaskStore reopened(root.filePath("tasks"));
        CTEST_COMPARE(reopened.execute("a", "TaskGet", {{"taskId", "1"}}).data["task"].toObject()["owner"].toString(), owner);
        const auto before = reopened.snapshot("a");
        fails([&] { reopened.execute("a", "TaskUpdate", {{"taskId", "1"}, {"owner", owner + "x"}}); });
        CTEST_COMPARE(reopened.snapshot("a"), before);
    }
    void todosIsolationAndCorruptStorage() {
        iiFileProvider::TemporaryDirectory root; a::TaskStore store(root.filePath("tasks"));
        const QJsonArray todos{QJsonObject{{"content", "Verify build"}, {"activeForm", "Verifying build"}, {"status", "in_progress"}}};
        CTEST_COMPARE(store.execute("a", "TodoWrite", {{"todos", todos}}).data["newTodos"].toArray(), todos);
        CTEST_COMPARE(store.execute("a", "TodoRead", {}).data["todos"].toArray(), todos);
        CTEST_CHECK(store.execute("b", "TodoRead", {}).data["todos"].toArray().isEmpty());
        auto done = todos[0].toObject(); done["status"] = "completed";
        store.execute("a", "TodoWrite", {{"todos", QJsonArray{done}}});
        CTEST_COMPARE(store.execute("a", "TodoRead", {}).data["todos"].toArray(), QJsonArray{done});
        // Completed evidence remains visible until an explicit empty replacement.
        fails([&] { store.execute("a", "TodoWrite", {{"todos", QJsonArray{QJsonObject{{"content", ""}}}}}); });
        QFile file(root.filePath("tasks/a/board.json")); CTEST_CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{broken"); file.close();
        fails([&] { create(store, "a"); }, ErrorCode::ProtocolError);
        CTEST_CHECK(file.open(QIODevice::ReadOnly)); CTEST_COMPARE(file.readAll(), QByteArray("{broken"));
    }
    void processesCoordinateAndWaitingCanBeCancelled() {
        iiFileProvider::TemporaryDirectory root; a::TaskStore store(root.filePath("tasks"));
        std::vector<std::unique_ptr<QProcess>> children;
        for (int n = 0; n < 8; ++n) {
            auto process = std::make_unique<QProcess>();
            process->start(QCoreApplication::applicationFilePath(), {"--task-worker", root.filePath("tasks"), "shared", "create", QString::number(n)});
            CTEST_CHECK(process->waitForStarted(5000)); children.push_back(std::move(process));
        }
        for (auto& child : children) {
            CTEST_CHECK(child->state()==QProcess::NotRunning || child->waitForFinished(15000));
            const auto diagnostics=child->readAllStandardError();
            CTEST_CHECK_MESSAGE(child->exitStatus()==QProcess::NormalExit && child->exitCode()==0,diagnostics.constData());
            QJsonParseError error;
            const auto output=child->readAllStandardOutput();
            const auto document=QJsonDocument::fromJson(output,&error);
            CTEST_CHECK_MESSAGE(error.error==QJsonParseError::NoError && document.isObject(),output.constData());
            CTEST_CHECK(document.object()["task"].isObject());
        }
        CTEST_COMPARE(store.snapshot("shared")["tasks"].toArray().size(), 8);
        for (int n = 0; n < 8; ++n)
            children[n]->start(QCoreApplication::applicationFilePath(), {"--task-worker", root.filePath("tasks"), "shared", "claim", QString::number(n)});
        int wins = 0;
        for (auto& child : children) {
            CTEST_CHECK(child->state()==QProcess::NotRunning || child->waitForFinished(15000));
            const auto diagnostics=child->readAllStandardError();
            CTEST_CHECK_MESSAGE(child->exitStatus()==QProcess::NormalExit && child->exitCode()==0,diagnostics.constData());
            QJsonParseError error;
            const auto output=child->readAllStandardOutput();
            const auto document=QJsonDocument::fromJson(output,&error);
            CTEST_CHECK_MESSAGE(error.error==QJsonParseError::NoError && document.isObject(),output.constData());
            CTEST_CHECK(document.object()["success"].isBool());
            if (document.object()["success"].toBool()) ++wins;
        }
        CTEST_COMPARE(wins, 1);
        QLockFile lock(root.filePath("tasks/shared/board.lock")); lock.setStaleLockTime(0); CTEST_CHECK(lock.tryLock(0));
        CancellationToken token;
        auto waiting = std::async(std::launch::async, [&] {
            try { store.snapshot("shared", token); return ErrorCode::None; } catch (const Error& e) { return e.code(); }
        });
        std::this_thread::sleep_for(50ms); token.cancel();
        CTEST_CHECK(waiting.wait_for(1s) == std::future_status::ready); CTEST_COMPARE(waiting.get(), ErrorCode::Cancelled);
        lock.unlock();
#ifdef Q_OS_UNIX
        CTEST_CHECK(QFile::rename(root.filePath("tasks/shared/board.json"), root.filePath("saved.json")));
        CTEST_CHECK(createNativeTestLink(root.filePath("saved.json"), root.filePath("tasks/shared/board.json")));
        fails([&] { store.snapshot("shared"); }, ErrorCode::StorageFailure);
#endif
    }
    void modelToolsResumeContextPolicyAndFork() {
        iiFileProvider::TemporaryDirectory root; a::EngineOptions options{.decision={.enabled=false}}; options.sessionsDirectory = root.filePath("sessions");
        options.taskToolsEnabled = true; options.projectContext.enabled = false; options.compaction.automatic = false;
        int createdHooks = 0; bool rejectCompletion = false;
        options.hooks.append([&](const a::HookInput& input, const CancellationToken&) {
            if (input.kind == a::HookKind::TaskCreated) ++createdHooks;
            return a::HookResult{input.kind == a::HookKind::TaskCompleted && rejectCompletion, {}};
        });
        auto model = std::make_shared<Model>(); auto registry = std::make_shared<a::ToolRegistry>();
        model->replies = {{{}, {{"find", "ToolSearch", {{"query", "select:TaskCreate"}}}}},
            {{}, {{"create", "TaskCreate", {{"subject", "Verify"}, {"description", "Run tests"}}}}}, {"done", {}}};
        QString id;
        {
            a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Plan), options);
            id = engine.createSession("fixture", root.path()).id;
            CTEST_COMPARE(engine.run({id, "Create a verification task"}).result.get().status, a::RunStatus::Completed);
            CTEST_COMPARE(engine.runTaskTool(id, "TaskList").data["tasks"].toArray().size(), 1);
            CTEST_COMPARE(createdHooks, 1); rejectCompletion = true;
            CTEST_CHECK(engine.runTaskTool(id, "TaskUpdate", {{"taskId", "1"}, {"status", "completed"}}).isError);
            CTEST_COMPARE(engine.runTaskTool(id, "TaskGet", {{"taskId", "1"}}).data["task"].toObject()["status"], "pending");
            CTEST_COMPARE(model->requests.size(), 3);
            CTEST_CHECK(std::any_of(model->requests[2].messages.begin(), model->requests[2].messages.end(),
                [](const auto& m) { return m.metadata.contains("iilocal.task_state"); }));
            const auto fork = engine.forkSession(id); CTEST_CHECK(engine.runTaskTool(fork.id, "TaskList").data["tasks"].toArray().isEmpty());
            CTEST_COMPARE(engine.run({fork.id, "Continue with this conversation's task list"}).result.get().status, a::RunStatus::Completed);
            const auto& messages = model->requests.last().messages;
            CTEST_CHECK(std::any_of(messages.begin(), messages.end(), [](const auto& m) {
                return m.metadata.contains("iilocal.task_state") && m.text.contains("\"taskCount\":0");
            })); // A fork must not mistake copied parent tool results for its live task state.
        }
        a::Engine resumed(model, registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Default,
            QList<a::PermissionRule>{{"TaskUpdate", a::PermissionBehavior::Deny}}), options);
        CTEST_CHECK(resumed.runTaskTool(id, "TaskUpdate", {{"taskId", "1"}, {"status", "completed"}}).isError);
        CTEST_COMPARE(resumed.runTaskTool(id, "TaskGet", {{"taskId", "1"}}).data["task"].toObject()["status"], "pending");
        fails([&] { resumed.runTaskTool("00000000-0000-0000-0000-000000000000", "TaskList"); }, ErrorCode::NotFound);
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); const auto args = app.arguments();
    if (args.size() == 6 && args[1] == "--task-worker") {
        try {
            a::TaskStore store(args[2]);
            auto value = args[4] == "create" ? create(store, args[3], args[5])
                : store.execute(args[3], "TaskClaim", {{"taskId", "1"}, {"owner", args[5]}}).data;
            std::cout << QJsonDocument(value).toJson(QJsonDocument::Compact).constData() << std::endl;
            if (!std::cout) { std::cerr << "Cannot flush task worker response" << std::endl; return 2; }
            return 0;
        } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    }
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    TaskTests tests;
#ifdef Q_OS_WIN
    runner.run("nativeLockSharingWaitsWithoutLosingTaskState", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.nativeLockSharingWaitsWithoutLosingTaskState(); });
    runner.run("nativeLockSharingCanBeCancelled", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.nativeLockSharingCanBeCancelled(); });
    runner.run("atomicTaskPublicationWaitsForAReader", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.atomicTaskPublicationWaitsForAReader(); });
    runner.run("persistentTaskSharingFailurePreservesTheOriginalBoard", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.persistentTaskSharingFailurePreservesTheOriginalBoard(); });
    runner.run("taskPublicationCanBeCancelledWithoutChangingTheBoard", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.taskPublicationCanBeCancelledWithoutChangingTheBoard(); });
#endif
    runner.run("retiredNamespaceCannotBeRecreatedByStaleOwners", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.retiredNamespaceCannotBeRecreatedByStaleOwners(); });
    runner.run("verificationCanReadTheBoardBeforePublication", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.verificationCanReadTheBoardBeforePublication(); });
    runner.run("verificationSideEffectsCannotBeOverwrittenByAStaleCommit", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.verificationSideEffectsCannotBeOverwrittenByAStaleCommit(); });
    runner.run("persistentLifecycleAndDependencies", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.persistentLifecycleAndDependencies(); });
    runner.run("validationTransactionsAndHooks", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.validationTransactionsAndHooks(); });
    runner.run("concurrentCreationAndAtomicClaim", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.concurrentCreationAndAtomicClaim(); });
    runner.run("unicodeOwnerSurvivesPersistentReload", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.unicodeOwnerSurvivesPersistentReload(); });
    runner.run("todosIsolationAndCorruptStorage", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.todosIsolationAndCorruptStorage(); });
    runner.run("processesCoordinateAndWaitingCanBeCancelled", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.processesCoordinateAndWaitingCanBeCancelled(); });
    runner.run("modelToolsResumeContextPolicyAndFork", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.modelToolsResumeContextPolicyAndFork(); });
    return runner.result();
}

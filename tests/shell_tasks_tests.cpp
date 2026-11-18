#include "iiMultimodalAgent/agent/ShellTasks.h"
#include <iiFileProviderDevice.h>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <future>
#include <QtCore/QStandardPaths>
#include <thread>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a = iiMultimodalAgent::agent;
using namespace std::chrono_literals;
namespace {
struct Fixture {
    iiFileProvider::TemporaryDirectory root{QDir::current().filePath("shell-tasks-XXXXXX")};
    std::shared_ptr<a::ShellTasks> tasks;
    std::shared_ptr<a::ToolRegistry> registry = std::make_shared<a::ToolRegistry>();
    a::ToolContext context;
    explicit Fixture(a::ShellTaskOptions options = {}, bool nested = false) {
        context.sessionId = "session-a"; context.runId = "run-a";
        context.workingDirectory = root.filePath("workspace"); QDir().mkpath(context.workingDirectory);
        tasks = std::make_shared<a::ShellTasks>(context.workingDirectory, root.filePath(nested ? "workspace/.iilocal-llm/shells" : "state"), options);
        a::registerWorkspaceTools(*registry, context.workingDirectory, tasks);
    }
    a::ToolResult call(QString name, QJsonObject args, a::ToolContext c = {}) {
        if (c.sessionId.isEmpty()) c = context;
        a::ToolRunner runner(registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
        return runner.run({"call", name, args}, c);
    }
    QString start(QString command, int timeout = 30000) {
        auto r = call("Bash", {{"command", command}, {"run_in_background", true}, {"timeout_ms", timeout}});
        if (r.isError) throw std::runtime_error(r.text.toStdString());
        return r.data["backgroundTaskId"].toString();
    }
    QJsonObject output(QString id, bool block = true, int timeout = 5000) {
        auto r = call("TaskOutput", {{"task_id", id}, {"block", block}, {"timeout", timeout}});
        if (r.isError) throw std::runtime_error(r.text.toStdString());
        return r.data;
    }
};
QByteArray read(const QString& path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
bool executing(const QByteArray& pid) {
    if (pid.toLongLong() < 2) throw std::runtime_error("Invalid process fixture PID");
    QProcess process;
#ifdef Q_OS_WIN
    auto executable = QStandardPaths::findExecutable("ps");
    if (executable.isEmpty()) {
        const auto shell = QStandardPaths::findExecutable("sh");
        executable = QDir(QFileInfo(shell).absolutePath()).absoluteFilePath("../usr/bin/ps.exe");
    }
    if (!QFileInfo(executable).isExecutable()) throw std::runtime_error("Git for Windows ps.exe is required to inspect the MSYS process ID");
    process.start(executable, {"-p", QString::fromLatin1(pid)});
#else
    process.start(QStandardPaths::findExecutable("ps"), {"-o", "stat=", "-p", QString::fromLatin1(pid)});
#endif
    if (!process.waitForFinished(1000)) throw std::runtime_error("Cannot inspect fixture process");
    const auto state = process.readAllStandardOutput().trimmed();
#ifdef Q_OS_WIN
    return QRegularExpression("(?:^|\\n)\\s*" + QString::fromLatin1(pid) + "\\s+")
        .match(QString::fromLatin1(state)).hasMatch();
#else
    return !state.isEmpty() && !state.startsWith('Z');
#endif
}
}
class ShellTaskTests : public QObject {
public:
    void liveTasksTransferOwnershipWithoutRestartingTheirProcess() {
        try {
        Fixture f;const auto id=f.start("printf $$ > worker.pid; printf before; while [ ! -f release ]; do sleep .02; done; printf after");
        const auto pidPath=f.context.workingDirectory+"/worker.pid";CTEST_WAIT_DEFAULT(!read(pidPath).isEmpty());const auto pid=read(pidPath);
        const auto moved=f.tasks->transferSession(f.context.sessionId,"session-b");CTEST_COMPARE(moved,QJsonArray{id});
        CTEST_CHECK(f.tasks->list(f.context.sessionId).isEmpty());CTEST_COMPARE(f.tasks->list("session-b").size(),1);CTEST_CHECK(executing(pid));
        CTEST_THROWS(Error,f.tasks->output(f.context.sessionId,id,false));
        QFile release(f.context.workingDirectory+"/release");CTEST_CHECK(release.open(QIODevice::WriteOnly));release.close();
        const auto done=f.tasks->output("session-b",id,true,5000);CTEST_COMPARE(done["task"].toObject()["output"],"beforeafter");
        CTEST_COMPARE(read(pidPath),pid);const auto output=done["task"].toObject()["output_file"].toString();
        CTEST_CHECK(f.tasks->ownsOutput("session-b",output));CTEST_CHECK(!f.tasks->ownsOutput(f.context.sessionId,output));
        f.tasks->close();a::ShellTasks reopened(f.context.workingDirectory,f.root.filePath("state"));
        CTEST_COMPARE(reopened.list("session-b").size(),1);CTEST_CHECK(reopened.list(f.context.sessionId).isEmpty());
        CTEST_COMPARE(reopened.output("session-b",id,false)["task"].toObject()["output"],"beforeafter");
        } catch (const std::exception& error) { agent_ctest::fail(__FILE__, __LINE__, error.what()); }
    }
    void initTestCase() {
#if (!defined(Q_OS_UNIX) && !defined(Q_OS_WIN)) || defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
        agent_ctest::skip("Background shell execution requires a desktop host");
#endif
    }
    void lifecycleOutputAndPersistentHistory() {
        Fixture f; const auto id = f.start("printf 'native-'; sleep 0.15; printf 'background'; printf 'error-stream' >&2");
        CTEST_CHECK(id.startsWith("sh-"));
        const auto done = f.output(id); CTEST_COMPARE(done["retrieval_status"], "success");
        const auto task = done["task"].toObject(); CTEST_COMPARE(task["status"], "completed"); CTEST_COMPARE(task["exitCode"].toInt(-1), 0);
        CTEST_CHECK(task["output"].toString().contains("native-background")); CTEST_CHECK(task["output"].toString().contains("error-stream"));
        const auto path = task["output_file"].toString(); CTEST_CHECK(QFileInfo(path).isFile());
        CTEST_CHECK(!f.call("Read", {{"path", path}}).isError);
        auto other = f.context; other.sessionId = "session-b";
        CTEST_CHECK(f.call("TaskOutput", {{"task_id", id}}, other).isError);
        CTEST_CHECK(f.call("Read", {{"path", path}}, other).isError);
        CTEST_CHECK(f.call("TaskStop", {{"task_id", id}}).isError);
        f.tasks->close();
        a::ShellTasks reopened(f.context.workingDirectory, f.root.filePath("state"));
        const auto recovered = reopened.output(f.context.sessionId, id, false, 0, 0, 65536);
        CTEST_COMPARE(recovered["task"].toObject()["output"], task["output"]);
        CTEST_COMPARE(reopened.list(f.context.sessionId).size(), 1); CTEST_CHECK(reopened.list("session-b").isEmpty());
    }
    void waitCancellationDoesNotStopExecution() {
        Fixture f; const auto id = f.start("printf started; sleep 30");
        CTEST_COMPARE(f.output(id, false)["retrieval_status"], "not_ready");
        CTEST_COMPARE(f.output(id, true, 20)["retrieval_status"], "timeout");
        auto context = f.context; context.cancellation = CancellationToken{};
        auto waiting = std::async(std::launch::async, [&] { return f.call("TaskOutput", {{"task_id", id}, {"timeout", 30000}}, context); });
        context.cancellation.cancel();
        try { auto response = waiting.get(); CTEST_CHECK(response.isError); }
        catch (const Error& error) { CTEST_COMPARE(error.code(), ErrorCode::Cancelled); }
        CTEST_COMPARE(f.output(id, false)["task"].toObject()["status"], "running");
        const auto stopped = f.call("TaskStop", {{"task_id", id}}); CTEST_CHECK_MESSAGE(!stopped.isError, qPrintable(stopped.text));
        CTEST_COMPARE(f.output(id)["task"].toObject()["status"], "killed");
    }
    void stopsProcessGroupAndShutdownJoins() {
        Fixture f;
        const auto id = f.start("printf '%s' \"$$\" > shell.pid; sleep 30 & printf '%s' \"$!\" > child.pid; wait; printf late > late.txt");
        CTEST_WAIT_VERIFY(read(f.root.filePath("workspace/child.pid")).toLongLong() > 1, 3000);
        const auto parent = read(f.root.filePath("workspace/shell.pid")), child = read(f.root.filePath("workspace/child.pid"));
        CTEST_CHECK(executing(parent)); CTEST_CHECK(executing(child));
        CTEST_CHECK(!f.call("TaskStop", {{"shell_id", id}}).isError);
        CTEST_WAIT_VERIFY(!executing(parent) && !executing(child), 3000);
        CTEST_COMPARE(f.output(id)["task"].toObject()["status"], "killed");
        CTEST_CHECK(!QFileInfo::exists(f.root.filePath("workspace/late.txt")));
        const auto second = f.start("sleep 30; printf leaked > leaked.txt");
        f.tasks->close(); CTEST_COMPARE(f.tasks->output(f.context.sessionId, second, false, 0, 0, 1024)["task"].toObject()["status"], "killed");
        CTEST_CHECK(!QFileInfo::exists(f.root.filePath("workspace/leaked.txt")));
        CTEST_CHECK(f.call("Bash", {{"command", "printf bad"}, {"run_in_background", true}}).isError);
    }
    void preservesOutputWrittenDuringTermination() {
#ifdef Q_OS_WIN
        agent_ctest::skip("Windows job termination is immediate and does not deliver POSIX TERM traps; subtree cleanup is verified separately");
#endif
        Fixture f;
        const auto id = f.start("trap 'printf stopped-log; exit 0' TERM; printf ready > trap.ready; while :; do sleep 30 & wait $!; done");
        CTEST_WAIT_COMPARE_TIMEOUT(read(f.root.filePath("workspace/trap.ready")), QByteArray("ready"), 3000);
        CTEST_CHECK(!f.call("TaskStop", {{"task_id", id}}).isError);
        const auto task = f.output(id)["task"].toObject();
        CTEST_COMPARE(task["status"], "killed");
        CTEST_CHECK_MESSAGE(task["output"].toString().contains("stopped-log"), qPrintable(task["output"].toString()));
        CTEST_COMPARE(read(task["output_file"].toString()), QByteArray::fromBase64(task["output_base64"].toString().toLatin1()));
    }
    void timeoutCapacityAndOutputLimit() {
        a::ShellTaskOptions options; options.maxConcurrent = 1; options.maxOutputBytes = 1024;
        Fixture f(options); auto id = f.start("sleep 30");
        CTEST_CHECK(f.call("Bash", {{"command", "printf should-not-start"}, {"run_in_background", true}}).isError);
        CTEST_CHECK(!f.call("TaskStop", {{"task_id", id}}).isError);
        id = f.start("sleep 30", 100);
        auto task = f.output(id)["task"].toObject(); CTEST_COMPARE(task["status"], "failed"); CTEST_COMPARE(task["error_code"], "timeout");
        id = f.start("while :; do printf 0123456789012345678901234567890123456789; done");
        task = f.output(id)["task"].toObject(); CTEST_COMPARE(task["error_code"], "resource_limit");
        CTEST_CHECK(read(task["output_file"].toString()).size() <= 1024);
        id = f.start("printf failure >&2; exit 7"); task = f.output(id)["task"].toObject();
        CTEST_COMPARE(task["status"], "failed"); CTEST_COMPARE(task["exitCode"].toInt(-1), 7);
    }
    void permissionsAndInputValidationPrecedeStart() {
        Fixture f;
        a::ToolRunner denied(f.registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Plan));
        auto r = denied.run({"x", "Bash", {{"command", "printf forbidden > forbidden.txt"}, {"run_in_background", true}}}, f.context);
        CTEST_CHECK(r.isError); CTEST_CHECK(f.tasks->list(f.context.sessionId).isEmpty());
        CTEST_CHECK(!QFileInfo::exists(f.root.filePath("workspace/forbidden.txt")));
        CTEST_CHECK(f.call("Bash", {{"command", "echo bad"}, {"run_in_background", "true"}}).isError);
        CTEST_CHECK(f.call("TaskOutput", {{"task_id", "../../etc/passwd"}}).isError);
        CTEST_CHECK(f.call("TaskStop", {}).isError);
        CTEST_CHECK(f.tasks->list(f.context.sessionId).isEmpty());
    }
    void crashDoesNotExposeAnUndefinedExitCode() {
        Fixture f; const auto id = f.start("kill -KILL $$");
        const auto task = f.output(id)["task"].toObject();
        CTEST_COMPARE(task["status"], "failed"); CTEST_COMPARE(task["error_code"], "runtime_failure");
        CTEST_CHECK(task["exitCode"].isNull());
    }
    void nestedStateProtectionAndBytePaging() {
        Fixture f({}, true); const auto id = f.start("printf 'private_marker_123456789'");
        const auto task = f.output(id)["task"].toObject(); const auto output = task["output_file"].toString();
        const auto state = QFileInfo(output).dir().filePath("state.json");
        CTEST_CHECK(f.call("Read", {{"path", state}}).isError);
        CTEST_CHECK(f.call("Grep", {{"pattern", "private_marker"}}).data["matches"].toArray().isEmpty());
        auto other = f.context; other.sessionId = "another";
        CTEST_CHECK(f.call("Read", {{"path", output}}, other).isError);
        const auto page = f.tasks->output(f.context.sessionId, id, false, 0, 8, 6)["task"].toObject();
        CTEST_COMPARE(page["output"], "marker"); CTEST_COMPARE(page["next_offset"].toInt(), 14); CTEST_CHECK(page["has_more"].toBool());
    }
    void recoveryDoesNotReplayAndRejectsCorruptState() {
        Fixture f; const auto id = f.start("printf once >> invocation-count; printf saved-output");
        auto task = f.output(id)["task"].toObject(); f.tasks->close();
        const auto path = QFileInfo(task["output_file"].toString()).dir().filePath("state.json");
        auto metadata = QJsonDocument::fromJson(read(path)).object(); metadata["status"] = "running";
        metadata["finished_at"] = QJsonValue::Null; metadata["exitCode"] = QJsonValue::Null;
        auto write = [&] { QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write(QJsonDocument(metadata).toJson()); };
        write();
        { a::ShellTasks recovered(f.context.workingDirectory, f.root.filePath("state"));
          task = recovered.output(f.context.sessionId, id, false, 0, 0, 1024)["task"].toObject();
          CTEST_COMPARE(task["status"], "interrupted"); CTEST_COMPARE(task["error_code"], "host_interrupted");
          CTEST_COMPARE(task["output"], "saved-output"); }
        CTEST_COMPARE(read(f.root.filePath("workspace/invocation-count")), QByteArray("once"));
        metadata = QJsonDocument::fromJson(read(path)).object(); metadata["exitCode"] = "not-an-integer"; write();
        CTEST_THROWS(Error, a::ShellTasks(f.context.workingDirectory, f.root.filePath("state")));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    ShellTaskTests tests;
    try { tests.initTestCase(); }
    catch (const agent_ctest::SkippedCase& skipped) { std::cerr << "initTestCase: skipped: " << skipped.reason << '\n'; return 77; }
    catch (const std::exception& error) { std::cerr << "initTestCase: " << error.what() << '\n'; return 1; }
    catch (...) { std::cerr << "initTestCase: unexpected exception\n"; return 1; }
    runner.run("liveTasksTransferOwnershipWithoutRestartingTheirProcess", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.liveTasksTransferOwnershipWithoutRestartingTheirProcess(); });
    runner.run("lifecycleOutputAndPersistentHistory", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.lifecycleOutputAndPersistentHistory(); });
    runner.run("waitCancellationDoesNotStopExecution", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.waitCancellationDoesNotStopExecution(); });
    runner.run("stopsProcessGroupAndShutdownJoins", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.stopsProcessGroupAndShutdownJoins(); });
    runner.run("preservesOutputWrittenDuringTermination", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.preservesOutputWrittenDuringTermination(); });
    runner.run("timeoutCapacityAndOutputLimit", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.timeoutCapacityAndOutputLimit(); });
    runner.run("permissionsAndInputValidationPrecedeStart", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.permissionsAndInputValidationPrecedeStart(); });
    runner.run("crashDoesNotExposeAnUndefinedExitCode", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.crashDoesNotExposeAnUndefinedExitCode(); });
    runner.run("nestedStateProtectionAndBytePaging", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.nestedStateProtectionAndBytePaging(); });
    runner.run("recoveryDoesNotReplayAndRejectsCorruptState", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.recoveryDoesNotReplayAndRejectsCorruptState(); });
    return runner.result();
}

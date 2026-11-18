#include <iiMultimodalAgent/agent/Worktrees.h>
#include <iiMultimodalAgent/agent/Engine.h>
#include <iiMultimodalAgent/agent/PermissionSettings.h>
#include <iiMultimodalAgent/agent/ShellTasks.h>
#include <QtCore/QJsonDocument>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QCoreApplication>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
QString shellWorkspacePath(QString path) {
#ifdef Q_OS_WIN
    path = QDir::fromNativeSeparators(path);
    if (path.size() >= 3 && path[1] == ':') return "/" + path.left(1).toLower() + path.mid(2);
#endif
    return path;
}

QByteArray git(const QString& root,QStringList args) {
    QProcess p;p.setWorkingDirectory(root);p.start("git",args);
    if(!p.waitForStarted(5000)||!p.waitForFinished(30000)||p.exitCode()!=0)
        throw std::runtime_error(("fixture git failed ("+args.join(' ').toUtf8()+"): "+p.errorString().toUtf8()+"; exit="+QByteArray::number(p.exitCode())+"; "+p.readAllStandardError()).constData());
    return p.readAllStandardOutput().trimmed();
}
void write(const QString& path,const QByteArray& bytes) {
    QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("fixture write");
}
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("worktree-test-XXXXXX")};
    QString repo=root.filePath("repo"),state=root.filePath("state");
    a::ToolContext context;a::WorktreeOptions options;
    Fixture(){QDir().mkpath(repo);git(repo,{"init","-q","-b","main"});git(repo,{"config","user.name","Fixture"});git(repo,{"config","user.email","fixture@example.invalid"});
        write(repo+"/main.cpp","int original=1;\n");git(repo,{"add","main.cpp"});git(repo,{"commit","-qm","initial"});
        context.sessionId="owner";context.workingDirectory=repo;options.directory=root.filePath("worktrees");options.fetchMissingBase=false;}
};
}
class WorktreeTests:public QObject {
public:
 void realGitEnterKeepAndResumePreserveTheOriginalCheckout(){
    Fixture f;const auto cwd=QDir::currentPath();a::Worktrees worktrees(f.state,f.options);
    const auto created=worktrees.enter({{"name","user/feature"}},f.context);CTEST_CHECK(!created.isError);
    const auto path=created.data["worktreePath"].toString();CTEST_CHECK(QFileInfo(path+"/.git").isFile());
    CTEST_COMPARE(git(path,{"branch","--show-current"}),QByteArray("worktree-user+feature"));
    CTEST_COMPARE(worktrees.view(f.context).directory,path);CTEST_COMPARE(QDir::currentPath(),cwd);
    write(path+"/main.cpp","int isolated=2;\n");
    const auto kept=worktrees.exit({{"action","keep"}},f.context);CTEST_CHECK(!kept.isError);CTEST_CHECK(QFileInfo(path).isDir());
    CTEST_COMPARE(worktrees.view(f.context).directory,f.repo);CTEST_COMPARE(git(f.repo,{"status","--porcelain"}),QByteArray());
    const auto resumed=worktrees.enter({{"name","user/feature"}},f.context);CTEST_COMPARE(resumed.data["worktreePath"].toString(),path);
    a::Worktrees reopened(f.state,f.options);CTEST_COMPARE(reopened.view(f.context).directory,path);
 }
 void removeRequiresDiscardForFilesAndNewCommits(){
    Fixture f;a::Worktrees worktrees(f.state,f.options);const auto path=worktrees.enter({{"name","dirty"}},f.context).data["worktreePath"].toString();
    write(path+"/new.txt","valuable\n");CTEST_THROWS(Error,worktrees.exit({{"action","remove"}},f.context));CTEST_CHECK(QFileInfo(path+"/new.txt").exists());
    git(path,{"add","new.txt"});git(path,{"commit","-qm","isolated change"});CTEST_THROWS(Error,worktrees.exit({{"action","remove"}},f.context));
    const auto removed=worktrees.exit({{"action","remove"},{"discard_changes",true}},f.context);
    CTEST_COMPARE(removed.data["discardedCommits"].toInt(),1);CTEST_CHECK(!QFileInfo(path).exists());
    CTEST_COMPARE(git(f.repo,{"branch","--list","worktree-dirty"}),QByteArray());CTEST_COMPARE(worktrees.view(f.context).directory,f.repo);
 }
 void ownershipCollisionsAndInvalidNamesCannotDeleteOtherWork(){
    Fixture f;a::Worktrees worktrees(f.state,f.options);
    for(const auto& name:{"../bad","/absolute","a//b","a+other","a/..","a..b","a.",""})CTEST_THROWS(Error,worktrees.enter({{"name",name}},f.context));
    git(f.repo,{"branch","worktree-existing"});CTEST_THROWS(Error,worktrees.enter({{"name","existing"}},f.context));
    const auto path=worktrees.enter({{"name","owner-only"}},f.context).data["worktreePath"].toString();
    auto other=f.context;other.sessionId="other";
    CTEST_THROWS(Error,worktrees.enter({{"name","owner-only"}},other));
    CTEST_THROWS(Error,worktrees.exit({{"action","remove"},{"discard_changes",true}},other));
    CTEST_CHECK(QFileInfo(path).isDir());CTEST_CHECK(!git(f.repo,{"branch","--list","worktree-existing"}).isEmpty());
 }
 void failedCreationAndMissingWorktreeCanReturnWithoutDeletingData(){
    Fixture f;auto options=f.options;options.create=[](const QString&,const a::ToolContext&)->QString{throw Error(ErrorCode::Cancelled,"creation cancelled");};
    a::Worktrees failing(f.root.filePath("failed-state"),options);CTEST_THROWS(Error,failing.enter({{"name","cancelled"}},f.context));
    const auto status=failing.status(f.context);CTEST_CHECK(!status["active"].toBool());CTEST_COMPARE(status["retained"].toArray().first().toObject()["phase"].toString(),"failed");
    a::Worktrees worktrees(f.state,f.options);const auto path=worktrees.enter({{"name","missing"}},f.context).data["worktreePath"].toString();
    git(f.repo,{"worktree","remove",path});CTEST_CHECK(worktrees.status(f.context)["recoveryRequired"].toBool());
    CTEST_CHECK(!worktrees.exit({{"action","keep"}},f.context).isError);CTEST_COMPARE(worktrees.view(f.context).directory,f.repo);
 }
 void preparedOperationsCannotReuseChangedStateOrDiscardNewContents(){
    Fixture f;a::Worktrees worktrees(f.state,f.options);
    const auto pending=worktrees.enterTool().prepare({},f.context);CTEST_CHECK(!pending.definition.metadata["worktree_name"].toString().isEmpty());
    const auto path=worktrees.enter({{"name","other"}},f.context).data["worktreePath"].toString();CTEST_THROWS(Error,pending.execute());
    const auto removal=worktrees.exitTool().prepare({{"action","remove"},{"discard_changes",true}},f.context);
    write(path+"/new.txt","arrived after preview");CTEST_THROWS(Error,removal.execute());CTEST_CHECK(QFileInfo(path+"/new.txt").exists());
    git(f.repo,{"worktree","lock",path});CTEST_THROWS(Error,worktrees.exit({{"action","remove"},{"discard_changes",true}},f.context));
    CTEST_CHECK(worktrees.status(f.context)["active"].toBool());git(f.repo,{"worktree","unlock",path});
 }
 void removalPreviewRequiresACompleteGitSnapshot(){
    Fixture f;a::Worktrees worktrees(f.state,f.options);const auto path=worktrees.enter({{"name","large"}},f.context).data["worktreePath"].toString();
    QFile large(path+"/large.bin");CTEST_CHECK(large.open(QIODevice::WriteOnly));CTEST_CHECK(large.resize(65LL*1024*1024));large.close();
    const auto status=worktrees.status(f.context);CTEST_CHECK(!status["changesKnown"].toBool());CTEST_CHECK(status["recoveryRequired"].toBool());
    CTEST_THROWS(Error,worktrees.exitTool().prepare({{"action","remove"},{"discard_changes",true}},f.context));
    CTEST_CHECK(QFileInfo(path+"/large.bin").exists());CTEST_CHECK(!worktrees.exitTool().prepare({{"action","keep"}},f.context).execute().isError);
    CTEST_COMPARE(worktrees.view(f.context).directory,f.repo);CTEST_CHECK(QFileInfo(path).isDir());
 }
 void sparseCheckoutAndWhitespacePathsRetainTheOriginalChanges(){
    Fixture f;const auto renamed=f.root.filePath(
#ifdef Q_OS_WIN
        "repo with space"
#else
        "repo with space "
#endif
    );CTEST_CHECK(QDir().rename(f.repo,renamed));f.repo=renamed;f.context.workingDirectory=f.repo;
    QDir().mkpath(f.repo+"/one");QDir().mkpath(f.repo+"/two");write(f.repo+"/one/a.txt","selected");write(f.repo+"/two/b.txt","excluded");
    git(f.repo,{"add","."});git(f.repo,{"commit","-qm","folders"});write(f.repo+"/main.cpp","original uncommitted work");write(f.repo+"/untracked.txt","original untracked work");
    f.options.sparsePaths={"one"};a::Worktrees worktrees(f.state,f.options);const auto path=worktrees.enter({{"name","sparse"}},f.context).data["worktreePath"].toString();
    CTEST_CHECK(QFileInfo(path+"/one/a.txt").exists());CTEST_CHECK(!QFileInfo(path+"/two/b.txt").exists());CTEST_CHECK(!QFileInfo(path+"/untracked.txt").exists());
    CTEST_CHECK(!worktrees.exit({{"action","remove"}},f.context).isError);CTEST_CHECK(git(f.repo,{"status","--porcelain"}).contains("untracked.txt"));
 }
 void nonGitAdaptersAreOwnerBoundAndRemovalMustActuallySucceed(){
    Fixture f;const auto hookRoot=f.root.filePath("hook-work");QDir().mkpath(hookRoot);write(hookRoot+"/valuable","data");
    f.options.create=[hookRoot](const QString&,const a::ToolContext&){return hookRoot;};f.options.remove=[](const QString&,const a::ToolContext&){};
    a::Worktrees worktrees(f.state,f.options);worktrees.enter({{"name","hook"}},f.context);CTEST_CHECK(!worktrees.status(f.context)["changesKnown"].toBool());
    CTEST_THROWS(Error,worktrees.exit({{"action","remove"}},f.context));CTEST_THROWS(Error,worktrees.exit({{"action","remove"},{"discard_changes",true}},f.context));
    CTEST_CHECK(worktrees.status(f.context)["active"].toBool());CTEST_CHECK(QFileInfo(hookRoot+"/valuable").exists());worktrees.exit({{"action","keep"}},f.context);
 }
 void cachedRemoteBaseAndChangedFileContentAreBound(){
    Fixture f;git(f.repo,{"update-ref","refs/remotes/origin/main","HEAD"});git(f.repo,{"symbolic-ref","refs/remotes/origin/HEAD","refs/remotes/origin/main"});
    write(f.repo+"/later.txt","local commit");git(f.repo,{"add","later.txt"});git(f.repo,{"commit","-qm","local ahead"});
    a::Worktrees worktrees(f.state,f.options);const auto path=worktrees.enter({{"name","remote"}},f.context).data["worktreePath"].toString();CTEST_CHECK(!QFileInfo(path+"/later.txt").exists());
    write(path+"/main.cpp","before preview");const auto prepared=worktrees.exitTool().prepare({{"action","remove"},{"discard_changes",true}},f.context);
    write(path+"/main.cpp","after! preview");CTEST_THROWS(Error,prepared.execute());CTEST_CHECK(QFileInfo(path).exists());
 }
 void boundScopesReloadSettingsAndExpireReadsWithoutGrantingTheOriginalRoot(){
    Fixture f;write(f.repo+"/host-private.txt","private");git(f.repo,{"add","host-private.txt"});git(f.repo,{"commit","-qm","private fixture"});
    QDir().mkpath(f.root.filePath("extra"));QDir().mkpath(f.options.directory+"/extra");
    auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,f.repo,{}, {f.repo+"/host-private.txt"});
    a::PermissionSettingsOptions config;config.workingDirectory=f.repo;config.additionalDirectories={"../extra"};config.fallbackMode=a::PermissionMode::Bypass;auto policy=std::make_shared<a::SettingsPermissionPolicy>(config);a::ToolRunner runner(registry,policy);
    CTEST_CHECK(!runner.run({"read","Read",{{"path","main.cpp"}}},f.context).isError);
    a::Worktrees worktrees(f.state,f.options);const auto path=worktrees.enter({{"name","settings"}},f.context).data["worktreePath"].toString();
    auto scope=f.context;scope.workingDirectory=path;CTEST_CHECK(runner.run({"unbound","Read",{{"path","main.cpp"}}},scope).isError);
    scope.originalWorkingDirectory=f.repo;scope.workspaceRevision=worktrees.view(f.context).revision;
    CTEST_CHECK(!runner.run({"bound","Read",{{"path","main.cpp"}}},scope).isError);CTEST_CHECK(runner.run({"outside","Read",{{"path",f.repo+"/main.cpp"}}},scope).isError);
    CTEST_CHECK(runner.run({"private","Read",{{"path","host-private.txt"}}},scope).isError);
    CTEST_CHECK(policy->workingDirectories(scope).contains(f.root.filePath("extra")));CTEST_CHECK(!policy->workingDirectories(scope).contains(f.options.directory+"/extra"));
    QDir().mkpath(path+"/.claude");write(path+"/.claude/settings.json",R"json({"permissions":{"deny":["Read(main.cpp)"]}})json");
    CTEST_CHECK(runner.run({"denied","Read",{{"path","main.cpp"}}},scope).isError);CTEST_COMPARE(policy->describe(scope)["working_directories"].toArray().first().toString(),path);
    worktrees.exit({{"action","keep"}},f.context);scope.workingDirectory=f.repo;scope.workspaceRevision=worktrees.view(f.context).revision;
    CTEST_CHECK(runner.run({"old-read","Write",{{"path","main.cpp"},{"content","must not write"}}},scope).isError);
    CTEST_CHECK(!runner.run({"fresh","Read",{{"path","main.cpp"}}},scope).isError);
 }
 void engineSwitchesFilesShellPolicyAndPromptInsideOneToolBatch(){
    class Model:public a::Model {public:int turns=0;QString path;a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const std::function<bool(const QString&)>&)override {
        if(turns++==0)return {{},{{"enter","EnterWorktree",{{"name","batch"}}},{"read","Read",{{"path","main.cpp"}}},
            {"write","Write",{{"path","main.cpp"},{"content","isolated change"}}},{"shell","Bash",{{"command","pwd; cat main.cpp"}}},
            {"keep","ExitWorktree",{{"action","keep"}}},{"original","Write",{{"path","original-only.txt"},{"content","original workspace"}}}},{1,1}};
        return {"DONE",{},{1,1}};
    }};
    Fixture f;auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,f.repo);
    a::PermissionSettingsOptions settings;settings.workingDirectory=f.repo;settings.fallbackMode=a::PermissionMode::Bypass;
    auto policy=std::make_shared<a::SettingsPermissionPolicy>(settings);a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=f.state;options.worktrees=f.options;options.worktrees.enabled=true;options.worktrees.deferred=false;options.projectMemoryEnabled=true;options.memoryRecall.enabled=false;options.memoryExtraction.enabled=false;
    a::Engine engine(model,registry,policy,options);const auto session=engine.createSession("fixture",f.repo);const auto result=engine.run({session.id,"Create an isolated worktree, edit and leave keeping it."}).result.get();
    CTEST_CHECK_MESSAGE(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage));CTEST_COMPARE(result.text,"DONE");
    const auto history=engine.session(session.id);for(const auto& message:history.messages)if(message.role==a::MessageRole::Tool)CTEST_CHECK_MESSAGE(!message.isError,qPrintable(message.text));
    QFile original(f.repo+"/main.cpp");CTEST_CHECK(original.open(QIODevice::ReadOnly));CTEST_COMPARE(original.readAll(),QByteArray("int original=1;\n"));
    const auto path=f.options.directory+"/batch";QFile changed(path+"/main.cpp");CTEST_CHECK(changed.open(QIODevice::ReadOnly));CTEST_COMPARE(changed.readAll(),QByteArray("isolated change"));
    bool shell=false;for(const auto& message:history.messages)if(message.toolCallId=="shell"){shell=true;CTEST_CHECK(message.text.contains(shellWorkspacePath(path)));CTEST_CHECK(message.text.contains("isolated change"));}CTEST_CHECK(shell);
    CTEST_CHECK(QFileInfo(f.repo+"/original-only.txt").exists());CTEST_CHECK(!QFileInfo(path+"/original-only.txt").exists());CTEST_COMPARE(engine.sessionMetadata(session.id).workingDirectory,f.repo);
 }
 void sessionEndHooksObserveTheCurrentWorktree(){
    class Model:public a::Model {public:a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const std::function<bool(const QString&)>&)override{return {"READY",{},{1,1}};}};
    Fixture f;auto registry=std::make_shared<a::ToolRegistry>();auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=f.state;options.worktrees=f.options;options.worktrees.enabled=true;
    QString cwd,snapshot;options.hooks.append([&](const a::HookInput& input,const CancellationToken&){if(input.kind==a::HookKind::SessionEnd){cwd=input.context["cwd"].toString();if(input.modelContext&&input.modelContext->session)snapshot=input.modelContext->session->workingDirectory;}return a::HookResult{};});
    a::Engine engine(std::make_shared<Model>(),registry,policy,options);const auto id=engine.createSession("fixture",f.repo).id;
    CTEST_COMPARE(engine.run({id,"Start"}).result.get().text,QString("READY"));const auto entered=engine.runWorktreeTool(id,"EnterWorktree",{{"name","end-hook"}});
    CTEST_CHECK_MESSAGE(!entered.isError,qPrintable(entered.text));const auto path=entered.data["worktreePath"].toString();engine.endSession(id);
    CTEST_COMPARE(cwd,path);CTEST_COMPARE(snapshot,path);CTEST_CHECK(engine.worktreeStatus(id)["active"].toBool());
 }
 void engineResumeAndBackgroundShellUseTheOwnedWorkspace(){
    class Model:public a::Model {public:a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const std::function<bool(const QString&)>&)override{return {"DONE",{},{1,1}};}};
    Fixture f;auto model=std::make_shared<Model>();auto registry=std::make_shared<a::ToolRegistry>();auto shells=std::make_shared<a::ShellTasks>(f.repo,f.root.filePath("shells"));a::registerWorkspaceTools(*registry,f.repo,shells);
    auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=f.state;options.worktrees=f.options;options.worktrees.enabled=true;
    QString id,path;
    {a::Engine engine(model,registry,policy,options);id=engine.createSession("fixture",f.repo).id;auto entered=engine.runWorktreeTool(id,"EnterWorktree",{{"name","persistent"}});CTEST_CHECK_MESSAGE(!entered.isError,qPrintable(entered.text));path=entered.data["worktreePath"].toString();
     CTEST_COMPARE(engine.session(id).workingDirectory,path);CTEST_COMPARE(engine.sessionMetadata(id).workingDirectory,f.repo);CTEST_THROWS(Error,engine.clearSession(id));
     auto child=engine.forkSession(id);CTEST_COMPARE(child.workingDirectory,f.repo);CTEST_CHECK(!engine.worktreeStatus(child.id)["active"].toBool());
     a::ToolContext context{id,{},f.repo};auto scope=engine.bindWorkspaceContext(context);auto task=shells->start(context,"pwd",{},10000);scope.reset();
     const auto output=shells->output(id,task["task_id"].toString());CTEST_CHECK_MESSAGE(QJsonDocument(output).toJson().contains(shellWorkspacePath(path).toUtf8()),QJsonDocument(output).toJson().constData());
     auto running=engine.runShellTool(id,"Bash",{{"command","sleep 30"},{"run_in_background",true}});CTEST_CHECK_MESSAGE(!running.isError,qPrintable(running.text));
     auto removal=engine.runWorktreeTool(id,"ExitWorktree",{{"action","remove"}});CTEST_CHECK(removal.isError);CTEST_CHECK(QFileInfo(path).exists());
     CTEST_CHECK(!engine.runShellTool(id,"TaskStop",{{"task_id",running.data["task_id"]}}).isError);}
    {a::Engine engine(model,registry,policy,options);CTEST_COMPARE(engine.session(id).workingDirectory,path);auto result=engine.run({id,"Resume"}).result.get();CTEST_CHECK(result.status==a::RunStatus::Completed);
     CTEST_CHECK(!engine.runWorktreeTool(id,"ExitWorktree",{{"action","remove"}}).isError);CTEST_COMPARE(engine.session(id).workingDirectory,f.repo);CTEST_CHECK(!QFileInfo(path).exists());}
 }
 void ignoredFilesAndChangedBranchAreProtected(){
    Fixture f;a::Worktrees worktrees(f.state,f.options);const auto path=worktrees.enter({{"name","ignored"}},f.context).data["worktreePath"].toString();
    write(path+"/.gitignore","cache/\n");git(path,{"add",".gitignore"});git(path,{"commit","-qm","ignore cache"});
    QDir().mkpath(path+"/cache");write(path+"/cache/data.bin","keep me");
    CTEST_CHECK(worktrees.status(f.context)["changedFiles"].toInt()>0);
    CTEST_THROWS(Error,worktrees.exit({{"action","remove"}},f.context));
    git(path,{"checkout","-qb","unrelated"});CTEST_THROWS(Error,worktrees.exit({{"action","remove"},{"discard_changes",true}},f.context));
    CTEST_CHECK(QFileInfo(path+"/cache/data.bin").exists());
 }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    WorktreeTests tests;
    runner.run("realGitEnterKeepAndResumePreserveTheOriginalCheckout", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.realGitEnterKeepAndResumePreserveTheOriginalCheckout(); });
    runner.run("removeRequiresDiscardForFilesAndNewCommits", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.removeRequiresDiscardForFilesAndNewCommits(); });
    runner.run("ownershipCollisionsAndInvalidNamesCannotDeleteOtherWork", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.ownershipCollisionsAndInvalidNamesCannotDeleteOtherWork(); });
    runner.run("failedCreationAndMissingWorktreeCanReturnWithoutDeletingData", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.failedCreationAndMissingWorktreeCanReturnWithoutDeletingData(); });
    runner.run("preparedOperationsCannotReuseChangedStateOrDiscardNewContents", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.preparedOperationsCannotReuseChangedStateOrDiscardNewContents(); });
    runner.run("removalPreviewRequiresACompleteGitSnapshot", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.removalPreviewRequiresACompleteGitSnapshot(); });
    runner.run("sparseCheckoutAndWhitespacePathsRetainTheOriginalChanges", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.sparseCheckoutAndWhitespacePathsRetainTheOriginalChanges(); });
    runner.run("nonGitAdaptersAreOwnerBoundAndRemovalMustActuallySucceed", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.nonGitAdaptersAreOwnerBoundAndRemovalMustActuallySucceed(); });
    runner.run("cachedRemoteBaseAndChangedFileContentAreBound", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.cachedRemoteBaseAndChangedFileContentAreBound(); });
    runner.run("boundScopesReloadSettingsAndExpireReadsWithoutGrantingTheOriginalRoot", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.boundScopesReloadSettingsAndExpireReadsWithoutGrantingTheOriginalRoot(); });
    runner.run("engineSwitchesFilesShellPolicyAndPromptInsideOneToolBatch", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.engineSwitchesFilesShellPolicyAndPromptInsideOneToolBatch(); });
    runner.run("sessionEndHooksObserveTheCurrentWorktree", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.sessionEndHooksObserveTheCurrentWorktree(); });
    runner.run("engineResumeAndBackgroundShellUseTheOwnedWorkspace", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.engineResumeAndBackgroundShellUseTheOwnedWorkspace(); });
    runner.run("ignoredFilesAndChangedBranchAreProtected", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.ignoredFilesAndChangedBranchAreProtected(); });
    return runner.result();
}

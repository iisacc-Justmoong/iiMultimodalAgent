#include "native_link.h"
#include "iiMultimodalAgent/agent/PermissionSettings.h"
#include "iiMultimodalAgent/agent/Engine.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include "CTestSupport.h"
#include <QCoreApplication>
#include <chrono>
#include <thread>
#include <future>
#include <barrier>
#include <iostream>
#ifdef Q_OS_UNIX
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
void save(const QString& path,const QJsonObject& value) {
    QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);
    if(!file.open(QIODevice::WriteOnly)||file.write(QJsonDocument(value).toJson())<0)throw std::runtime_error("Fixture write failed");
}
QJsonObject read(const QString& path) {QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Fixture read failed");return QJsonDocument::fromJson(file.readAll()).object();}
QJsonObject rules(const QString& type,const QString& destination,const QString& behavior,const QString& tool,const QString& content={}) {
    QJsonObject rule{{"toolName",tool}};if(!content.isEmpty())rule["ruleContent"]=content;
    return {{"type",type},{"destination",destination},{"behavior",behavior},{"rules",QJsonArray{rule}}};
}
QJsonObject dirs(const QString& type,const QString& destination,const QString& directory) {return {{"type",type},{"destination",destination},{"directories",QJsonArray{directory}}};}
QJsonObject mode(const QString& destination,const QString& value){return {{"type","setMode"},{"destination",destination},{"mode",value}};}
struct Fixture {
    QTemporaryDir root;QString work=root.filePath("work");a::PermissionSettingsOptions options;
    Fixture(){QDir().mkpath(work);options.workingDirectory=work;options.userDirectory=root.filePath("user");options.managedDirectory=root.filePath("managed");}
    a::ToolContext context(const QString& id="one") const{return {id,{},work};}
};
a::PermissionBehavior decide(const a::SettingsPermissionPolicy& policy,const a::ToolContext& context,const QString& path) {
    return policy.decide({"Write","write",{{"type","object"}}},{{"path",path}},context).behavior;
}
class Model final:public a::Model {public:a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const TextCallback&)override{return {"DONE"};}};
}
class PermissionUpdatesTests final:public QObject {
public:
    void approvalAppliesDirectoryGrantBeforePreparingChangedInput() {
        for(bool valid:{true,false}) {
            Fixture f;const auto outside=f.root.filePath("outside");QDir().mkpath(outside);
            auto policy=std::make_shared<a::SettingsPermissionPolicy>(f.options);auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,f.work);
            a::ToolRunnerOptions options;options.permissionResponse=[&](const auto&,const auto&,const auto&){a::PermissionResponse result;result.behavior=a::PermissionBehavior::Allow;
                result.updatedArguments=QJsonObject{{"path",outside+"/created.txt"},{"content",valid?QJsonValue("APPROVED"):QJsonValue(7)}};
                result.updatedPermissions={dirs("addDirectories","session",outside)};return result;};
            const auto result=a::ToolRunner(registry,policy,options).run({"call","Write",{{"path","original.txt"},{"content","ORIGINAL"}}},f.context());
            CTEST_COMPARE(result.isError,!valid);CTEST_COMPARE(QFileInfo::exists(outside+"/created.txt"),valid);
            CTEST_COMPARE(policy->workingDirectories(f.context()).contains(outside),valid);CTEST_CHECK(!QFileInfo::exists(f.work+"/original.txt"));
        }
    }
    void approvalRemovalDropsCachedRootsAndRejectsSuppliedSnapshots() {
        for(bool suppliedSnapshot:{false,true}) {
            Fixture f;const auto outside=f.root.filePath("outside");QDir().mkpath(outside);f.options.additionalDirectories={outside};
            auto policy=std::make_shared<a::SettingsPermissionPolicy>(f.options);auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,f.work);
            a::ToolRunnerOptions options;options.permissionResponse=[&](const auto&,const auto&,const auto&){a::PermissionResponse response;response.behavior=a::PermissionBehavior::Allow;
                response.updatedPermissions={dirs("removeDirectories","cliArg",outside)};return response;};
            auto context=f.context();if(suppliedSnapshot)context.workingDirectories={outside};
            const auto result=a::ToolRunner(registry,policy,options).run({"write","Write",{{"path",outside+"/out.txt"},{"content","REMOVED"}}},context);
            CTEST_CHECK(result.isError);CTEST_CHECK(!QFileInfo::exists(outside+"/out.txt"));
            CTEST_CHECK(!policy->workingDirectories(f.context()).contains(outside));
        }
    }
    void persistentUpdatesPreserveUnrelatedSettingsAndSurviveRestart() {
        Fixture f;const auto path=f.work+"/.claude/settings.json";
        save(path,{{"theme","USER_THEME"},{"permissions",QJsonObject{{"allow",QJsonArray{"Write(/old/**)"}},{"deny",QJsonArray{"Write(/guard/**)"}}}}});
        a::SettingsPermissionPolicy policy(f.options);const auto c=f.context();
        policy.applyUpdates({rules("addRules","projectSettings","allow","Write","/new/**"),rules("removeRules","projectSettings","allow","Write","/old/**"),
            rules("replaceRules","localSettings","ask","Write","/review/**"),rules("addRules","userSettings","allow","Bash","git status")},c);
        CTEST_COMPARE(read(path)["theme"],"USER_THEME");CTEST_COMPARE(read(path)["permissions"].toObject()["allow"].toArray(),QJsonArray{"Write(/new/**)"});
        CTEST_COMPARE(decide(policy,c,"new/out"),a::PermissionBehavior::Allow);CTEST_COMPARE(decide(policy,c,"old/out"),a::PermissionBehavior::Ask);
        CTEST_COMPARE(decide(policy,c,"guard/out"),a::PermissionBehavior::Deny);CTEST_COMPARE(decide(policy,c,"review/out"),a::PermissionBehavior::Ask);
        a::SettingsPermissionPolicy reopened(f.options);CTEST_COMPARE(decide(reopened,c,"new/out"),a::PermissionBehavior::Allow);
        policy.applyUpdates({mode("localSettings","plan")},c);CTEST_COMPARE(policy.describe(c)["mode"],"plan");
        CTEST_COMPARE(a::SettingsPermissionPolicy(f.options).snapshot().mode,a::PermissionMode::Plan);
        policy.applyUpdates({rules("replaceRules","projectSettings","allow","Write","/replacement/**")},c);
        CTEST_COMPARE(read(path)["permissions"].toObject()["deny"].toArray(),QJsonArray{"Write(/guard/**)"});
        CTEST_CHECK(QFileInfo(path).permissions().testFlag(QFileDevice::ReadOwner));
    }
    void runtimeRulesCliOverridesAndModesAreIsolated() {
        Fixture f;a::SettingsPermissionPolicy policy(f.options,{{"Write(native/**)",a::PermissionBehavior::Allow}});const auto one=f.context(),two=f.context("two");
        CTEST_COMPARE(decide(policy,one,"native/out"),a::PermissionBehavior::Allow);
        policy.applyUpdates({rules("addRules","session","allow","Write","/one/**"),rules("removeRules","cliArg","allow","Write","native/**")},one);
        CTEST_COMPARE(decide(policy,one,"one/out"),a::PermissionBehavior::Allow);CTEST_COMPARE(decide(policy,two,"one/out"),a::PermissionBehavior::Ask);
        CTEST_COMPARE(decide(policy,one,"native/out"),a::PermissionBehavior::Ask);CTEST_COMPARE(decide(policy,two,"native/out"),a::PermissionBehavior::Allow);
        policy.applyUpdates({rules("replaceRules","cliArg","deny","Write","/one/**")},one);
        CTEST_COMPARE(decide(policy,one,"one/out"),a::PermissionBehavior::Deny);
        policy.applyUpdates({mode("session","plan")},one);CTEST_COMPARE(policy.describe(one)["mode"],"plan");CTEST_COMPARE(policy.describe(two)["mode"],"default");
        CTEST_CHECK(!QFileInfo::exists(f.work+"/.claude/settings.json"));
        CTEST_COMPARE(decide(a::SettingsPermissionPolicy(f.options),one,"one/out"),a::PermissionBehavior::Ask);
    }
    void directoriesRetainBindingsAcrossSessionsAndCanBeRemovedPerSession() {
        Fixture f;const auto outside=f.root.filePath("outside"),other=f.root.filePath("other"),link=f.root.filePath("link");QDir().mkpath(outside);QDir().mkpath(other);
        CTEST_CHECK(createNativeTestLink(outside,link));f.options.additionalDirectories={link};
        a::SettingsPermissionPolicy policy(f.options);auto one=f.context(),two=f.context("two");
        CTEST_CHECK(policy.workingDirectories(one).contains(outside));
        policy.applyUpdates({dirs("removeDirectories","session",link)},one);
        CTEST_CHECK(!policy.workingDirectories(one).contains(outside));CTEST_CHECK(policy.workingDirectories(two).contains(outside));
        policy.applyUpdates({dirs("addDirectories","session",link)},one);CTEST_CHECK(policy.workingDirectories(one).contains(outside));
        policy.inheritSession(one,f.context("child"));
        CTEST_CHECK(removeNativeTestLink(link));CTEST_CHECK(createNativeTestLink(other,link));
        CTEST_CHECK(!policy.workingDirectories(f.context("child")).contains(other));CTEST_CHECK(!policy.workingDirectories(two).contains(other));
        policy.applyUpdates({dirs("addDirectories","session",link)},one);CTEST_CHECK(policy.workingDirectories(one).contains(other));
        CTEST_CHECK(!policy.workingDirectories(f.context("child")).contains(other));
        policy.applyUpdates({dirs("addDirectories","localSettings",other)},one);
        CTEST_CHECK(a::SettingsPermissionPolicy(f.options).workingDirectories(two).contains(other));
        policy.applyUpdates({dirs("removeDirectories","localSettings",other)},one);
        CTEST_CHECK(!read(f.work+"/.claude/settings.local.json")["permissions"].toObject()["additionalDirectories"].toArray().contains(other));
    }
    void invalidBatchAndManagedPolicyCannotSilentlyGrant() {
        Fixture f;f.options.enabledSources={"local"};a::SettingsPermissionPolicy policy(f.options);const auto c=f.context();
        CTEST_THROWS(Error,policy.applyUpdates({rules("addRules","localSettings","allow","Write"),rules("addRules","userSettings","allow","Write")},c));
        CTEST_CHECK(!QFileInfo::exists(f.work+"/.claude/settings.local.json"));
        CTEST_THROWS(Error,policy.applyUpdates({rules("addRules","session","allow","Bad(Name")},c));
        CTEST_COMPARE(decide(policy,c,"out"),a::PermissionBehavior::Ask);
        save(f.options.managedDirectory+"/managed-settings.json",{{"allowManagedPermissionRulesOnly",true},{"permissions",QJsonObject{
            {"deny",QJsonArray{"Write(/guard/**)"}},{"disableBypassPermissionsMode","disable"}}}});
        CTEST_THROWS(Error,policy.applyUpdates({mode("session","bypassPermissions")},c));
        policy.applyUpdates({rules("addRules","session","allow","Write")},c);
        CTEST_COMPARE(decide(policy,c,"out"),a::PermissionBehavior::Ask);CTEST_COMPARE(decide(policy,c,"guard/out"),a::PermissionBehavior::Deny);
        CancellationToken token;token.cancel();auto cancelled=c;cancelled.cancellation=token;
        CTEST_THROWS(Error,policy.applyUpdates({mode("localSettings","plan")},cancelled));
        CTEST_CHECK(!QFileInfo::exists(f.work+"/.claude/settings.local.json"));
    }
    void ruleRemovalNormalizesWholeToolAliasesAndEscapesContent() {
        Fixture f;const auto path=f.work+"/.claude/settings.local.json";
        save(path,{{"permissions",QJsonObject{{"allow",QJsonArray{"Bash(*)","Write()"}}}}});a::SettingsPermissionPolicy policy(f.options);
        policy.applyUpdates({rules("removeRules","localSettings","allow","Bash"),rules("removeRules","localSettings","allow","Write"),
            rules("addRules","localSettings","ask","Bash","python -c 'print(1)'" )},f.context());
        const auto p=read(path)["permissions"].toObject();CTEST_CHECK(p["allow"].toArray().isEmpty());
        CTEST_COMPARE(p["ask"].toArray().first(),"Bash(python -c 'print\\(1\\)')");
    }
    void concurrentWritersDoNotLoseRulesAndLinksAreRejected() {
        Fixture f;std::vector<std::future<void>> jobs;std::barrier start(8);
        for(int i=0;i<8;++i)jobs.push_back(std::async(std::launch::async,[&,i]{a::SettingsPermissionPolicy p(f.options);
            start.arrive_and_wait();
            p.applyUpdates({rules("addRules","localSettings","allow","Write","/path"+QString::number(i)+"/**")},f.context());}));
        for(auto& job:jobs)job.get();const auto path=f.work+"/.claude/settings.local.json";
        CTEST_COMPARE(read(path)["permissions"].toObject()["allow"].toArray().size(),8);
        const auto external=f.root.filePath("protected.json");save(external,{{"untouched",true}});CTEST_CHECK(QFile::remove(path));CTEST_CHECK(createNativeTestLink(external,path));
        a::SettingsPermissionPolicy p(f.options);CTEST_THROWS(Error,p.applyUpdates({mode("localSettings","plan")},f.context()));
        CTEST_COMPARE(read(external),(QJsonObject{{"untouched",true}}));
    }
    void clearAndForkInheritTrustedRuntimeStateAndCapacityIsBounded() {
        Fixture f;auto policy=std::make_shared<a::SettingsPermissionPolicy>(f.options);
        a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=f.root.filePath("sessions");options.compaction.automatic=false;
        a::Engine engine(std::make_shared<Model>(),std::make_shared<a::ToolRegistry>(),policy,options);
        const auto old=engine.createSession("fixture",f.work).id;
        policy->applyUpdates({rules("addRules","session","allow","Write","/approved/**")},f.context(old));
        const auto fork=engine.forkSession(old).id;CTEST_COMPARE(decide(*policy,f.context(fork),"approved/out"),a::PermissionBehavior::Allow);
        const auto cleared=engine.clearSession(old);CTEST_CHECK(cleared["complete"].toBool());const auto next=cleared["session_id"].toString();
        CTEST_COMPARE(decide(*policy,f.context(next),"approved/out"),a::PermissionBehavior::Allow);
        policy->applyUpdates({rules("removeRules","session","allow","Write","/approved/**")},f.context(next));
        CTEST_COMPARE(decide(*policy,f.context(old),"approved/out"),a::PermissionBehavior::Allow);CTEST_COMPARE(decide(*policy,f.context(next),"approved/out"),a::PermissionBehavior::Ask);
        f.options.maxRuntimeSessions=1;a::SettingsPermissionPolicy tiny(f.options);tiny.applyUpdates({mode("session","plan")},f.context());
        CTEST_THROWS(Error,tiny.applyUpdates({mode("session","plan")},f.context("two")));
        tiny.forgetSession(f.context());tiny.applyUpdates({mode("session","plan")},f.context("two"));
        CTEST_COMPARE(tiny.describe(f.context("two"))["mode"],"plan");
    }
    void batchValidationKeepsFilesAndRuntimeUnchanged() {
        Fixture f;const auto path=f.work+"/.claude/settings.local.json";
        save(path,{{"theme","KEEP"}});f.options.maxFileBytes=1024;a::SettingsPermissionPolicy policy(f.options);
        CTEST_THROWS(Error,policy.applyUpdates({mode("session","plan"),
            rules("addRules","localSettings","allow","Write",QString(1100,'x'))},f.context()));
        CTEST_COMPARE(read(path),(QJsonObject{{"theme","KEEP"}}));CTEST_COMPARE(policy.describe(f.context())["mode"],"default");
    }
    void changedInputIsDeniedAfterTheApprovedSettingsArePublished() {
        Fixture f;auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,f.work);
        auto policy=std::make_shared<a::SettingsPermissionPolicy>(f.options);a::ToolRunnerOptions options;
        options.permissionResponse=[](const auto&,const auto&,const auto&){a::PermissionResponse response;response.behavior=a::PermissionBehavior::Allow;
            response.updatedArguments=QJsonObject{{"path","blocked.txt"},{"content","NO"}};
            response.updatedPermissions={rules("addRules","localSettings","deny","Write","/blocked.txt")};return response;};
        const auto result=a::ToolRunner(registry,policy,options).run({"write","Write",{{"path","original.txt"},{"content","ORIGINAL"}}},f.context());
        CTEST_CHECK(result.isError);CTEST_CHECK(!QFileInfo::exists(f.work+"/blocked.txt"));CTEST_CHECK(!QFileInfo::exists(f.work+"/original.txt"));
        CTEST_COMPARE(decide(a::SettingsPermissionPolicy(f.options),f.context(),"blocked.txt"),a::PermissionBehavior::Deny);
    }
    void lockedFileTimesOutOrCancelsWithoutPublishing() {
#ifdef Q_OS_UNIX
        Fixture f;const auto path=f.work+"/.claude/settings.local.json";save(path,{{"theme","KEEP"}});
        const auto lockPath=f.work+"/.claude/.settings.local.json.iillm-permissions.lock";
        struct Lock {int fd=-1;~Lock(){if(fd>=0)::close(fd);}} lock;
        lock.fd=::open(QFile::encodeName(lockPath).constData(),O_RDWR|O_CREAT|O_CLOEXEC,0600);CTEST_CHECK(lock.fd>=0);CTEST_CHECK(::flock(lock.fd,LOCK_EX|LOCK_NB)==0);
        f.options.updateLockTimeoutMs=30;a::SettingsPermissionPolicy timeout(f.options);
        try {timeout.applyUpdates({mode("localSettings","plan")},f.context());agent_ctest::fail(__FILE__, __LINE__, "Expected lock timeout");}
        catch(const Error& error){CTEST_COMPARE(error.code(),ErrorCode::Timeout);}
        f.options.updateLockTimeoutMs=5000;a::SettingsPermissionPolicy cancelled(f.options);auto context=f.context();
        auto job=std::async(std::launch::async,[&]{try{cancelled.applyUpdates({mode("localSettings","plan")},context);return ErrorCode::None;}catch(const Error& error){return error.code();}});
        std::this_thread::sleep_for(std::chrono::milliseconds(30));context.cancellation.cancel();CTEST_COMPARE(job.get(),ErrorCode::Cancelled);
        CTEST_COMPARE(read(path),(QJsonObject{{"theme","KEEP"}}));CTEST_CHECK(::flock(lock.fd,LOCK_UN)==0);
        timeout.applyUpdates({mode("localSettings","plan")},f.context());CTEST_COMPARE(timeout.describe(f.context())["mode"],"plan");
#else
        agent_ctest::skip("POSIX descriptor lock fixture");
#endif
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) { std::cerr << "Expected a case name" << std::endl; return 1; }
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    PermissionUpdatesTests tests;
    runner.run("approvalAppliesDirectoryGrantBeforePreparingChangedInput", [&] { tests.approvalAppliesDirectoryGrantBeforePreparingChangedInput(); });
    runner.run("approvalRemovalDropsCachedRootsAndRejectsSuppliedSnapshots", [&] { tests.approvalRemovalDropsCachedRootsAndRejectsSuppliedSnapshots(); });
    runner.run("persistentUpdatesPreserveUnrelatedSettingsAndSurviveRestart", [&] { tests.persistentUpdatesPreserveUnrelatedSettingsAndSurviveRestart(); });
    runner.run("runtimeRulesCliOverridesAndModesAreIsolated", [&] { tests.runtimeRulesCliOverridesAndModesAreIsolated(); });
    runner.run("directoriesRetainBindingsAcrossSessionsAndCanBeRemovedPerSession", [&] { tests.directoriesRetainBindingsAcrossSessionsAndCanBeRemovedPerSession(); });
    runner.run("invalidBatchAndManagedPolicyCannotSilentlyGrant", [&] { tests.invalidBatchAndManagedPolicyCannotSilentlyGrant(); });
    runner.run("ruleRemovalNormalizesWholeToolAliasesAndEscapesContent", [&] { tests.ruleRemovalNormalizesWholeToolAliasesAndEscapesContent(); });
    runner.run("concurrentWritersDoNotLoseRulesAndLinksAreRejected", [&] { tests.concurrentWritersDoNotLoseRulesAndLinksAreRejected(); });
    runner.run("clearAndForkInheritTrustedRuntimeStateAndCapacityIsBounded", [&] { tests.clearAndForkInheritTrustedRuntimeStateAndCapacityIsBounded(); });
    runner.run("batchValidationKeepsFilesAndRuntimeUnchanged", [&] { tests.batchValidationKeepsFilesAndRuntimeUnchanged(); });
    runner.run("changedInputIsDeniedAfterTheApprovedSettingsArePublished", [&] { tests.changedInputIsDeniedAfterTheApprovedSettingsArePublished(); });
    runner.run("lockedFileTimesOutOrCancelsWithoutPublishing", [&] { tests.lockedFileTimesOutOrCancelsWithoutPublishing(); });
    return runner.result();
}

#include "native_link.h"
#include "iiMultimodalAgent/agent/PermissionSettings.h"
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QSet>
#include "CTestSupport.h"
#include <QCoreApplication>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
void put(const QString& path,const QByteArray& bytes) {
    if(!QDir().mkpath(QFileInfo(path).absolutePath()))throw std::runtime_error("mkdir fixture");
    QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("write fixture");
}
void settings(const QString& root,const QJsonArray& directories) {
    put(root+"/.claude/settings.json",QJsonDocument(QJsonObject{{"permissions",QJsonObject{
        {"additionalDirectories",directories},{"allow",QJsonArray{"Write","Edit"}},{"defaultMode","dontAsk"}}}}).toJson());
}
}
class WorkingDirectoriesTests final:public QObject {
public:
    void directorySourcesValidationAndLimits() {
        QTemporaryDir base;const auto work=base.path()+"/work",extra=base.path()+"/shared",home=base.path()+"/home";
        CTEST_CHECK(QDir().mkpath(extra));CTEST_CHECK(QDir().mkpath(home));put(base.path()+"/file","file");
        settings(work,{"../shared","../shared/","../missing","../file",""});
        a::PermissionSettingsOptions options;options.workingDirectory=work;options.homeDirectory=home;options.additionalDirectories={"~/"};
        const auto snapshot=a::SettingsPermissionPolicy(options).snapshot();
        CTEST_CHECK(snapshot.workingDirectories.contains(extra));CTEST_CHECK(snapshot.workingDirectories.contains(home));
        CTEST_COMPARE(snapshot.additionalDirectories.size(),6);CTEST_CHECK(!snapshot.unsupportedFeatures.contains("permissions.additionalDirectories"));
        QSet<QString> statuses;for(const auto& value:snapshot.additionalDirectories)statuses.insert(value.toObject()["status"].toString());
        for(const auto& status:{"active","already_covered","not_found","not_directory","empty"})CTEST_CHECK(statuses.contains(status));
        options.maxDirectories=1;CTEST_THROWS(Error,a::SettingsPermissionPolicy(options).snapshot());
        options.maxDirectories=128;options.homeDirectory.clear();CTEST_THROWS(Error,a::SettingsPermissionPolicy(options).snapshot());
        options.additionalDirectories.clear();CancellationToken token;token.cancel();
        CTEST_THROWS(Error,a::SettingsPermissionPolicy(options).snapshot(token));
    }
    void configuredDirectorySupportsReadWriteEditAndRevocation() {
        QTemporaryDir base;const auto work=base.path()+"/work",extra=base.path()+"/shared";
        CTEST_CHECK(QDir().mkpath(extra));settings(work,{"../shared"});put(extra+"/before.txt","old value");
        a::PermissionSettingsOptions options;options.workingDirectory=work;
        auto policy=std::make_shared<a::SettingsPermissionPolicy>(options);
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work);
        a::ToolRunner runner(registry,policy);a::ToolContext context{"session","run",work};
        auto result=runner.run({"r","Read",{{"path","../shared/before.txt"}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(result.text,QString("old value"));
        result=runner.run({"e","Edit",{{"path","../shared/before.txt"},{"old_string","old"},{"new_string","new"}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
        result=runner.run({"w","Write",{{"path",extra+"/created.txt"},{"content","created"}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
        QFile file(extra+"/before.txt");CTEST_CHECK(file.open(QIODevice::ReadOnly));CTEST_COMPARE(file.readAll(),QByteArray("new value"));
        settings(work,{});
        CTEST_CHECK(runner.run({"r2","Read",{{"path",extra+"/before.txt"}}},context).isError);
        CTEST_CHECK(runner.run({"w2","Write",{{"path",extra+"/revoked.txt"},{"content","forbidden"}}},context).isError);
        CTEST_CHECK(!QFileInfo::exists(extra+"/revoked.txt"));
    }
    void explicitPathRulesAndShellRedirectsUseTheAddedRoot() {
        QTemporaryDir base;const auto work=base.path()+"/work",extra=base.path()+"/shared";
        CTEST_CHECK(QDir().mkpath(extra));settings(work,{"../shared"});
        a::PermissionSettingsOptions options;options.workingDirectory=work;
        options.inlineSettings={{"permissions",QJsonObject{{"allow",QJsonArray{"Bash(printf:*)"}},
            {"deny",QJsonArray{"Edit(/"+extra+"/blocked.txt)"}}}}};
        auto policy=std::make_shared<a::SettingsPermissionPolicy>(options);
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work);
        a::ToolRunner runner(registry,policy);a::ToolContext context{"session","run",work};
        CTEST_CHECK(runner.run({"deny","Write",{{"path",extra+"/blocked.txt"},{"content","forbidden"}}},context).isError);
        CTEST_CHECK(!QFileInfo::exists(extra+"/blocked.txt"));
#ifdef Q_OS_UNIX
        auto result=runner.run({"bash","Bash",{{"command","printf value > ../shared/shell.txt"}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
        QFile file(extra+"/shell.txt");CTEST_CHECK(file.open(QIODevice::ReadOnly));CTEST_COMPARE(file.readAll(),QByteArray("value"));
        CTEST_CHECK(runner.run({"deny-shell","Bash",{{"command","printf value > ../shared/blocked.txt"}}},context).isError);
        CTEST_CHECK(!QFileInfo::exists(extra+"/blocked.txt"));
#endif
    }
    void searchCanSelectAnAddedDirectory() {
        QTemporaryDir base;const auto work=base.path()+"/work",extra=base.path()+"/shared";
        CTEST_CHECK(QDir().mkpath(extra));settings(work,{"../shared"});put(extra+"/a.txt","MATCH");put(extra+"/b.bin","ignore");
        a::PermissionSettingsOptions options;options.workingDirectory=work;
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work);
        a::ToolRunner runner(registry,std::make_shared<a::SettingsPermissionPolicy>(options));a::ToolContext context{"session","run",work};
        auto result=runner.run({"g","Glob",{{"path","../shared"},{"pattern","*.txt"}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(result.data["paths"].toArray(),QJsonArray{"a.txt"});
        result=runner.run({"s","Grep",{{"path","../shared"},{"pattern","MATCH"}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(result.data["matches"].toArray().size(),1);
        CTEST_CHECK(runner.run({"outside","Grep",{{"path",".."},{"pattern","MATCH"}}},context).isError);
    }
    void callerCannotInjectDirectoriesAndPrivatePathsRemainHidden() {
        QTemporaryDir base;const auto work=base.path()+"/work",extra=base.path()+"/shared",secret=extra+"/private";
        settings(work,{"../shared"});put(secret+"/token.txt","SECRET");put(extra+"/public.txt","PUBLIC");
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work,{},QStringList{secret});
        a::ToolContext context{"session","run",work};context.workingDirectories={base.path()};
        a::ToolRunner fixed(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
        CTEST_CHECK(fixed.run({"injected","Read",{{"path",extra+"/public.txt"}}},context).isError);
        a::PermissionSettingsOptions options;options.workingDirectory=work;
        a::ToolRunner runner(registry,std::make_shared<a::SettingsPermissionPolicy>(options));
        CTEST_CHECK(runner.run({"private-read","Read",{{"path",secret+"/token.txt"}}},context).isError);
        CTEST_CHECK(runner.run({"private-write","Write",{{"path",secret+"/created.txt"},{"content","forbidden"}}},context).isError);
        const auto privateCase=extra+"/PRIVATE";
        if(QFileInfo::exists(privateCase)) {
            CTEST_CHECK(runner.run({"private-case-read","Read",{{"path",privateCase+"/TOKEN.TXT"}}},context).isError);
            CTEST_CHECK(runner.run({"private-case-write","Write",{{"path",privateCase+"/created.txt"},{"content","forbidden"}}},context).isError);
            CTEST_CHECK(!QFileInfo::exists(secret+"/created.txt"));
        }
        auto result=runner.run({"search","Grep",{{"path",extra},{"pattern","SECRET"}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_CHECK(result.data["matches"].toArray().isEmpty());
        result=runner.run({"list","Glob",{{"path",extra},{"pattern","**"}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(result.data["paths"].toArray(),QJsonArray{"public.txt"});
        context.artifactsDirectory=secret+"/own";put(context.artifactsDirectory+"/output.txt","OWN");
        result=runner.run({"artifact","Read",{{"path",context.artifactsDirectory+"/output.txt"}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(result.text,QString("OWN"));
        CTEST_CHECK(runner.run({"artifact-write","Write",{{"path",context.artifactsDirectory+"/other.txt"},{"content","forbidden"}}},context).isError);
    }
    void symlinkAuthorityStaysBoundUntilSettingsChange() {
#ifndef Q_OS_UNIX
        agent_ctest::skip("Requires POSIX symlink behavior");
#endif
        QTemporaryDir base;const auto work=base.path()+"/work",first=base.path()+"/first",second=base.path()+"/second",link=base.path()+"/link";
        put(first+"/data.txt","FIRST");put(second+"/data.txt","SECOND");CTEST_CHECK(createNativeTestLink(first,link));settings(work,{"../link"});
        a::PermissionSettingsOptions options;options.workingDirectory=work;
        auto policy=std::make_shared<a::SettingsPermissionPolicy>(options);
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work);
        a::ToolRunner runner(registry,policy);a::ToolContext context{"session","run",work};
        auto result=runner.run({"first","Read",{{"path",link+"/data.txt"}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(result.text,QString("FIRST"));
        CTEST_CHECK(removeNativeTestLink(link));CTEST_CHECK(createNativeTestLink(second,link));
        CTEST_CHECK(runner.run({"changed","Read",{{"path",link+"/data.txt"}}},context).isError);
        result=runner.run({"bound","Read",{{"path",first+"/data.txt"}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(result.text,QString("FIRST"));
        QFile f(work+"/.claude/settings.json");CTEST_CHECK(f.open(QIODevice::Append));CTEST_COMPARE(f.write("\n"),qint64(1));f.close();
        result=runner.run({"new-source","Read",{{"path",link+"/data.txt"}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(result.text,QString("SECOND"));
    }
    void duplicateCliGrantKeepsItsOriginalBindingAfterSettingsRemoval() {
#ifndef Q_OS_UNIX
        agent_ctest::skip("Requires POSIX symlink behavior");
#endif
        QTemporaryDir base;const auto work=base.path()+"/work",first=base.path()+"/first",second=base.path()+"/second",link=base.path()+"/shared";
        put(first+"/data.txt","FIRST");put(second+"/data.txt","SECOND");CTEST_CHECK(createNativeTestLink(first,link));
        settings(work,{"../shared"});a::PermissionSettingsOptions options;options.workingDirectory=work;options.additionalDirectories={"../shared"};
        auto policy=std::make_shared<a::SettingsPermissionPolicy>(options);
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work);
        a::ToolRunner runner(registry,policy);a::ToolContext context{"session","run",work};
        auto result=runner.run({"original","Read",{{"path","../shared/data.txt"}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(result.text,QString("FIRST"));
        CTEST_CHECK(removeNativeTestLink(link));CTEST_CHECK(createNativeTestLink(second,link));
        QFile file(work+"/.claude/settings.json");CTEST_CHECK(file.open(QIODevice::Append));CTEST_COMPARE(file.write("\n"),qint64(1));file.close();
        result=runner.run({"changed-settings","Read",{{"path","../shared/data.txt"}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(result.text,QString("SECOND"));
        result=runner.run({"still-cli-bound","Read",{{"path",first+"/data.txt"}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(result.text,QString("FIRST"));
        settings(work,{});
        CTEST_CHECK(runner.run({"retargeted","Read",{{"path","../shared/data.txt"}}},context).isError);
        result=runner.run({"original-cli-target","Read",{{"path",first+"/data.txt"}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(result.text,QString("FIRST"));
        const auto snapshot=policy->snapshot();CTEST_COMPARE(snapshot.additionalDirectories.size(),1);
        CTEST_COMPARE(snapshot.additionalDirectories.first().toObject()["source"].toString(),QString("cliArg"));
        CTEST_COMPARE(snapshot.additionalDirectories.first().toObject()["status"].toString(),QString("target_changed"));
    }
    void preparedExternalTargetCannotBeReplacedBeforeExecution() {
#ifndef Q_OS_UNIX
        agent_ctest::skip("Requires POSIX symlink behavior");
#endif
        QTemporaryDir base;const auto work=base.path()+"/work",extra=base.path()+"/shared",other=base.path()+"/other";
        CTEST_CHECK(QDir().mkpath(extra));CTEST_CHECK(QDir().mkpath(other));settings(work,{"../shared"});
        a::PermissionSettingsOptions options;options.workingDirectory=work;
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work);
        a::ToolRunner runner(registry,std::make_shared<a::SettingsPermissionPolicy>(options));a::ToolContext context{"session","run",work};
        const auto result=runner.run({"write","Write",{{"path","../shared/target.txt"},{"content","forbidden"}}},context,[&](const a::Event& event) {
            if(event.kind==a::EventKind::ToolStarted) {
                if(!QDir().rename(extra,base.path()+"/original")||!createNativeTestLink(other,extra))throw std::runtime_error("replace fixture path");
            }
        });
        CTEST_CHECK(result.isError);CTEST_CHECK(!QFileInfo::exists(other+"/target.txt"));CTEST_CHECK(!QFileInfo::exists(base.path()+"/original/target.txt"));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) { std::cerr << "Expected a case name or --offline" << std::endl; return 1; }
    const std::string_view selection = argc == 2 ? argv[1] : "";
    const bool offline = selection == "--offline";
    agent_ctest::CTestRunner runner(offline ? std::string_view{} : selection);
    WorkingDirectoriesTests tests;
    runner.run("directorySourcesValidationAndLimits", [&] { tests.directorySourcesValidationAndLimits(); });
    runner.run("configuredDirectorySupportsReadWriteEditAndRevocation", [&] { tests.configuredDirectorySupportsReadWriteEditAndRevocation(); });
    if (!offline)
        runner.run("explicitPathRulesAndShellRedirectsUseTheAddedRoot", [&] { tests.explicitPathRulesAndShellRedirectsUseTheAddedRoot(); });
    if (!offline)
        runner.run("searchCanSelectAnAddedDirectory", [&] { tests.searchCanSelectAnAddedDirectory(); });
    if (!offline)
        runner.run("callerCannotInjectDirectoriesAndPrivatePathsRemainHidden", [&] { tests.callerCannotInjectDirectoriesAndPrivatePathsRemainHidden(); });
    runner.run("symlinkAuthorityStaysBoundUntilSettingsChange", [&] { tests.symlinkAuthorityStaysBoundUntilSettingsChange(); });
    runner.run("duplicateCliGrantKeepsItsOriginalBindingAfterSettingsRemoval", [&] { tests.duplicateCliGrantKeepsItsOriginalBindingAfterSettingsRemoval(); });
    runner.run("preparedExternalTargetCannotBeReplacedBeforeExecution", [&] { tests.preparedExternalTargetCannotBeReplacedBeforeExecution(); });
    return runner.result();
}

#include "native_link.h"
#include "iiMultimodalAgent/agent/PermissionSettings.h"
#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include "CTestSupport.h"
#include <QtCore/QCoreApplication>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a = iiMultimodalAgent::agent;
namespace {
void save(const QString& path, const QJsonObject& object) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) throw std::runtime_error("fixture directory");
    QFile f(path); if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(object).toJson()) < 0) throw std::runtime_error("fixture settings");
}
QJsonObject permissions(const char* behavior, const QStringList& rules) {
    return {{"permissions", QJsonObject{{behavior, QJsonArray::fromStringList(rules)}}}};
}
a::ToolDefinition writeTool() { return {"Write", "write", {{"type", "object"}}}; }
}
class PermissionSettingsTests final {
public:
    void matchesIndependentNodeIgnoreCorpus() {
        QFile file(QDir(QString::fromUtf8(IIMULTIMODAL_PERMISSION_TEST_DATA_DIR)).filePath("permission_settings_patterns.json"));CTEST_CHECK(file.open(QIODevice::ReadOnly));
        const auto data=QJsonDocument::fromJson(file.readAll()).object();CTEST_CHECK(data["cases"].toArray().size()>=40);
        QTemporaryDir root;a::ToolContext c;c.workingDirectory=root.path();
        a::ToolDefinition read{"Read","read",{{"type","object"}},{},true};
        for(const auto& value:data["cases"].toArray()) {
            const auto row=value.toObject();QStringList rules;for(const auto& pattern:row["patterns"].toArray())rules.append("Read("+pattern.toString()+")");
            a::PermissionSettingsOptions options;options.workingDirectory=root.path();options.inlineSettings=permissions("deny",rules);
            a::SettingsPermissionPolicy policy(options);
            const bool denied=policy.decide(read,{{"path",row["path"]}},c).behavior==a::PermissionBehavior::Deny;
            CTEST_CHECK_MESSAGE(denied==row["denied"].toBool(),qPrintable(QJsonDocument(row).toJson(QJsonDocument::Compact)));
        }
    }
    void mergesSourcesAndRefreshesRemoval() {
        QTemporaryDir root; const auto work = root.filePath("work"); CTEST_CHECK(QDir().mkpath(work));
        a::PermissionSettingsOptions o; o.workingDirectory = work; o.userDirectory = root.filePath("user"); o.managedDirectory = root.filePath("managed");
        o.flagFiles = {root.filePath("flag.json")}; o.inlineSettings = permissions("ask", {"Bash(rm:*)"});
        save(o.userDirectory+"/settings.json", permissions("allow", {"Write(/user/**)"}));
        save(work+"/.claude/settings.json", permissions("allow", {"Write(/src/**)"}));
        save(work+"/.claude/settings.local.json", permissions("deny", {"Write(/src/private/**)"}));
        save(o.flagFiles[0], permissions("allow", {"Skill(review)"}));
        save(o.managedDirectory+"/managed-settings.json", {{"permissions", QJsonObject{{"defaultMode", "dontAsk"}}}});
        a::SettingsPermissionPolicy policy(o); a::ToolContext c; c.workingDirectory = work;
        CTEST_COMPARE(policy.decide(writeTool(), {{"path", "src/code.cpp"}}, c).behavior, a::PermissionBehavior::Allow);
        CTEST_COMPARE(policy.decide(writeTool(), {{"path", "src/private/secret"}}, c).behavior, a::PermissionBehavior::Deny);
        auto snapshot = policy.snapshot(); CTEST_COMPARE(snapshot.mode, a::PermissionMode::DontAsk); CTEST_COMPARE(snapshot.sources.size(), 6);
        CTEST_CHECK(snapshot.toJson().contains("rules"));
        save(work+"/.claude/settings.local.json", {});
        CTEST_COMPARE(policy.decide(writeTool(), {{"path", "src/private/secret"}}, c).behavior, a::PermissionBehavior::Allow);
        save(work+"/.claude/settings.json", {});
        CTEST_COMPARE(policy.decide(writeTool(), {{"path", "src/code.cpp"}}, c).behavior, a::PermissionBehavior::Deny);
    }
    void managedDropInsAndSourceSelection() {
        QTemporaryDir root; const auto work = root.filePath("work"); CTEST_CHECK(QDir().mkpath(work));
        a::PermissionSettingsOptions o; o.workingDirectory=work; o.managedDirectory=root.filePath("managed"); o.enabledSources={};
        save(work+"/.claude/settings.json", permissions("deny", {"Write"}));
        save(o.managedDirectory+"/managed-settings.json", {{"allowManagedPermissionRulesOnly",true},{"permissions",QJsonObject{{"defaultMode","dontAsk"}}}});
        save(o.managedDirectory+"/managed-settings.d/10-base.json", permissions("allow",{"Write(/managed/**)"}));
        save(o.managedDirectory+"/managed-settings.d/20-mode.json", {{"permissions",QJsonObject{{"defaultMode","plan"}}}});
        save(o.managedDirectory+"/managed-settings.d/.hidden.json", permissions("deny",{"Write"}));
        a::SettingsPermissionPolicy policy(o, {{"Write",a::PermissionBehavior::Allow}});
        auto s=policy.snapshot(); CTEST_CHECK(s.managedRulesOnly); CTEST_COMPARE(s.mode,a::PermissionMode::Plan); CTEST_COMPARE(s.sources.size(),3);
        CTEST_COMPARE(s.rules.first().source,QString("policySettings"));
        for(const auto& rule:s.rules)CTEST_CHECK(rule.source=="policySettings"||rule.source=="host.settingsProtection");
        o.modeOverride=a::PermissionMode::Default; a::SettingsPermissionPolicy normal(o,{{"Write",a::PermissionBehavior::Allow}});
        a::ToolContext c; c.workingDirectory=work;c.allowedTools={"Skill(review)"};
        CTEST_COMPARE(normal.decide(writeTool(),{{"path","elsewhere"}},c).behavior,a::PermissionBehavior::Ask);
        CTEST_COMPARE(normal.decide(writeTool(),{{"path","managed/file"}},c).behavior,a::PermissionBehavior::Allow);
    }
    void anchoredBasenamesNegationAndSourceRoots() {
        QTemporaryDir root; const auto work=root.filePath("work"); CTEST_CHECK(QDir().mkpath(work+"/sub"));
        a::PermissionSettingsOptions o; o.workingDirectory=work; o.homeDirectory=root.path();o.userDirectory=work+"/config";
        save(o.userDirectory+"/settings.json", permissions("allow",{"Write(/only/**)"}));
        save(work+"/.claude/settings.json",permissions("deny",{"Read(.env)","Read([Ss]ecret?.txt)","Read(/locked/*)","Read(!/locked/public.txt)"}));
        a::SettingsPermissionPolicy p(o);a::ToolContext c;c.workingDirectory=work;
        a::ToolDefinition read{"Read","read",{{"type","object"}},{},true};
        for(const auto& path:{".env","sub/.env","sub/Secret1.txt","locked/private.txt"})
            CTEST_COMPARE(p.decide(read,{{"path",path}},c).behavior,a::PermissionBehavior::Deny);
        CTEST_COMPARE(p.decide(read,{{"path","locked/public.txt"}},c).behavior,a::PermissionBehavior::Allow);
        CTEST_COMPARE(p.decide(writeTool(),{{"path","config/only/file"}},c).behavior,a::PermissionBehavior::Allow);
        CTEST_COMPARE(p.decide(writeTool(),{{"path","only/file"}},c).behavior,a::PermissionBehavior::Ask);
    }
    void disabledBypassAndInvalidSettingsDoNotGrant() {
        QTemporaryDir root; a::PermissionSettingsOptions o;o.workingDirectory=root.path();o.modeOverride=a::PermissionMode::Bypass;
        save(root.filePath(".claude/settings.json"),{{"permissions",QJsonObject{{"defaultMode","plan"},{"disableBypassPermissionsMode","disable"}}}});
        a::SettingsPermissionPolicy p(o);CTEST_COMPARE(p.snapshot().mode,a::PermissionMode::Plan);
        save(root.filePath(".claude/settings.json"),{{"permissions",QJsonObject{{"deny",QJsonArray{42}}}}});
        CTEST_THROWS(Error,p.snapshot());
        a::ToolContext c;c.workingDirectory=root.path();CTEST_THROWS(Error,p.decide(writeTool(),{{"path","file"}},c));
    }
    void unicodeDirectoryParentsAndDuplicateNegations() {
        QTemporaryDir root;a::PermissionSettingsOptions o;o.workingDirectory=root.path();
        o.inlineSettings=permissions("deny",{"Read(비밀?.txt)","Read([가-힣]문서.txt)","Read(ÉTUDE.txt)","Read(/private/)","Read(!/private/visible.txt)",
            "Read(/flat/*)","Read(!/flat/public.txt)","Read(/flat/*)"});
        a::SettingsPermissionPolicy p(o);a::ToolContext c;c.workingDirectory=root.path();
        a::ToolDefinition read{"Read","read",{{"type","object"}},{},true};
        for(const auto& path:{"sub/비밀가.txt","한문서.txt","étude.txt","private/visible.txt","flat/secret"})
            CTEST_COMPARE(p.decide(read,{{"path",path}},c).behavior,a::PermissionBehavior::Deny);
        for(const auto& path:{"비밀가나.txt","public.txt","flat/public.txt"})
            CTEST_COMPARE(p.decide(read,{{"path",path}},c).behavior,a::PermissionBehavior::Allow);
    }
    void filesCannotSilentlyDropRulesOrRewriteAuthority() {
        QTemporaryDir root;a::PermissionSettingsOptions o;o.workingDirectory=root.path();o.fallbackMode=a::PermissionMode::DontAsk;
        const auto path=root.filePath(".claude/settings.json");save(path,permissions("allow",{"Edit"}));
        a::SettingsPermissionPolicy p(o);a::ToolContext c;c.workingDirectory=root.path();
        CTEST_COMPARE(p.decide(writeTool(),{{"path","ordinary.txt"}},c).behavior,a::PermissionBehavior::Allow);
        CTEST_COMPARE(p.decide(writeTool(),{{"path",".claude/./settings.json"}},c).behavior,a::PermissionBehavior::Deny);
        CTEST_COMPARE(p.decide(writeTool(),{{"path","nested/.claude/settings.local.json"}},c).behavior,a::PermissionBehavior::Deny);
        CTEST_CHECK(QFile::remove(path));CTEST_CHECK(createNativeTestLink(root.filePath("missing.json"),path));CTEST_THROWS(Error,p.snapshot());
        CTEST_CHECK(QFile::remove(path));CTEST_CHECK(QDir().mkdir(path));CTEST_THROWS(Error,p.snapshot());CTEST_CHECK(QDir().rmdir(path));
        save(path,permissions("deny",{"Write"}));
#ifdef Q_OS_UNIX
        CTEST_CHECK(QFile::setPermissions(path,{}));CTEST_THROWS(Error,p.snapshot());CTEST_CHECK(QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner));
#endif
        o.maxFileBytes=16;a::SettingsPermissionPolicy tiny(o);CTEST_THROWS(Error,tiny.snapshot());
        o.maxFileBytes=1024;o.inlineSettings={{"permissions",QJsonObject{{"unknownDirectories",QJsonArray{root.filePath("outside")}}}}};
        a::SettingsPermissionPolicy unsupported(o);CTEST_CHECK(unsupported.snapshot().unsupportedFeatures.contains("permissions.unknownDirectories"));
        CTEST_THROWS(Error,unsupported.decide(writeTool(),{{"path","file"}},c));
    }
    void finalComponentDirectoryMetadataIsRefreshedBeforeMatching() {
        QTemporaryDir root;a::PermissionSettingsOptions o;o.workingDirectory=root.path();
        o.inlineSettings=permissions("deny",{"Read(target/)"});
        a::SettingsPermissionPolicy policy(o);a::ToolContext context;context.workingDirectory=root.path();
        a::ToolDefinition read{"Read","read",{{"type","object"}},{},true};
        const QJsonObject input{{"path","target"}};
        CTEST_CHECK(QDir(root.path()).mkdir("target"));
        CTEST_COMPARE(policy.decide(read,input,context).behavior,a::PermissionBehavior::Deny);
        CTEST_CHECK(QDir(root.path()).rmdir("target"));
        QFile file(root.filePath("target"));CTEST_CHECK(file.open(QIODevice::WriteOnly));file.close();
        CTEST_COMPARE(policy.decide(read,input,context).behavior,a::PermissionBehavior::Allow);
        CTEST_CHECK(file.remove());
        CTEST_COMPARE(policy.decide(read,input,context).behavior,a::PermissionBehavior::Allow);
    }
    void hostilePatternsAreBoundedAndCancellationPropagates() {
        QTemporaryDir root;a::PermissionSettingsOptions o;o.workingDirectory=root.path();
        o.inlineSettings=permissions("deny",{"Read("+QString("*a").repeated(160)+"b)"});
        a::SettingsPermissionPolicy p(o);a::ToolContext c;c.workingDirectory=root.path();
        a::ToolDefinition read{"Read","read",{{"type","object"}},{},true};
        QElapsedTimer elapsed;elapsed.start();
        try { p.decide(read,{{"path",QString(240,'a')+'b'}},c);agent_ctest::fail(__FILE__, __LINE__, "Pattern exhaustion must fail the decision"); }
        catch(const Error& error) { CTEST_COMPARE(error.code(),ErrorCode::ResourceLimit); }
        CTEST_CHECK(elapsed.elapsed()<2000);
        c.cancellation.cancel();
        try { p.decide(read,{{"path","file"}},c);agent_ctest::fail(__FILE__, __LINE__, "Expected cancellation"); }
        catch(const Error& error) { CTEST_COMPARE(error.code(),ErrorCode::Cancelled); }
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner;
    PermissionSettingsTests tests;
    runner.run("matchesIndependentNodeIgnoreCorpus", [&] { tests.matchesIndependentNodeIgnoreCorpus(); });
    runner.run("mergesSourcesAndRefreshesRemoval", [&] { tests.mergesSourcesAndRefreshesRemoval(); });
    runner.run("managedDropInsAndSourceSelection", [&] { tests.managedDropInsAndSourceSelection(); });
    runner.run("anchoredBasenamesNegationAndSourceRoots", [&] { tests.anchoredBasenamesNegationAndSourceRoots(); });
    runner.run("disabledBypassAndInvalidSettingsDoNotGrant", [&] { tests.disabledBypassAndInvalidSettingsDoNotGrant(); });
    runner.run("unicodeDirectoryParentsAndDuplicateNegations", [&] { tests.unicodeDirectoryParentsAndDuplicateNegations(); });
    runner.run("filesCannotSilentlyDropRulesOrRewriteAuthority", [&] { tests.filesCannotSilentlyDropRulesOrRewriteAuthority(); });
    runner.run("finalComponentDirectoryMetadataIsRefreshedBeforeMatching", [&] { tests.finalComponentDirectoryMetadataIsRefreshedBeforeMatching(); });
    runner.run("hostilePatternsAreBoundedAndCancellationPropagates", [&] { tests.hostilePatternsAreBoundedAndCancellationPropagates(); });
    return runner.result();
}

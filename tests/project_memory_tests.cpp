#include "native_link.h"
#include "iiMultimodalAgent/agent/ProjectMemory.h"
#include "iiMultimodalAgent/agent/Engine.h"
#include "CTestSupport.h"
#include <QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QCryptographicHash>
#include <future>

using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
void put(const QString& path,const QByteArray& content) {
    CTEST_CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));QFile f(path);CTEST_CHECK(f.open(QIODevice::WriteOnly));CTEST_COMPARE(f.write(content),content.size());
}
QString digest(const QByteArray& bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
struct Fixture {
    QTemporaryDir root;
    QString workspace=root.filePath("workspace"),state=root.filePath("state");
    a::ProjectMemoryOptions options{root.filePath("state/memory")};
    std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::ProjectMemory> memory;
    std::shared_ptr<const a::PermissionPolicy> policy=std::make_shared<a::RulePolicy>(a::PermissionMode::AcceptEdits);
    a::ToolContext context;
    Fixture() {
        QDir().mkpath(workspace);memory=std::make_shared<a::ProjectMemory>(options);
        a::registerWorkspaceTools(*registry,workspace,{},QStringList{state});memory->directory(workspace);
        memory->bindWorkspaceTools(*registry);registry->add(memory->forgetTool(false));
        context={"session","run",workspace,root.filePath("artifacts")};
    }
    QString path(const QString& name="MEMORY.md") {return QDir(memory->directory(workspace)).filePath(name);}
    a::ToolResult run(const QString& name,const QJsonObject& args) {return a::ToolRunner(registry,policy).run({"call",name,args},context);}
};
}
class ProjectMemoryTests final {
public:
    void workspaceNotesPersistAcrossOwnersWithoutExposingAdjacentState() {
        Fixture f;put(f.root.filePath("state/secret.txt"),"secret");
        auto written=f.run("Write",{{"path",f.path()},{"content","Preferred language: Korean"}});CTEST_CHECK_MESSAGE(!written.isError,qPrintable(written.text));
        a::ProjectMemory reopened(f.options);auto value=reopened.snapshot(f.workspace);
        CTEST_COMPARE(value["index"].toString(),"Preferred language: Korean");CTEST_COMPARE(value["index_sha256"].toString(),digest("Preferred language: Korean"));
        CTEST_CHECK(f.run("Read",{{"path",f.root.filePath("state/secret.txt")}}).isError);
        const auto other=f.root.filePath("other-workspace");QDir().mkpath(other);
        CTEST_CHECK(reopened.directory(other)!=f.memory->directory(f.workspace));
        CTEST_CHECK(f.run("Read",{{"path",QDir(reopened.directory(other)).filePath("MEMORY.md")}}).isError);
        put(f.root.filePath("workspace/ordinary.md"),"workspace file");
        CTEST_COMPARE(f.run("Read",{{"path","ordinary.md"}}).text,"workspace file");
    }
    void indexHasUtf8AndLineBoundsAndNeverBecomesAnInstruction() {
        Fixture f;auto options=f.options;options.maxIndexLines=2;options.maxIndexBytes=7;
        a::ProjectMemory memory(options);put(f.path(),QString::fromUtf8("한글입니다\nsecond\nthird").toUtf8());
        const auto state=memory.snapshot(f.workspace);CTEST_CHECK(state["index_truncated"].toBool());
        CTEST_COMPARE(state["index"].toString(),QString::fromUtf8("한글"));CTEST_CHECK(state["index"].toString().toUtf8().size()<=7);
        const auto message=memory.message(f.workspace);CTEST_COMPARE(message.role,a::MessageRole::User);
        CTEST_CHECK(message.text.contains("untrusted"));CTEST_CHECK(message.text.contains("truncated"));
        CTEST_CHECK(message.metadata.contains("iilocal.project_memory"));
        const auto index=memory.index(f.workspace);CTEST_COMPARE(index["index"],state["index"]);CTEST_COMPARE(index["index_sha256"],state["index_sha256"]);
        CTEST_CHECK(index["index_truncated"].toBool());CTEST_CHECK(!index.contains("files"));
    }
    void topicDiscoverySearchAndInvalidFilesHaveExplicitBoundaries() {
        Fixture f;
        put(f.path("preferences.md"),"---\nname: Communication\ndescription: Korean responses\ntype: user\n---\nKeep replies concise.");
        put(f.path("build/testing.md"),"---\nname: Tests\ndescription: Build validation\ntype: feedback\n---\nBuild and run tests.");
        put(f.path("broken.md"),"---\nname: first\nname: second\n---\nInvalid YAML metadata");
        const auto result=f.memory->snapshot(f.workspace,"korean");
        CTEST_COMPARE(result["files"].toArray().size(),1);CTEST_COMPARE(result["files"].toArray().first().toObject()["path"].toString(),"preferences.md");
        CTEST_COMPARE(result["files"].toArray().first().toObject()["type"].toString(),"user");
        CTEST_COMPARE(result["diagnostics"].toArray().size(),1);
        CTEST_CHECK(f.run("Glob",{{"path",f.memory->directory(f.workspace)},{"pattern","**/*.md"}}).text.contains("testing.md"));
        CTEST_CHECK(f.run("Grep",{{"path",f.memory->directory(f.workspace)},{"pattern","Build and run"}}).text.contains("testing.md"));
    }
    void recursiveGlobFindsRootAndDeeperTopics() {
        Fixture f;
        for(const auto& name:QStringList{"preferences.md","build/testing.md","build/deep/note.md","한글.md","upper.MD"})put(f.path(name),"topic");
        auto matches=[&](const QString& pattern){return f.run("Glob",{{"path",f.memory->directory(f.workspace)},{"pattern",pattern}}).data["paths"].toArray();};
        CTEST_COMPARE(matches("**/*.md"),(QJsonArray{"build/deep/note.md","build/testing.md","preferences.md","한글.md"}));
        CTEST_COMPARE(matches("*.md"),(QJsonArray{"preferences.md","한글.md"}));
        CTEST_COMPARE(matches("build/**/*.md"),(QJsonArray{"build/deep/note.md","build/testing.md"}));
        CTEST_COMPARE(matches("**/preferences.md"),QJsonArray{"preferences.md"});
        CTEST_COMPARE(matches("**/한?.md"),QJsonArray{"한글.md"});
        CTEST_COMPARE(matches("**/[np]*.md"),(QJsonArray{"build/deep/note.md","preferences.md"}));
        CTEST_COMPARE(matches("**/*.MD"),QJsonArray{"upper.MD"});
        put(f.root.filePath("workspace/preferences.md"),"ordinary");put(f.root.filePath("workspace/deep/note.md"),"ordinary");
        CTEST_COMPARE(f.run("Glob",{{"pattern","**/*.md"}}).data["paths"].toArray(),(QJsonArray{"deep/note.md","preferences.md"}));
    }
    void changedNotesNeedFreshReadsAndForgetNeedsCurrentHash() {
        Fixture f;const auto path=f.path("feedback.md");put(path,"before");
        CTEST_CHECK(f.run("Write",{{"path",path},{"content","after"}}).isError);
        auto observed=f.run("Read",{{"path",path}});CTEST_CHECK(!observed.isError);CTEST_COMPARE(observed.data["sha256"].toString(),digest("before"));put(path,"external");
        CTEST_CHECK(f.run("Edit",{{"path",path},{"old_string","before"},{"new_string","after"}}).isError);
        CTEST_CHECK(f.run("MemoryForget",{{"path",path},{"sha256",digest("before")}}).isError);CTEST_CHECK(QFileInfo::exists(path));
        auto result=f.run("MemoryForget",{{"path",path},{"sha256",digest("external")}});CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
        CTEST_CHECK(!QFileInfo::exists(path));CTEST_CHECK(result.data["removed"].toBool());CTEST_CHECK(!result.data["backup_path"].toString().isEmpty());
        CTEST_CHECK(f.run("MemoryForget",{{"path",f.root.filePath("workspace/ordinary.md")},{"sha256",digest("external")}}).isError);
    }
    void concurrentOwnersCannotOverwriteEachOthersReadSnapshot() {
        Fixture f;const auto path=f.path("concurrent.md");put(path,"initial");
        a::ProjectMemory second(f.options);auto registry=std::make_shared<a::ToolRegistry>();
        a::registerWorkspaceTools(*registry,f.workspace,{},QStringList{f.state});second.bindWorkspaceTools(*registry);
        a::ToolContext context=f.context;context.sessionId="second";
        a::ToolRunner runner(registry,f.policy);
        CTEST_CHECK(!f.run("Read",{{"path",path}}).isError);CTEST_CHECK(!runner.run({"read","Read",{{"path",path}}},context).isError);
        auto one=std::async(std::launch::async,[&]{return f.run("Write",{{"path",path},{"content","one"}});});
        auto two=std::async(std::launch::async,[&]{return runner.run({"write","Write",{{"path",path},{"content","two"}}},context);});
        const auto a=one.get(),b=two.get();CTEST_COMPARE(int(!a.isError)+int(!b.isError),1);
        QFile file(path);CTEST_CHECK(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();CTEST_CHECK(bytes=="one"||bytes=="two");
    }
    void replacingTheMemoryRootCannotRedirectAnApprovedWrite() {
        Fixture f;const auto directory=f.memory->directory(f.workspace),outside=f.root.filePath("outside");QDir().mkpath(outside);
        f.policy=std::make_shared<a::RulePolicy>();a::ToolRunnerOptions options;
        options.permission=[&](const auto&,const auto&,const auto&) {
            const auto moved=directory+"-moved";
            if(!QDir().rename(directory,moved)||!createNativeTestLink(outside,directory))throw std::runtime_error("Cannot set up root replacement");
            return true;
        };
        auto result=a::ToolRunner(f.registry,f.policy,options).run({"write","Write",{{"path",QDir(directory).filePath("new.md")},{"content","unsafe"}}},f.context);
        CTEST_CHECK(result.isError);CTEST_CHECK(!QFileInfo::exists(QDir(outside).filePath("new.md")));
    }
    void publicWorkspaceSearchCannotDiscoverOtherProjectMemory() {
        QTemporaryDir root;const auto workspace=root.filePath("work"),other=root.filePath("other");
        QDir().mkpath(workspace);QDir().mkpath(other);
        a::ProjectMemory memory({QDir(workspace).filePath("state/memory")});
        const auto own=memory.directory(workspace),foreign=memory.directory(other);
        put(QDir(own).filePath("MEMORY.md"),"OWN_NOTE");put(QDir(foreign).filePath("MEMORY.md"),"FOREIGN_NOTE_943");
        auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,workspace);memory.bindWorkspaceTools(*registry);
        a::ToolRunner runner(registry,std::make_shared<a::RulePolicy>());a::ToolContext context{"owner","run",workspace};
        auto result=runner.run({"search","Grep",{{"path","."},{"pattern","FOREIGN_NOTE_943"}}},context);
        CTEST_CHECK(!result.text.contains("FOREIGN_NOTE_943"));
        CTEST_CHECK(runner.run({"read","Read",{{"path",QDir(foreign).filePath("MEMORY.md")}}},context).isError);
        CTEST_CHECK(createNativeTestLink(QDir(foreign).filePath("MEMORY.md"),QDir(workspace).filePath("alias.md")));
        CTEST_CHECK(runner.run({"alias","Read",{{"path","alias.md"}}},context).isError);
        CTEST_COMPARE(runner.run({"own","Read",{{"path",QDir(own).filePath("MEMORY.md")}}},context).text,"OWN_NOTE");
    }
    void symlinksCancellationLimitsAndExplicitPolicyDenialsApply() {
        Fixture f;put(f.root.filePath("outside.md"),"private");
        CTEST_CHECK(createNativeTestLink(f.root.filePath("outside.md"),f.path("link.md")));
        CTEST_CHECK(f.run("Read",{{"path",f.path("link.md")}}).isError);
        CTEST_CHECK(f.run("Write",{{"path",f.path("link.md")},{"content","overwrite"}}).isError);
        CTEST_CHECK(!f.memory->snapshot(f.workspace)["diagnostics"].toArray().isEmpty());
        CancellationToken cancelled;cancelled.cancel();CTEST_THROWS(Error,f.memory->snapshot(f.workspace,{},cancelled));
        auto options=f.options;options.maxFiles=1;a::ProjectMemory bounded(options);
        put(f.path("a.md"),"alpha");put(f.path("b.md"),"beta");CTEST_CHECK(bounded.snapshot(f.workspace)["catalog_truncated"].toBool());
        f.policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"Write",a::PermissionBehavior::Deny},{"MemoryForget",a::PermissionBehavior::Deny}});
        CTEST_CHECK(f.run("Write",{{"path",f.path("new.md")},{"content","new"}}).isError);CTEST_CHECK(!QFileInfo::exists(f.path("new.md")));
        CTEST_CHECK(f.run("MemoryForget",{{"path",f.path("a.md")},{"sha256",digest("alpha")}}).isError);CTEST_CHECK(QFileInfo::exists(f.path("a.md")));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner;
    ProjectMemoryTests tests;
    runner.run("workspaceNotesPersistAcrossOwnersWithoutExposingAdjacentState", [&] { tests.workspaceNotesPersistAcrossOwnersWithoutExposingAdjacentState(); });
    runner.run("indexHasUtf8AndLineBoundsAndNeverBecomesAnInstruction", [&] { tests.indexHasUtf8AndLineBoundsAndNeverBecomesAnInstruction(); });
    runner.run("topicDiscoverySearchAndInvalidFilesHaveExplicitBoundaries", [&] { tests.topicDiscoverySearchAndInvalidFilesHaveExplicitBoundaries(); });
    runner.run("recursiveGlobFindsRootAndDeeperTopics", [&] { tests.recursiveGlobFindsRootAndDeeperTopics(); });
    runner.run("changedNotesNeedFreshReadsAndForgetNeedsCurrentHash", [&] { tests.changedNotesNeedFreshReadsAndForgetNeedsCurrentHash(); });
    runner.run("concurrentOwnersCannotOverwriteEachOthersReadSnapshot", [&] { tests.concurrentOwnersCannotOverwriteEachOthersReadSnapshot(); });
    runner.run("replacingTheMemoryRootCannotRedirectAnApprovedWrite", [&] { tests.replacingTheMemoryRootCannotRedirectAnApprovedWrite(); });
    runner.run("publicWorkspaceSearchCannotDiscoverOtherProjectMemory", [&] { tests.publicWorkspaceSearchCannotDiscoverOtherProjectMemory(); });
    runner.run("symlinksCancellationLimitsAndExplicitPolicyDenialsApply", [&] { tests.symlinksCancellationLimitsAndExplicitPolicyDenialsApply(); });
    return runner.result();
}

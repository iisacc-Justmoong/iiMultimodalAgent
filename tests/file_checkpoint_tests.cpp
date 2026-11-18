#include "native_link.h"
#include <iiMultimodalAgent/agent/FileCheckpoints.h>
#include <iiMultimodalAgent/agent/Engine.h>
#include <iiMultimodalAgent/agent/Api.h>
#include <iiMultimodalAgent/agent/McpServer.h>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <QtCore/QCryptographicHash>
#include "CTestSupport.h"
#include <QtCore/QCoreApplication>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
QString uuid(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
void write(const QString& path,const QByteArray& bytes){QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("Fixture write failed");}
QByteArray read(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))throw std::runtime_error("Fixture read failed");return f.readAll();}
struct Fixture {
    QTemporaryDir root{QDir::temp().filePath("checkpoint-test-XXXXXX")};
    QString work=root.filePath("work"),state=root.filePath("state"),path=work+"/file.txt";
    a::ToolContext context{uuid(),{},work};a::FileCheckpoints history{state};
    Fixture(){if(!QDir().mkpath(work))throw std::runtime_error("Fixture mkdir failed");write(path,"original\r\n");}
    QString checkpoint(){context.fileCheckpointId=uuid();history.checkpoint(context.fileCheckpointId,context);return context.fileCheckpointId;}
    void edit(const QString& target,const QByteArray& bytes){std::optional<QByteArray> before;if(QFileInfo::exists(target))before=read(target);history.track(target,before,context);write(target,bytes);}
};
class EditModel:public a::Model {public:
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken&,const TextCallback&)override{
        const auto last=request.messages.last();
        if(last.role==a::MessageRole::User)return {{},{{uuid(),"Read",{{"path","file.txt"}}}}, {1,1}};
        if(last.role==a::MessageRole::Tool&&last.metadata["name"]=="Read")return {{},{{uuid(),"Write",{{"path","file.txt"},{"content","edited"}}}}, {1,1}};
        // Tool messages carry their call identity; find the last assistant tool.
        for(auto it=request.messages.crbegin();it!=request.messages.crend();++it)if(!it->toolCalls.isEmpty()){
            if(it->toolCalls.last().name=="Read")return {{},{{uuid(),"Write",{{"path","file.txt"},{"content","edited"}}}}, {1,1}};break;}
        return {"DONE",{}, {1,1}};
    }
};
a::EngineOptions options(QString state){a::EngineOptions o{.decision={.enabled=false}};o.sessionsDirectory=state;o.fileCheckpointsEnabled=true;o.skills.enabled=false;o.projectContext.enabled=false;o.toolSearch.enabled=false;o.compaction.automatic=false;return o;}
std::shared_ptr<a::ToolRegistry> tools(const QString& path){auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,path);return registry;}
}
class FileCheckpointTests {
public:
 void restoresFirstVersionWithinATurnAndLaterVersionsAcrossTurns(){
    Fixture f;const auto original=read(f.path);const auto first=f.checkpoint();f.edit(f.path,"one");f.edit(f.path,"two");
    const auto second=f.checkpoint();f.edit(f.path,"three");
    const auto preview=f.history.rewind(first,true,f.context);CTEST_COMPARE(preview["filesChanged"].toArray().size(),1);CTEST_COMPARE(read(f.path),"three");
    a::FileCheckpoints resumed(f.state);CTEST_CHECK(resumed.rewind(first,false,f.context)["complete"].toBool());CTEST_COMPARE(read(f.path),original);
    CTEST_CHECK(resumed.rewind(second,false,f.context)["complete"].toBool());CTEST_COMPARE(read(f.path),"two");
    CTEST_CHECK(resumed.rewind(second,true,f.context)["filesChanged"].toArray().isEmpty());
 }
 void lateTrackedFilesUseFirstBackupAndEmptyNewFilesAreDeleted(){
    Fixture f;const auto first=f.checkpoint();const auto second=f.checkpoint();const auto created=f.work+"/empty";
    f.edit(f.path,"modified");f.edit(created,{});
    const auto preview=f.history.rewind(first,true,f.context);CTEST_COMPARE(preview["filesChanged"].toArray().size(),2);
    const auto result=f.history.rewind(first,false,f.context);CTEST_CHECK(result["complete"].toBool());CTEST_CHECK(!QFileInfo::exists(created));CTEST_COMPARE(read(f.path),"original\r\n");
    CTEST_CHECK(f.history.rewind(second,false,f.context)["complete"].toBool());CTEST_CHECK(!QFileInfo::exists(created));
 }
 void deletedTrackedFilesAndPermissionsAreRestored(){
    Fixture f;CTEST_CHECK(QFile::setPermissions(f.path,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
    const auto mode=QFileInfo(f.path).permissions();const auto first=f.checkpoint();f.edit(f.path,"new");CTEST_CHECK(QFile::remove(f.path));const auto deleted=f.checkpoint();
    CTEST_CHECK(f.history.rewind(first,false,f.context)["complete"].toBool());CTEST_COMPARE(read(f.path),"original\r\n");CTEST_COMPARE(QFileInfo(f.path).permissions(),mode);
    CTEST_CHECK(f.history.rewind(deleted,false,f.context)["complete"].toBool());CTEST_CHECK(!QFileInfo::exists(f.path));
 }
 void preflightCorruptionAndChangedPreviewNeverOverwriteOtherFiles(){
    Fixture f;const auto first=f.checkpoint();f.edit(f.path,"new");const auto next=f.work+"/second";f.edit(next,"new file");
    const auto preview=f.history.rewind(first,true,f.context);write(f.path,"external");
    CTEST_THROWS(Error,f.history.rewind(first,false,f.context,preview["fingerprint"].toString()));CTEST_COMPARE(read(next),"new file");CTEST_COMPARE(read(f.path),"external");
    const auto digest=QString::fromLatin1(QCryptographicHash::hash("original\r\n",QCryptographicHash::Sha256).toHex());write(f.state+'/'+f.context.sessionId+'/'+digest+".blob","corrupt");
    CTEST_THROWS(Error,f.history.rewind(first,false,f.context));CTEST_COMPARE(read(next),"new file");
 }
 void rejectsSymlinksPrivateRootsForeignScopesAndTamperedManifest(){
    Fixture f;const auto first=f.checkpoint();f.edit(f.path,"new");
    auto denied=f.context;denied.protectedPaths={f.path};CTEST_THROWS(Error,f.history.rewind(first,false,denied));
    auto revision=f.context;revision.workspaceRevision=1;CTEST_THROWS(Error,f.history.rewind(first,false,revision));
    const auto outside=f.root.filePath("outside");write(outside,"outside");CTEST_CHECK(QFile::remove(f.path));CTEST_CHECK(createNativeTestLink(outside,f.path));
    CTEST_THROWS(Error,f.history.rewind(first,false,f.context));CTEST_COMPARE(read(outside),"outside");
    const auto journal=f.state+'/'+f.context.sessionId+"/state.json";auto data=QJsonDocument::fromJson(read(journal)).object();data["snapshots"]=42;write(journal,QJsonDocument(data).toJson());
    CTEST_THROWS(Error,f.history.list(f.context));
 }
 void retentionKeepsFirstBackupsAndBoundsSnapshotCount(){
    Fixture f;QString retained;for(int i=0;i<104;++i){const auto id=f.checkpoint();if(i==4)retained=id;f.edit(f.path,QByteArray::number(i));}
    const auto list=f.history.list(f.context);CTEST_COMPARE(list["snapshots"].toArray().size(),100);CTEST_COMPARE(list["sequence"].toInt(),104);
    CTEST_CHECK(f.history.rewind(retained,false,f.context)["complete"].toBool());CTEST_COMPARE(read(f.path),"3");
    const auto blobs=QDir(f.state+'/'+f.context.sessionId).entryList({"*.blob"});CTEST_CHECK(blobs.size()<=101);
 }
 void cancellationAndOversizeFailBeforeMutation(){
    Fixture f;const auto first=f.checkpoint();f.edit(f.path,"new");auto cancelled=f.context;cancelled.cancellation.cancel();
    CTEST_THROWS(Error,f.history.rewind(first,false,cancelled));CTEST_COMPARE(read(f.path),"new");
    write(f.path,QByteArray(1024*1024+1,'x'));CTEST_THROWS(Error,f.history.rewind(first,false,f.context));CTEST_COMPARE(QFileInfo(f.path).size(),1024*1024+1);
 }
 void partialIoFailureReportsRestoredFilesPrecisely(){
    Fixture f;const auto nested=f.work+"/z";CTEST_CHECK(QDir().mkpath(nested));const auto path=nested+"/file";write(path,"second original");
    const auto first=f.checkpoint();f.edit(f.path,"new");f.edit(path,"second new");const auto mode=QFileInfo(nested).permissions();
    #ifdef Q_OS_WIN
    HANDLE blocked=CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    CTEST_CHECK(blocked!=INVALID_HANDLE_VALUE);
#else
    CTEST_CHECK(QFile::setPermissions(nested,QFileDevice::ReadOwner|QFileDevice::ExeOwner));
#endif
    const auto result=f.history.rewind(first,false,f.context);const auto readable=read(path);
    #ifdef Q_OS_WIN
    CloseHandle(blocked);
#else
    const bool permissionsRestored=QFile::setPermissions(nested,mode);CTEST_CHECK(permissionsRestored);
#endif
    CTEST_CHECK(!result["complete"].toBool());CTEST_COMPARE(result["filesRestored"].toArray(),QJsonArray{f.path});CTEST_COMPARE(result["errors"].toArray().size(),1);
    CTEST_COMPARE(read(f.path),"original\r\n");CTEST_COMPARE(readable,"second new");
 }
 void engineCreatesMessageCheckpointsAndExpiresReadsAfterRewind(){
    Fixture f;auto registry=tools(f.work);auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);auto model=std::make_shared<EditModel>();
    a::Engine engine(model,registry,policy,options(f.root.filePath("sessions")));const auto id=engine.createSession("fixture",f.work).id;
    a::RunRequest request;request.sessionId=id;request.prompt="change";const auto run=engine.run(request).result.get();CTEST_CHECK_MESSAGE(run.status==a::RunStatus::Completed,qPrintable(run.errorMessage));CTEST_COMPARE(read(f.path),"edited");
    const auto messages=engine.session(id).messages;const auto first=messages.first().id;CTEST_COMPARE(engine.fileCheckpoints(id)["snapshots"].toArray().first().toObject()["message_id"].toString(),first);
    const auto dry=engine.rewindFiles(id,first,true);CTEST_CHECK_MESSAGE(!dry.isError,qPrintable(dry.text));CTEST_COMPARE(read(f.path),"edited");
    const auto result=engine.rewindFiles(id,first);CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(read(f.path),"original\r\n");CTEST_COMPARE(engine.session(id).messages.size(),messages.size());
    a::ToolContext context{id,{},f.work};auto guard=engine.bindWorkspaceContext(context,true);
    CTEST_CHECK(a::ToolRunner(registry,policy).run({uuid(),"Write",{{"path","file.txt"},{"content","stale read"}}},context).isError);
    a::ToolContext independent{id,{},f.work};CTEST_CHECK(!engine.bindWorkspaceContext(independent,false));
    CTEST_THROWS(Error,engine.checkpointFiles(id));guard.reset();const auto fork=engine.forkSession(id);
    CTEST_COMPARE(engine.fileCheckpoints(fork.id)["snapshots"],engine.fileCheckpoints(id)["snapshots"]);
 }
 void nativeControlsTrackNewFilesAndRespectCurrentWriteDeny(){
    Fixture f;auto registry=tools(f.work);auto allow=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);auto model=std::make_shared<EditModel>();const auto config=options(f.root.filePath("sessions"));QString id,point;
    {a::Engine engine(model,registry,allow,config);id=engine.createSession("fixture",f.work).id;point=engine.checkpointFiles(id)["message_id"].toString();
        a::ToolContext context{id,{},f.work};auto guard=engine.bindWorkspaceContext(context,true);
        CTEST_CHECK(!a::ToolRunner(registry,allow).run({uuid(),"Write",{{"path","created"},{"content","new"}}},context).isError);}
    auto deny=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,QList<a::PermissionRule>{{"Write(*)",a::PermissionBehavior::Deny}});
    a::Engine denied(model,registry,deny,config);CTEST_CHECK(denied.rewindFiles(id,point).isError);CTEST_COMPARE(read(f.work+"/created"),"new");
    a::Engine resumed(model,registry,allow,config);CTEST_CHECK(!resumed.rewindFiles(id,point).isError);CTEST_CHECK(!QFileInfo::exists(f.work+"/created"));
 }
 void approvalCannotRestoreIntoARevokedAdditionalDirectory(){
    class Policy:public a::PermissionPolicy {public:QString extra;bool granted=true;
        a::PermissionDecision decide(const a::ToolDefinition& tool,const QJsonObject&,const a::ToolContext&)const override{return {tool.name=="RewindFiles"?a::PermissionBehavior::Ask:a::PermissionBehavior::Allow,{}};}
        QStringList workingDirectories(const a::ToolContext& c)const override{return granted?QStringList{c.workingDirectory,extra}:QStringList{c.workingDirectory};}
    };
    Fixture f;auto registry=tools(f.work);auto policy=std::make_shared<Policy>();policy->extra=f.root.filePath("extra");CTEST_CHECK(QDir().mkpath(policy->extra));
    auto config=options(f.root.filePath("sessions"));config.permission=[policy](const auto&,const auto&,const auto&){policy->granted=false;return true;};
    a::Engine engine(std::make_shared<EditModel>(),registry,policy,config);const auto id=engine.createSession("fixture",f.work).id;const auto point=engine.checkpointFiles(id)["message_id"].toString();
    const auto path=policy->extra+"/file";{a::ToolContext context{id,{},f.work};auto guard=engine.bindWorkspaceContext(context,true);
        CTEST_CHECK(!a::ToolRunner(registry,policy).run({uuid(),"Write",{{"path",path},{"content","preserve"}}},context).isError);}
    const auto result=engine.rewindFiles(id,point);CTEST_CHECK(result.isError);CTEST_COMPARE(read(path),"preserve");
 }
 void projectMemoryDoesNotJoinWorkspaceHistory(){
    Fixture f;auto registry=tools(f.work);auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);auto config=options(f.root.filePath("sessions"));
    config.projectMemoryEnabled=true;config.memoryRecall.enabled=false;config.memoryExtraction.enabled=false;
    a::Engine engine(std::make_shared<EditModel>(),registry,policy,config);const auto id=engine.createSession("fixture",f.work).id;
    const auto point=engine.checkpointFiles(id)["message_id"].toString();const auto index=engine.memory(id)["index_path"].toString();
    const auto result=engine.runMemoryTool(id,"Write",{{"path",index},{"content","persistent note"}});CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
    CTEST_COMPARE(engine.fileCheckpoints(id)["total_tracked_files"].toInt(),0);CTEST_CHECK(!engine.rewindFiles(id,point).isError);CTEST_COMPARE(read(index),"persistent note");
 }
 void apiAndMcpBindHistoryToAuthenticatedOwner(){
    Fixture f;auto registry=tools(f.work);auto policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);auto model=std::make_shared<EditModel>();
    a::ApiOptions config{.engine={.decision={.enabled=false}}};config.engine=options({});config.workingDirectory=f.work;config.stateDirectory=f.root.filePath("api");config.clientTokens={{"society",QString(48,'a')},{"dreamscapes",QString(48,'b')}};
    a::Api api(model,registry,policy,config);auto call=[&](QString method,QJsonObject params={},QString token=QString(48,'a')){return api.dispatch(method,params,token).result.get().toObject();};
    CTEST_CHECK(call("agent.info")["file_checkpoints_enabled"].toBool());const auto id=call("agent.sessions.create",{{"model","fixture"}})["session_id"].toString();
    const auto run=call("agent.run",{{"session_id",id},{"prompt","change"}});CTEST_COMPARE(read(f.path),"edited");
    const auto history=call("agent.checkpoints.list",{{"session_id",id}});CTEST_CHECK_MESSAGE(!history["snapshots"].toArray().isEmpty(),QJsonDocument(QJsonObject{{"history",history},{"run",run}}).toJson().constData());
    const auto point=history["snapshots"].toArray().first().toObject()["message_id"].toString();
    const auto child=call("agent.sessions.fork",{{"session_id",id}})["session_id"].toString();CTEST_CHECK(child!=id&&!child.isEmpty());
    CTEST_THROWS(Error,call("agent.sessions.fork",{{"session_id",child}},QString(48,'b')));
    CTEST_CHECK(!call("agent.checkpoints.rewind",{{"session_id",child},{"message_id",point}})["is_error"].toBool());CTEST_COMPARE(read(f.path),"original\r\n");
    CTEST_THROWS(Error,call("agent.checkpoints.list",{{"session_id",id}},QString(48,'b')));
    CTEST_THROWS(Error,call("agent.checkpoints.rewind",{{"session_id",id},{"message_id",point},{"dry_run","false"}}));
    auto engine=std::make_shared<a::Engine>(model,registry,policy,options(f.root.filePath("mcp")));a::McpServerOptions mo;mo.engine=engine;mo.model="fixture";mo.workingDirectory=f.work;
    const auto serverOptions=a::mcpServerOptions(registry,policy,mo);CTEST_CHECK(serverOptions.experimentalCapabilities.contains("iisacc/fileCheckpoints"));CTEST_CHECK(serverOptions.experimentalCapabilities.contains("iisacc/sessionFork"));mcp::ServerSession server(serverOptions);
    int number=0;auto rpc=[&](QString method,QJsonObject params){const int n=++number;server.receive(QJsonObject{{"jsonrpc","2.0"},{"id",n},{"method",method},{"params",params}});
        for(int i=0;i<400;++i)for(const auto& v:server.takeMessages(10))if(v.toObject()["id"]==n)return v.toObject();throw std::runtime_error("MCP timeout");};
    rpc("initialize",{{"protocolVersion","2025-11-25"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","test"},{"version","1"}}}});server.receive(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/initialized"}});
    auto tool=[&](QString name,QJsonObject args={}){const auto response=rpc("tools/call",{{"name",name},{"arguments",args}});if(response.contains("error"))throw std::runtime_error(QJsonDocument(response).toJson().constData());return response["result"].toObject();};
    const auto checkpoint=tool("iiLocalLLM.agent.checkpoints.create")["structuredContent"].toObject()["message_id"].toString();
    CTEST_CHECK(!tool("Write",{{"path","mcp.txt"},{"content","new"}})["isError"].toBool());CTEST_COMPARE(read(f.work+"/mcp.txt"),"new");
    const auto old=tool("iiLocalLLM.agent.session")["structuredContent"].toObject()["session_id"].toString();
    CTEST_CHECK(!tool("Read",{{"path","file.txt"}})["isError"].toBool());
    const auto edited=tool("Write",{{"path","file.txt"},{"content","mcp edit"}});CTEST_CHECK(!edited["isError"].toBool());
    const auto backup=edited["structuredContent"].toObject()["backup_path"].toString();CTEST_CHECK_MESSAGE(backup.contains('/'+old+"/artifacts/"),qPrintable(backup));
    CTEST_CHECK(tool("iiLocalLLM.agent.fork",{{"session_id",id}})["isError"].toBool());
    const auto forked=tool("iiLocalLLM.agent.fork");CTEST_CHECK_MESSAGE(!forked["isError"].toBool(),QJsonDocument(forked).toJson().constData());
    const auto newId=forked["structuredContent"].toObject()["session_id"].toString();CTEST_CHECK(!newId.isEmpty()&&newId!=old);
    CTEST_COMPARE(tool("iiLocalLLM.agent.session")["structuredContent"].toObject()["session_id"].toString(),newId);
    auto copied=backup;copied.replace('/'+old+"/artifacts/",'/'+newId+"/artifacts/");CTEST_COMPARE(read(copied),"original\r\n");
    const auto restored=tool("iiLocalLLM.agent.checkpoints.rewind",{{"message_id",checkpoint}});CTEST_CHECK_MESSAGE(!restored["isError"].toBool(),QJsonDocument(restored).toJson().constData());CTEST_CHECK(!QFileInfo::exists(f.work+"/mcp.txt"));
    CTEST_COMPARE(read(f.path),"original\r\n");
 }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner;
    FileCheckpointTests tests;
    runner.run("restoresFirstVersionWithinATurnAndLaterVersionsAcrossTurns", [&] { tests.restoresFirstVersionWithinATurnAndLaterVersionsAcrossTurns(); });
    runner.run("lateTrackedFilesUseFirstBackupAndEmptyNewFilesAreDeleted", [&] { tests.lateTrackedFilesUseFirstBackupAndEmptyNewFilesAreDeleted(); });
    runner.run("deletedTrackedFilesAndPermissionsAreRestored", [&] { tests.deletedTrackedFilesAndPermissionsAreRestored(); });
    runner.run("preflightCorruptionAndChangedPreviewNeverOverwriteOtherFiles", [&] { tests.preflightCorruptionAndChangedPreviewNeverOverwriteOtherFiles(); });
    runner.run("rejectsSymlinksPrivateRootsForeignScopesAndTamperedManifest", [&] { tests.rejectsSymlinksPrivateRootsForeignScopesAndTamperedManifest(); });
    runner.run("retentionKeepsFirstBackupsAndBoundsSnapshotCount", [&] { tests.retentionKeepsFirstBackupsAndBoundsSnapshotCount(); });
    runner.run("cancellationAndOversizeFailBeforeMutation", [&] { tests.cancellationAndOversizeFailBeforeMutation(); });
    runner.run("partialIoFailureReportsRestoredFilesPrecisely", [&] { tests.partialIoFailureReportsRestoredFilesPrecisely(); });
    runner.run("engineCreatesMessageCheckpointsAndExpiresReadsAfterRewind", [&] { tests.engineCreatesMessageCheckpointsAndExpiresReadsAfterRewind(); });
    runner.run("nativeControlsTrackNewFilesAndRespectCurrentWriteDeny", [&] { tests.nativeControlsTrackNewFilesAndRespectCurrentWriteDeny(); });
    runner.run("approvalCannotRestoreIntoARevokedAdditionalDirectory", [&] { tests.approvalCannotRestoreIntoARevokedAdditionalDirectory(); });
    runner.run("projectMemoryDoesNotJoinWorkspaceHistory", [&] { tests.projectMemoryDoesNotJoinWorkspaceHistory(); });
    runner.run("apiAndMcpBindHistoryToAuthenticatedOwner", [&] { tests.apiAndMcpBindHistoryToAuthenticatedOwner(); });
    return runner.result();
}

#include "native_link.h"
#include <iiMultimodalAgent/agent/Tools.h>
#include <iiMultimodalAgent/agent/PermissionSettings.h>
#include <iiMultimodalAgent/agent/Notebook.h>
#include <iiMultimodalAgent/agent/Engine.h>
#include <iiMultimodalAgent/agent/Api.h>
#include <iiMultimodalAgent/agent/McpServer.h>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QProcess>
#include <QtCore/QUuid>
#include "CTestSupport.h"
#include <QCoreApplication>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
void write(const QString& path,const QByteArray& bytes){QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("Fixture write failed");}
QByteArray read(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))throw std::runtime_error("Fixture read failed");return f.readAll();}
QJsonObject cell(QString type,QString id,QString source){QJsonObject result{{"cell_type",type},{"id",id},{"source",source},{"metadata",QJsonObject{{"custom","preserved"}}}};
    if(type=="code"){result["outputs"]=QJsonArray{QJsonObject{{"output_type","stream"},{"name","stdout"},{"text","old output"}}};result["execution_count"]=9;}return result;}
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("notebook-test-XXXXXX")};QString path=root.filePath("book.ipynb");
    std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass);
    a::ToolContext context{"owner","run",root.path(),root.filePath("artifacts")};
    Fixture(){a::registerWorkspaceTools(*registry,root.path());save({cell("code","first","print('old')"),cell("markdown","cell-0","# Existing")});}
    void save(QJsonArray cells,int minor=5){write(path,QJsonDocument(QJsonObject{{"nbformat",4},{"nbformat_minor",minor},{"metadata",QJsonObject{{"language_info",QJsonObject{{"name","python"}}},{"custom",42}}},{"cells",cells}}).toJson());}
    QJsonObject value()const{return QJsonDocument::fromJson(read(path)).object();}
    a::ToolResult call(QString name,QJsonObject args){return a::ToolRunner(registry,policy).run({"call",name,args},context);}
    a::ToolResult observe(){return call("Read",{{"path",path}});}
    a::ToolResult edit(QJsonObject args){args["notebook_path"]=path;return call("NotebookEdit",args);}
};
class QuietModel:public a::Model {public:a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const TextCallback&)override{return {"DONE",{},{1,1}};}};
a::EngineOptions engineOptions(const QString& state){a::EngineOptions result{.decision={.enabled=false}};result.sessionsDirectory=state;
    result.skills.enabled=false;result.projectContext.enabled=false;result.toolSearch.enabled=false;result.compaction.automatic=false;return result;}
}
class NotebookTests:public QObject {
public:
 void replacementRequiresReadAndPreservesUnrelatedData(){
    Fixture f;const auto original=read(f.path);const QJsonValue untouched=f.value()["cells"].toArray()[1];
    CTEST_CHECK(f.edit({{"cell_id","first"},{"new_source","print('new')"}}).isError);CTEST_COMPARE(read(f.path),original);
    CTEST_CHECK(!f.observe().isError);auto result=f.edit({{"cell_id","first"},{"new_source","print('new')"}});CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
    const auto first=f.value()["cells"].toArray()[0].toObject();CTEST_COMPARE(first["source"].toString(),"print('new')");
    CTEST_CHECK(first["execution_count"].isNull());CTEST_CHECK(first["outputs"].toArray().isEmpty());CTEST_COMPARE(first["metadata"].toObject()["custom"].toString(),"preserved");
    CTEST_COMPARE(f.value()["cells"].toArray()[1],untouched);CTEST_COMPARE(f.value()["metadata"].toObject()["custom"].toInt(),42);
    CTEST_COMPARE(read(result.data["backup_path"].toString()),original);CTEST_COMPARE(result.data["cell_id"].toString(),"first");
    CTEST_CHECK(!f.edit({{"cell_id","first"},{"new_source","print('again')"}}).isError);
 }
 void actualCellIdPrecedesFallbackIndexAndInsertionIsAfter(){
    Fixture f;CTEST_CHECK(!f.observe().isError);auto result=f.edit({{"cell_id","cell-0"},{"new_source","# Target"}});CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
    CTEST_COMPARE(f.value()["cells"].toArray()[0].toObject()["source"].toString(),"print('old')");
    CTEST_COMPARE(f.value()["cells"].toArray()[1].toObject()["source"].toString(),"# Target");CTEST_COMPARE(result.data["cell_type"].toString(),"markdown");
    result=f.edit({{"cell_id","first"},{"new_source","value = 1"},{"cell_type","code"},{"edit_mode","insert"}});CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
    auto inserted=f.value()["cells"].toArray()[1].toObject();CTEST_COMPARE(inserted["source"].toString(),"value = 1");CTEST_CHECK(!inserted["id"].toString().isEmpty());
    CTEST_CHECK(inserted["outputs"].toArray().isEmpty());CTEST_CHECK(inserted["execution_count"].isNull());
    result=f.edit({{"cell_id",inserted["id"]},{"new_source",""},{"edit_mode","delete"}});CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(f.value()["cells"].toArray().size(),2);
 }
 void conversionProducesValidFieldsAndOldFormatsUseIndexSelectors(){
    Fixture f;CTEST_CHECK(!f.observe().isError);auto result=f.edit({{"cell_id","first"},{"new_source","# Text"},{"cell_type","markdown"}});CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
    auto first=f.value()["cells"].toArray()[0].toObject();CTEST_CHECK(!first.contains("outputs"));CTEST_CHECK(!first.contains("execution_count"));
    first["attachments"]=QJsonObject{{"inline.txt",QJsonObject{{"text/plain","attachment"}}}};first.remove("id");f.save({first},4);CTEST_CHECK(!f.observe().isError);
    result=f.edit({{"cell_id","cell-0"},{"new_source","answer = 42"},{"cell_type","code"}});CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
    first=f.value()["cells"].toArray()[0].toObject();CTEST_CHECK(!first.contains("attachments"));CTEST_CHECK(!first.contains("id"));CTEST_CHECK(first["execution_count"].isNull());CTEST_CHECK(first["outputs"].toArray().isEmpty());
    result=f.edit({{"new_source","# First"},{"cell_type","markdown"},{"edit_mode","insert"}});CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
    CTEST_COMPARE(f.value()["cells"].toArray()[0].toObject()["source"].toString(),"# First");
 }
 void largeAttributionHasImmutableBeforeAndAfterFiles(){
    Fixture f;auto value=f.value();auto metadata=value["metadata"].toObject();metadata["large"]=QString(40000,'x');value["metadata"]=metadata;
    write(f.path,QJsonDocument(value).toJson());const auto before=read(f.path);CTEST_CHECK(!f.observe().isError);
    auto result=f.edit({{"cell_id","first"},{"new_source","x = 1"}});CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
    CTEST_CHECK(!result.data["files_inlined"].toBool());CTEST_CHECK(!result.data.contains("original_file"));CTEST_CHECK(!result.data.contains("updated_file"));
    const auto after=read(f.path);const auto snapshot=result.data["updated_file_path"].toString();CTEST_COMPARE(read(snapshot),after);CTEST_COMPARE(read(result.data["backup_path"].toString()),before);
    CTEST_CHECK(!f.edit({{"cell_id","first"},{"new_source","x = 2"}}).isError);CTEST_COMPARE(read(snapshot),after);
 }
 void invalidInputsAndDocumentsNeverWrite(){
    Fixture f;const auto original=read(f.path);CTEST_CHECK(!f.observe().isError);
    for(const QJsonObject args:QList<QJsonObject>{
        {{"new_source","x"}},{{"cell_id","first"}},{{"cell_id","first"},{"new_source",42}},
        {{"cell_id","first"},{"new_source","x"},{"extra",true}},{{"new_source","x"},{"edit_mode","insert"}},
        {{"cell_id","cell-2"},{"new_source","x"}},{{"cell_id","cell-18446744073709551616"},{"new_source","x"}},
        {{"cell_id","first"},{"new_source","x"},{"cell_type","raw"}},{{"cell_id","first"},{"new_source","x"},{"edit_mode","append"}}}){
        CTEST_CHECK(f.edit(args).isError);CTEST_COMPARE(read(f.path),original);
    }
    QList<QByteArray> documents{"[1,2]","{broken",QByteArray("\xff",1)};
    for(int kind=0;kind<9;++kind){auto value=f.value();auto cells=value["cells"].toArray();auto first=cells[0].toObject();
        switch(kind){case 0:value["nbformat"]=5;break;case 1:value["nbformat_minor"]=-1;break;
        case 2:value["metadata"]=1;break;case 3:first.remove("id");break;case 4:first["id"]="cell-0";break;
        case 5:first["id"]="invalid id";break;case 6:first["source"]=QJsonArray{1};break;
        case 7:first["outputs"]=QJsonArray{1};break;case 8:first.remove("execution_count");break;}
        cells[0]=first;value["cells"]=cells;documents.append(QJsonDocument(value).toJson());}
    for(const auto& bytes:documents){write(f.path,bytes);f.observe();CTEST_CHECK(f.edit({{"cell_id","first"},{"new_source","x"}}).isError);CTEST_COMPARE(read(f.path),bytes);}
 }
 void rawCellsArraysAttachmentsAndFuturePayloadsArePreserved(){
    Fixture f;auto raw=cell("raw","raw","raw text");raw["source"]=QJsonArray{"line one\n","line two"};
    raw["attachments"]=QJsonObject{{"image",QJsonObject{{"image/png","payload"}}}};
    const QJsonObject future{{"cell_type","future-cell"},{"id","future"},{"payload",QJsonArray{42,"unknown"}}};
    f.save({cell("code","first","x = 1"),raw,future},6);CTEST_CHECK(!f.observe().isError);
    CTEST_CHECK(!f.edit({{"cell_id","first"},{"new_source","x = 2"}}).isError);
    CTEST_COMPARE(f.value()["cells"].toArray()[1].toObject(),raw);CTEST_COMPARE(f.value()["cells"].toArray()[2].toObject(),future);
    CTEST_CHECK(f.edit({{"cell_id","future"},{"new_source",""},{"edit_mode","delete"}}).isError);
    const auto result=f.edit({{"cell_id","raw"},{"new_source","updated raw"}});CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
    CTEST_COMPARE(result.data["cell_type"].toString(),"raw");CTEST_COMPARE(f.value()["cells"].toArray()[1].toObject()["attachments"],raw["attachments"]);
 }
 void readsAreBoundToContentOwnerAndRevisions(){
    Fixture f;const QJsonObject args{{"cell_id","first"},{"new_source","x"}};
    CTEST_CHECK(!f.call("Read",{{"path",f.path},{"limit",1}}).isError);CTEST_CHECK(f.edit(args).isError);
    CTEST_CHECK(!f.observe().isError);const auto original=read(f.path);write(f.path,original+" ");CTEST_CHECK(f.edit(args).isError);
    CTEST_CHECK(!f.observe().isError);f.context.sessionId="other";CTEST_CHECK(f.edit(args).isError);
    f.context.sessionId="owner";++f.context.contextRevision;CTEST_CHECK(f.edit(args).isError);
    CTEST_CHECK(!f.observe().isError);++f.context.workspaceRevision;CTEST_CHECK(f.edit(args).isError);
    CTEST_CHECK(!f.observe().isError);CTEST_CHECK(!f.edit(args).isError);
 }
 void permissionAliasesUseTheNotebookPathAndCanonicalTarget(){
    Fixture f;CTEST_CHECK(!f.observe().isError);a::PermissionSettingsOptions settings;settings.workingDirectory=f.root.path();
    settings.fallbackMode=a::PermissionMode::Bypass;settings.inlineSettings={{"permissions",QJsonObject{{"deny",QJsonArray{"Edit(book.ipynb)"}}}}};
    const a::ToolCall call{"edit","NotebookEdit",{{"notebook_path",f.path},{"cell_id","first"},{"new_source","x"}}};
    auto policy=std::make_shared<a::SettingsPermissionPolicy>(settings);CTEST_CHECK(a::ToolRunner(f.registry,policy).run(call,f.context).isError);
    settings.fallbackMode=a::PermissionMode::DontAsk;settings.inlineSettings={{"permissions",QJsonObject{{"allow",QJsonArray{"NotebookEdit(book.ipynb)"}}}}};
    policy=std::make_shared<a::SettingsPermissionPolicy>(settings);const auto allowed=a::ToolRunner(f.registry,policy).run(call,f.context);
    CTEST_CHECK_MESSAGE(!allowed.isError,qPrintable(allowed.text+QString::fromUtf8(QJsonDocument(allowed.data).toJson())));
    auto plan=std::make_shared<a::RulePolicy>(a::PermissionMode::Plan);CTEST_CHECK(a::ToolRunner(f.registry,plan).run(call,f.context).isError);
    auto accept=std::make_shared<a::RulePolicy>(a::PermissionMode::AcceptEdits);CTEST_CHECK(!a::ToolRunner(f.registry,accept).run(call,f.context).isError);
    const auto second=f.root.filePath("second.ipynb"),link=f.root.filePath("alias.ipynb");write(second,read(f.path));CTEST_CHECK(createNativeTestLink(f.path,link));
    auto args=call.arguments;args["notebook_path"]=link;const auto prepared=f.registry->get("NotebookEdit").prepare(args,f.context);
    CTEST_CHECK(removeNativeTestLink(link));CTEST_CHECK(createNativeTestLink(second,link));CTEST_THROWS(Error,prepared.execute());
    f.context.protectedPaths.append(f.path);CTEST_CHECK(f.edit({{"cell_id","first"},{"new_source","private"}}).isError);
 }
 void boundsCancellationAndFailedArtifactsLeaveOriginalIntact(){
    Fixture f;CTEST_CHECK(!f.observe().isError);const auto original=read(f.path);
    CTEST_CHECK(f.edit({{"cell_id","first"},{"new_source",QString(1024*1024,'x')}}).isError);CTEST_COMPARE(read(f.path),original);
    const QJsonObject args{{"notebook_path",f.path},{"cell_id","first"},{"new_source","x"}};
    CancellationToken cancelled;cancelled.cancel();CTEST_THROWS(Error,a::editNotebook(original,args,cancelled));
    CTEST_THROWS(Error,a::editNotebook(QByteArray(1024*1024+1,' '),args));
    write(f.context.artifactsDirectory,"not a directory");CTEST_CHECK(f.edit(args).isError);CTEST_COMPARE(read(f.path),original);
 }
 void directControlsKeepTranscriptAndExpireObservationsAfterCompaction(){
    Fixture f;const auto options=engineOptions(f.root.filePath("state"));a::Engine engine(std::make_shared<QuietModel>(),f.registry,f.policy,options);
    CTEST_CHECK(engine.notebookToolsEnabled());const auto id=engine.createSession("fixture",f.root.path()).id;
    const QJsonObject args{{"notebook_path","book.ipynb"},{"cell_id","first"},{"new_source","x = 1"}};
    CTEST_CHECK(engine.runNotebookTool(id,"NotebookEdit",args).isError);
    CTEST_CHECK(!engine.runNotebookTool(id,"Read",{{"notebook_path","book.ipynb"}}).isError);
    CTEST_CHECK(!engine.runNotebookTool(id,"NotebookEdit",args).isError);CTEST_CHECK(engine.session(id).messages.isEmpty());
    a::SessionStore store(options.sessionsDirectory);
    {auto lease=store.acquire(id);CTEST_THROWS(Error,engine.runNotebookTool(id,"Read",{{"notebook_path","book.ipynb"}}));
        lease->append({"u1",a::MessageRole::User,"old request"});lease->append({"a1",a::MessageRole::Assistant,"old answer"});lease->append({"u2",a::MessageRole::User,"new request"});
        a::Compaction checkpoint;checkpoint.id=QUuid::createUuid().toString(QUuid::WithoutBraces);checkpoint.atMessageId="u2";checkpoint.throughMessageId="a1";
        checkpoint.summary="old request answered";checkpoint.inputTokensBefore=100;checkpoint.inputTokensAfter=10;lease->compact(checkpoint);}
    CTEST_CHECK(engine.runNotebookTool(id,"NotebookEdit",args).isError);
    CTEST_CHECK(!engine.runNotebookTool(id,"Read",{{"notebook_path","book.ipynb"}}).isError);
    CTEST_CHECK(!engine.runNotebookTool(id,"NotebookEdit",args).isError);CTEST_COMPARE(engine.session(id).messages.size(),3);
    CTEST_THROWS(Error,engine.runNotebookTool(id,"Read",{{"notebook_path","book.ipynb"},{"extra",1}}));
    CTEST_THROWS(Error,engine.runNotebookTool(id,"Read",{{"notebook_path","ordinary.txt"}}));
 }
 void modelBatchReadsBeforeEditingAndNativeControlsFollowWorktrees(){
    class Model:public a::Model {public:int calls=0;a::ModelReply generate(const a::ModelRequest&,const CancellationToken&,const TextCallback&)override{
        if(calls++==0)return {{},{{"read","Read",{{"path","book.ipynb"}}},{"edit","NotebookEdit",{{"notebook_path","book.ipynb"},{"cell_id","first"},{"new_source","batch = 42"}}}},{1,1}};
        return {"DONE",{},{1,1}};}};
    Fixture f;auto git=[&](QStringList args){args.prepend(f.root.path());args.prepend("-C");return QProcess::execute("git",args);};
    CTEST_COMPARE(git({"init","-q"}),0);CTEST_COMPARE(git({"add","book.ipynb"}),0);
    CTEST_COMPARE(git({"-c","user.name=Fixture","-c","user.email=fixture@example.invalid","commit","-qm","fixture"}),0);
    auto options=engineOptions(f.root.filePath("state"));options.worktrees.enabled=true;options.worktrees.fetchMissingBase=false;options.worktrees.directory=f.root.filePath("worktrees");
    a::Engine engine(std::make_shared<Model>(),f.registry,f.policy,options);const auto id=engine.createSession("fixture",f.root.path()).id;
    const auto run=engine.run({id,"Edit the notebook"}).result.get();CTEST_CHECK_MESSAGE(run.status==a::RunStatus::Completed,qPrintable(run.errorMessage));
    for(const auto& message:engine.session(id).messages)if(message.role==a::MessageRole::Tool)CTEST_CHECK_MESSAGE(!message.isError,qPrintable(message.text));
    CTEST_COMPARE(f.value()["cells"].toArray()[0].toObject()["source"].toString(),"batch = 42");const auto original=read(f.path);
    const auto entered=engine.runWorktreeTool(id,"EnterWorktree",{{"name","notebook"}});CTEST_CHECK_MESSAGE(!entered.isError,qPrintable(entered.text));
    const auto path=entered.data["worktreePath"].toString();
    CTEST_CHECK(!engine.runNotebookTool(id,"Read",{{"notebook_path","book.ipynb"}}).isError);
    auto edited=engine.runNotebookTool(id,"NotebookEdit",{{"notebook_path","book.ipynb"},{"cell_id","first"},{"new_source","isolated = 1"}});CTEST_CHECK_MESSAGE(!edited.isError,qPrintable(edited.text));
    CTEST_COMPARE(edited.data["notebook_path"].toString(),path+"/book.ipynb");CTEST_COMPARE(read(f.path),original);
    CTEST_CHECK(engine.runNotebookTool(id,"Read",{{"notebook_path",f.path}}).isError);
    CTEST_CHECK(!engine.runWorktreeTool(id,"ExitWorktree",{{"action","keep"}}).isError);
    CTEST_CHECK(engine.runNotebookTool(id,"NotebookEdit",{{"notebook_path","book.ipynb"},{"cell_id","first"},{"new_source","stale"}}).isError);
 }
 void apiAndMcpExposeOwnedNativeNotebookOperations(){
    Fixture f;QTemporaryDir state(QDir::current().filePath("notebook-owners-XXXXXX"));auto model=std::make_shared<QuietModel>();a::ApiOptions options{.engine={.decision={.enabled=false}}};options.engine=engineOptions({});options.workingDirectory=f.root.path();
    options.stateDirectory=state.filePath("api");options.clientTokens={{"society",QString(48,'a')},{"dreamscapes",QString(48,'b')}};
    a::Api api(model,f.registry,f.policy,options);auto call=[&](QString method,QJsonObject params={},QString token=QString(48,'a')){return api.dispatch(method,params,token).result.get().toObject();};
    CTEST_CHECK(call("agent.info")["notebooks_enabled"].toBool());const auto id=call("agent.sessions.create",{{"model","fixture"}})["session_id"].toString();
    const QJsonObject observe{{"session_id",id},{"notebook_path","book.ipynb"}};
    QJsonObject edit=observe;edit["cell_id"]="first";edit["new_source"]="api = 1";
    CTEST_CHECK(call("agent.notebooks.edit",edit)["is_error"].toBool());CTEST_CHECK(!call("agent.notebooks.read",observe)["is_error"].toBool());
    CTEST_CHECK(!call("agent.notebooks.edit",edit)["is_error"].toBool());CTEST_THROWS(Error,call("agent.notebooks.edit",edit,QString(48,'b')));
    CTEST_COMPARE(call("agent.sessions.get",{{"session_id",id}})["message_count"].toInt(),0);
    a::McpServerOptions standalone;standalone.workingDirectory=f.root.path();CTEST_CHECK(a::mcpServerOptions(f.registry,f.policy,standalone).experimentalCapabilities.contains("iisacc/notebooks"));
    auto host=std::make_shared<a::Engine>(model,f.registry,f.policy,engineOptions(state.filePath("mcp")));
    a::McpServerOptions mo;mo.engine=host;mo.model="fixture";mo.workingDirectory=f.root.path();
    mcp::ServerSession server(a::mcpServerOptions(f.registry,f.policy,mo));
    auto rpc=[&](int id,QString method,QJsonObject params){server.receive(QJsonObject{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}});
        for(int i=0;i<400;++i)for(const auto& value:server.takeMessages(10))if(value.toObject()["id"]==id)return value.toObject();throw std::runtime_error("MCP timeout");};
    const auto init=rpc(1,"initialize",{{"protocolVersion","2025-11-25"},{"capabilities",QJsonObject{}},{"clientInfo",QJsonObject{{"name","test"},{"version","1"}}}});
    CTEST_CHECK(QJsonDocument(init).toJson().contains("iisacc/notebooks"));server.receive(QJsonObject{{"jsonrpc","2.0"},{"method","notifications/initialized"}});
    edit.remove("session_id");CTEST_CHECK(rpc(2,"tools/call",{{"name","NotebookEdit"},{"arguments",edit}})["result"].toObject()["isError"].toBool());
    CTEST_CHECK(!rpc(3,"tools/call",{{"name","Read"},{"arguments",QJsonObject{{"path","book.ipynb"}}}})["result"].toObject()["isError"].toBool());
    const auto result=rpc(4,"tools/call",{{"name","NotebookEdit"},{"arguments",edit}})["result"].toObject();
    CTEST_CHECK_MESSAGE(!result["isError"].toBool(),QJsonDocument(result).toJson().constData());CTEST_COMPARE(result["structuredContent"].toObject()["cell_id"].toString(),"first");
 }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) { std::cerr << "Expected a case name or --offline" << std::endl; return 1; }
    const std::string_view selection = argc == 2 ? argv[1] : "";
    const bool offline = selection == "--offline";
    agent_ctest::CTestRunner runner(offline ? std::string_view{} : selection);
    NotebookTests tests;
    runner.run("replacementRequiresReadAndPreservesUnrelatedData", [&] { tests.replacementRequiresReadAndPreservesUnrelatedData(); });
    runner.run("actualCellIdPrecedesFallbackIndexAndInsertionIsAfter", [&] { tests.actualCellIdPrecedesFallbackIndexAndInsertionIsAfter(); });
    runner.run("conversionProducesValidFieldsAndOldFormatsUseIndexSelectors", [&] { tests.conversionProducesValidFieldsAndOldFormatsUseIndexSelectors(); });
    runner.run("largeAttributionHasImmutableBeforeAndAfterFiles", [&] { tests.largeAttributionHasImmutableBeforeAndAfterFiles(); });
    runner.run("invalidInputsAndDocumentsNeverWrite", [&] { tests.invalidInputsAndDocumentsNeverWrite(); });
    runner.run("rawCellsArraysAttachmentsAndFuturePayloadsArePreserved", [&] { tests.rawCellsArraysAttachmentsAndFuturePayloadsArePreserved(); });
    runner.run("readsAreBoundToContentOwnerAndRevisions", [&] { tests.readsAreBoundToContentOwnerAndRevisions(); });
    runner.run("permissionAliasesUseTheNotebookPathAndCanonicalTarget", [&] { tests.permissionAliasesUseTheNotebookPathAndCanonicalTarget(); });
    runner.run("boundsCancellationAndFailedArtifactsLeaveOriginalIntact", [&] { tests.boundsCancellationAndFailedArtifactsLeaveOriginalIntact(); });
    runner.run("directControlsKeepTranscriptAndExpireObservationsAfterCompaction", [&] { tests.directControlsKeepTranscriptAndExpireObservationsAfterCompaction(); });
    if (!offline)
        runner.run("modelBatchReadsBeforeEditingAndNativeControlsFollowWorktrees", [&] { tests.modelBatchReadsBeforeEditingAndNativeControlsFollowWorktrees(); });
    runner.run("apiAndMcpExposeOwnedNativeNotebookOperations", [&] { tests.apiAndMcpExposeOwnedNativeNotebookOperations(); });
    return runner.result();
}

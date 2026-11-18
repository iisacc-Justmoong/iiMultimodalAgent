#include "iiMultimodalAgent/agent/Tools.h"
#include "iiMultimodalAgent/agent/Engine.h"
#include "iiMultimodalAgent/agent/Api.h"
#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>
#include "CTestSupport.h"
#include <QCoreApplication>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
struct Policy final:a::PermissionPolicy {
    mutable int queries=0;
    bool granted=false;
    a::PermissionDecision decide(const a::ToolDefinition&,const QJsonObject& args,const a::ToolContext&) const override {
        ++queries;
        return {args["value"]==99?a::PermissionBehavior::Deny:granted?a::PermissionBehavior::Allow:a::PermissionBehavior::Ask,
            "HOST_REASON",QJsonArray{QJsonObject{{"type","addRules"},{"rules",QJsonArray{QJsonObject{{"toolName","change"}}}},
                {"behavior","allow"},{"destination","session"}}}};
    }
};
struct Fixture {
    std::shared_ptr<a::ToolRegistry> registry=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<Policy> policy=std::make_shared<Policy>();
    int executions=0,preparations=0;QList<int> executed;
    Fixture() {
        a::Tool tool;tool.definition={"change","Prepared fixture",{{"type","object"},{"properties",QJsonObject{{"value",QJsonObject{{"type","integer"},{"minimum",1}}}}},
            {"required",QJsonArray{"value"}},{"additionalProperties",false}}};
        tool.definition.concurrencySafe=true;
        tool.prepare=[this,definition=tool.definition](const QJsonObject& input,const a::ToolContext&) {
            ++preparations;auto preview=definition;preview.metadata["value"]=input["value"];
            return a::PreparedTool{preview,[this,input]{++executions;executed.append(input["value"].toInt());return a::ToolResult{"ok",input};}};
        };
        tool.execute=[](const auto&,const auto&){throw std::runtime_error("Unprepared execution");return a::ToolResult{};};registry->add(tool);
    }
};
class Model final:public a::Model {
public:
    a::ModelReply generate(const a::ModelRequest& request,const CancellationToken&,const TextCallback&) override {
        if(request.messages.last().role==a::MessageRole::Tool)return {"DONE"};
        return {{},{{"call","change",{{"value",1}}}}};
    }
};
}
class PermissionRequestTests final {
public:
    void askRunsHooksBeforeHostAndRepreparesUpdatedInput() {
        Fixture f;int asked=0,fallback=0;QJsonObject payload;a::ToolRunnerOptions options;
        options.hooks.append([&](const a::HookInput& input,const CancellationToken&){
            a::HookResult result;if(input.kind!=a::HookKind::PermissionRequest)return result;
            ++asked;payload=input.context;if(input.call.arguments["value"]!=1)throw std::runtime_error("Wrong hook input");
            a::PermissionResponse response;response.behavior=a::PermissionBehavior::Allow;response.updatedArguments=QJsonObject{{"value",7}};
            result.permissionResponse=response;return result;
        });
        options.permission=[&](const auto&,const auto&,const auto&){++fallback;return false;};
        a::ToolContext context{"session","run","/work"};context.transcriptPath="/private/transcript.jsonl";
        const auto result=a::ToolRunner(f.registry,f.policy,options).run({"call","change",{{"value",1}}},context);
        CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));CTEST_COMPARE(asked,1);CTEST_COMPARE(fallback,0);CTEST_COMPARE(f.executed,QList<int>{7});CTEST_COMPARE(f.preparations,2);
        CTEST_COMPARE(payload["permission_reason"],"HOST_REASON\nPrepared fixture\n{\"value\":1}");
        CTEST_COMPARE(payload["permission_suggestions"].toArray().size(),1);CTEST_COMPARE(payload["permission_preview"].toObject()["_meta"].toObject()["value"],1);
        CTEST_COMPARE(payload["transcript_path"],context.transcriptPath);
    }
    void allowAndDenyDoNotAskAndChangedInputCannotBypassDenyOrSchema() {
        for(int value:{0,99}) {
            Fixture f;a::ToolRunnerOptions options;options.hooks.append([value](const auto& input,const auto&){
                a::HookResult result;if(input.kind==a::HookKind::PermissionRequest){a::PermissionResponse response;response.behavior=a::PermissionBehavior::Allow;
                    response.updatedArguments=QJsonObject{{"value",value}};result.permissionResponse=response;}return result;});
            CTEST_CHECK(a::ToolRunner(f.registry,f.policy,options).run({"id","change",{{"value",1}}},{}).isError);CTEST_COMPARE(f.executions,0);
        }
        Fixture f;int count=0;a::ToolRunnerOptions options;options.hooks.append([&](const auto& input,const auto&){
            if(input.kind==a::HookKind::PermissionRequest)++count;return a::HookResult{};});
        auto run=[&](int value){return a::ToolRunner(f.registry,f.policy,options).run({"id","change",{{"value",value}}},{});};
        CTEST_CHECK(run(99).isError);f.policy->granted=true;CTEST_CHECK(!run(1).isError);CTEST_COMPARE(count,0);
        for(auto mode:{a::PermissionMode::DontAsk,a::PermissionMode::Plan}) {
            CTEST_CHECK(a::ToolRunner(f.registry,std::make_shared<a::RulePolicy>(mode),options).run({"id","change",{{"value",1}}},{}).isError);
        }
        CTEST_COMPARE(count,0);
    }
    void structuredHostFallbackAndLegacyCallbackRemainAvailable() {
        Fixture f;a::ToolRunnerOptions options;int legacy=0,structured=0;
        options.permission=[&](const auto&,const auto&,const auto&){++legacy;return true;};
        options.permissionResponse=[&](const auto&,const auto& decision,const auto&){++structured;
            if(decision.suggestions.size()!=1)throw std::runtime_error("Missing suggestions");
            a::PermissionResponse result;result.behavior=a::PermissionBehavior::Allow;result.updatedArguments=QJsonObject{{"value",8}};return result;};
        CTEST_CHECK(!a::ToolRunner(f.registry,f.policy,options).concurrencySafe({"id","change",{{"value",1}}}));
        CTEST_CHECK(!a::ToolRunner(f.registry,f.policy,options).run({"id","change",{{"value",1}}},{}).isError);CTEST_COMPARE(f.executed,QList<int>{8});
        CTEST_COMPARE(structured,1);CTEST_COMPARE(legacy,0);options.permissionResponse={};
        CTEST_CHECK(a::ToolRunner(f.registry,f.policy,options).concurrencySafe({"id","change",{{"value",1}}}));
        CTEST_CHECK(!a::ToolRunner(f.registry,f.policy,options).run({"id","change",{{"value",1}}},{}).isError);CTEST_COMPARE(legacy,1);
        options.permission={};CTEST_CHECK(a::ToolRunner(f.registry,f.policy,options).run({"id","change",{{"value",1}}},{}).isError);
    }
    void updatesUseOnlyTrustedHostHandlerAndAffectLaterDecisions() {
        Fixture f;int requests=0,updates=0;a::ToolRunnerOptions options;
        options.permissionResponse=[&](const auto&,const auto& decision,const auto&){++requests;a::PermissionResponse r;
            r.behavior=a::PermissionBehavior::Allow;r.updatedPermissions=decision.suggestions;return r;};
        auto run=[&]{return a::ToolRunner(f.registry,f.policy,options).run({"id","change",{{"value",1}}},{"session"});};
        const auto unsupported=run();CTEST_CHECK(unsupported.isError);CTEST_COMPARE(unsupported.data["error_code"],"runtime_unavailable");CTEST_COMPARE(f.executions,0);
        options.permissionUpdates=[&](const QJsonArray& values,const a::ToolContext& context){
            if(context.sessionId!="session"||values.size()!=1)throw std::runtime_error("Wrong update identity");++updates;f.policy->granted=true;};
        CTEST_CHECK(!run().isError);CTEST_CHECK(!run().isError);CTEST_COMPARE(requests,2);CTEST_COMPARE(updates,1);CTEST_COMPARE(f.executions,2);
        f.policy->granted=false;options.permissionUpdates=[](const auto&,const auto&){throw Error(ErrorCode::StorageFailure,"HOST_UPDATE_FAILED");};
        const auto failed=run();CTEST_CHECK(failed.isError);CTEST_CHECK(failed.text.contains("HOST_UPDATE_FAILED"));CTEST_COMPARE(f.executions,2);
    }
    void denialIsDistinctFromInterruptAndInvalidResponseNeverExecutes() {
        Fixture f;a::ToolRunnerOptions options;options.permissionResponse=[](const auto&,const auto&,const auto&){a::PermissionResponse r;r.message="HOST_DENIED";return r;};
        const auto denied=a::ToolRunner(f.registry,f.policy,options).run({"id","change",{{"value",1}}},{});
        CTEST_CHECK(denied.isError);CTEST_CHECK(denied.text.contains("HOST_DENIED"));
        options.permissionResponse=[](const auto&,const auto&,const auto&){a::PermissionResponse r;r.interrupt=true;r.message="HOST_INTERRUPT";return r;};
        a::ToolContext context;CTEST_THROWS(Error,a::ToolRunner(f.registry,f.policy,options).run({"id","change",{{"value",1}}},context));
        CTEST_CHECK(context.cancellation.isCancelled());
        options.permissionResponse=[](const auto&,const auto&,const auto&){a::PermissionResponse r;r.behavior=a::PermissionBehavior::Ask;return r;};
        CTEST_CHECK(a::ToolRunner(f.registry,f.policy,options).run({"id","change",{{"value",1}}},{}).isError);CTEST_COMPARE(f.executions,0);
    }
    void authenticatedApiUsesSameStructuredApprovalAndDoesNotAcceptWireGrants() {
        Fixture f;QTemporaryDir root;const auto work=root.filePath("work");QDir().mkpath(work);int requests=0;
        a::ApiOptions options{.engine={.decision={.enabled=false}}};options.workingDirectory=work;options.stateDirectory=root.filePath("state");
        const QString key(40,'a');options.clientTokens={{"society",key}};options.engine.compaction.automatic=false;
        options.engine.permissionResponse=[&](const auto&,const auto&,const auto&){++requests;a::PermissionResponse result;
            result.behavior=a::PermissionBehavior::Allow;result.updatedArguments=QJsonObject{{"value",6}};return result;};
        a::Api api(std::make_shared<Model>(),f.registry,f.policy,options);
        auto call=[&](const QString& name,const QJsonObject& args){return api.dispatch(name,args,key).result.get().toObject();};
        const auto id=call("agent.sessions.create",{{"model","fixture"}})["session_id"].toString();
        CTEST_THROWS(Error,call("agent.run",{{"session_id",id},{"prompt","run"},{"permissionResponse",QJsonObject{{"behavior","allow"}}}}));CTEST_COMPARE(requests,0);
        const auto completed=call("agent.run",{{"session_id",id},{"prompt","run"}});
        CTEST_COMPARE(completed["status"],"completed");CTEST_COMPARE(requests,1);CTEST_COMPARE(f.executed,QList<int>{6});api.close();
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner;
    PermissionRequestTests tests;
    runner.run("askRunsHooksBeforeHostAndRepreparesUpdatedInput", [&] { tests.askRunsHooksBeforeHostAndRepreparesUpdatedInput(); });
    runner.run("allowAndDenyDoNotAskAndChangedInputCannotBypassDenyOrSchema", [&] { tests.allowAndDenyDoNotAskAndChangedInputCannotBypassDenyOrSchema(); });
    runner.run("structuredHostFallbackAndLegacyCallbackRemainAvailable", [&] { tests.structuredHostFallbackAndLegacyCallbackRemainAvailable(); });
    runner.run("updatesUseOnlyTrustedHostHandlerAndAffectLaterDecisions", [&] { tests.updatesUseOnlyTrustedHostHandlerAndAffectLaterDecisions(); });
    runner.run("denialIsDistinctFromInterruptAndInvalidResponseNeverExecutes", [&] { tests.denialIsDistinctFromInterruptAndInvalidResponseNeverExecutes(); });
    runner.run("authenticatedApiUsesSameStructuredApprovalAndDoesNotAcceptWireGrants", [&] { tests.authenticatedApiUsesSameStructuredApprovalAndDoesNotAcceptWireGrants(); });
    return runner.result();
}

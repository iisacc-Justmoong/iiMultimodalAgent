#include <iiMultimodalAgent/agent/Api.h>
#include <iiMultimodalAgent/agent/ShellTasks.h>
#include <iiMultimodalAgent/agent/PermissionSettings.h>
#include <iiMultimodalAgent/agent/GraphQL.h>
#include <iiFileProviderPresentation.h>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <chrono>
#include <filesystem>
#include <thread>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
using namespace std::chrono_literals;
namespace a = iiMultimodalAgent::agent;
namespace {
struct PresentationCleanup {
    std::filesystem::path root;
    ~PresentationCleanup(){try{iiFileProvider::native::Presentation::cancelPending(root);}catch(...){}}
};
const QString firstToken(48, 'a'), secondToken(48, 'b');
class Model final : public a::Model {
public:
    std::atomic_bool waiting = false, cancelled = false;
    bool budgets = false, waitForSummary = false;
    std::optional<ContextBudget> measure(const a::ModelRequest& request, const CancellationToken& token) override {
        token.throwIfCancelled(); if (!budgets) return std::nullopt;
        qint64 count = 100 + request.systemPrompt.size() + request.tools.size() * 40;
        for (const auto& m : request.messages) count += m.text.size() + 10;
        return ContextBudget{count, 16384};
    }
    a::ModelReply generate(const a::ModelRequest& request, const CancellationToken& token, const TextCallback& delta) override {
        if (request.messages.last().text == "wait" || (request.summarizing && waitForSummary)) {
            waiting = true;
            while (!token.isCancelled()) std::this_thread::sleep_for(1ms);
            cancelled = true; token.throwIfCancelled();
        }
        if (request.summarizing) return {"API summary preserves the earlier request and observed response.", {}, {100, 12, 0, 0}};
        QStringList values;
        for (const auto& m : request.messages) if (m.role == a::MessageRole::User) values.append(m.text);
        const auto text = values.join('|'); if (delta) delta(text);
        return {text, {}, {1, 1, 0, 0}};
    }
};
a::ApiOptions options(QTemporaryDir& root) {
    a::ApiOptions o{.engine={.decision={.enabled=false}}}; o.workingDirectory = root.filePath("workspace"); QDir().mkpath(o.workingDirectory);
    o.stateDirectory = root.filePath("private"); o.clientTokens = {{"society", firstToken}, {"dreamscapes", secondToken}};
    return o;
}
QJsonObject call(a::Api& api, QString method, QJsonObject p = {}, QString token = firstToken) {
    auto request = api.dispatch(method, p, token);
    if (request.result.wait_for(3s) != std::future_status::ready) throw std::runtime_error("API request did not finish");
    return request.result.get().toObject();
}
template<class F> void error(F fn, ErrorCode expected) {
    try { fn(); agent_ctest::fail(__FILE__, __LINE__, "Expected API error"); }
    catch (const Error& e) { CTEST_COMPARE(e.code(), expected); }
}
}
class AgentApiTests : public QObject {
public:
    void graphqlDirectApiUsesOwnedSessionsAndPermission() {
        QTemporaryDir root;PresentationCleanup cleanup{std::filesystem::path(root.path().toStdU16String())};
        auto config=options(root);auto registry=std::make_shared<a::ToolRegistry>();
        a::GraphQLOptions graph;graph.endpoints.insert("test",{QUrl("https://example.com/graphql"),{}});
        int calls=0;registry->add(a::graphQLTool(graph,[&](const auto&,const auto& input,const auto&,int,int){
            ++calls;return QJsonObject{{"data",QJsonObject{{"query",input["query"]}}}};
        }));
        a::Api api(std::make_shared<Model>(),registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),config);
        auto requestApi=[&](const QString& method,const QJsonObject& args,const QString& token=firstToken) {
            auto job=api.dispatch(method,args,token);
            if(job.result.wait_for(30s)!=std::future_status::ready)throw std::runtime_error("GraphQL API test timed out");
            return job.result.get().toObject();
        };
        const auto session=requestApi("agent.sessions.create",{{"model","fixture"}})["session_id"].toString();
        const QJsonObject request{{"session_id",session},{"endpoint","test"},{"query","query { ping }"}};
        const auto result=requestApi("agent.graphql.execute",request);
        CTEST_CHECK(!result["is_error"].toBool());CTEST_COMPARE(result["result"].toObject()["data"].toObject()["query"],"query { ping }");
        CTEST_CHECK(requestApi("agent.info",{})["graphql_enabled"].toBool());CTEST_COMPARE(calls,1);
        error([&]{requestApi("agent.graphql.execute",request,secondToken);},ErrorCode::NotFound);
        CTEST_COMPARE(calls,1);
        QTemporaryDir deniedRoot;PresentationCleanup deniedCleanup{std::filesystem::path(deniedRoot.path().toStdU16String())};auto deniedConfig=options(deniedRoot);
        a::Api deniedApi(std::make_shared<Model>(),registry,std::make_shared<a::RulePolicy>(),deniedConfig);
        auto create=deniedApi.dispatch("agent.sessions.create",{{"model","fixture"}},firstToken);
        CTEST_CHECK(create.result.wait_for(30s)==std::future_status::ready);
        const auto deniedSession=create.result.get().toObject()["session_id"].toString();CTEST_CHECK(!deniedSession.isEmpty());
        auto denied=deniedApi.dispatch("agent.graphql.execute",{{"session_id",deniedSession},{"endpoint","test"},{"query","query { ping }"}},firstToken);
        CTEST_CHECK(denied.result.wait_for(30s)==std::future_status::ready);CTEST_CHECK(denied.result.get().toObject()["is_error"].toBool());
        CTEST_COMPARE(calls,1);
    }
    void teamRoutesUseAuthenticatedSessionIdentity(){
        QTemporaryDir root;auto config=options(root);config.teamsEnabled=true;config.subagentsEnabled=true;config.engine.toolSearch.enabled=false;
        a::Api api(std::make_shared<Model>(),std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),config);
        const auto id=call(api,"agent.sessions.create",{{"model","local"}})["session_id"].toString();
        const auto other=call(api,"agent.sessions.create",{{"model","local"}},secondToken)["session_id"].toString();
        CTEST_CHECK(call(api,"agent.info")["teams_enabled"].toBool());
        CTEST_COMPARE(call(api,"agent.info")["team_auto_task_claim_enabled"].toBool(),config.engine.taskToolsEnabled);
        CTEST_CHECK(!call(api,"agent.teams.create",{{"session_id",id},{"team_name","society"}})["is_error"].toBool());
        error([&]{call(api,"agent.teams.status",{{"session_id",id}},secondToken);},ErrorCode::NotFound);
        CTEST_CHECK(call(api,"agent.teams.status",{{"session_id",other}},secondToken)["result"].toObject()["team"].isNull());
        error([&]{call(api,"agent.teams.spawn",{{"session_id",id},{"prompt","Do not start an ordinary subagent"},{"description","Missing teammate name"}});},ErrorCode::InvalidArgument);
        const auto child=call(api,"agent.teams.spawn",{{"session_id",id},{"name","worker"},{"prompt","API_TEAM"}});
        CTEST_CHECK_MESSAGE(!child["is_error"].toBool(),QJsonDocument(child).toJson().constData());
        const auto waited=call(api,"agent.teams.wait",{{"session_id",id},{"timeout_ms",2000}});
        CTEST_CHECK(waited["result"].toObject()["idle"].toBool());
        const auto inbox=call(api,"agent.teams.inbox",{{"session_id",id}})["result"].toObject();CTEST_CHECK(inbox["total"].toInt()>0);
        const auto childId=child["result"].toObject()["session_id"].toString();error([&]{call(api,"agent.teams.send",{{"session_id",childId},{"to","team-lead"},{"summary","forged"},{"message","forged"}});},ErrorCode::NotFound);
        CTEST_CHECK(!call(api,"agent.teams.delete",{{"session_id",id}})["is_error"].toBool());
        CTEST_CHECK(api.isControlMethod("agent.teams.stop"));CTEST_CHECK(api.isControlMethod("agent.teams.send"));
    }
    void permissionsInspectionIsAuthenticatedAndCannotChangePolicy() {
        QTemporaryDir root;auto config=options(root);a::PermissionSettingsOptions settings;settings.workingDirectory=config.workingDirectory;
        settings.inlineSettings={{"permissions",QJsonObject{{"deny",QJsonArray{"Write"}}}},{"env",QJsonObject{{"secret","DO_NOT_DISCLOSE"}}}};
        auto policy=std::make_shared<a::SettingsPermissionPolicy>(settings);
        a::Api api(std::make_shared<Model>(),std::make_shared<a::ToolRegistry>(),policy,config);
        const auto id=call(api,"agent.sessions.create",{{"model","local"}}).value("session_id");
        const auto result=call(api,"agent.permissions.get",{{"session_id",id}});
        CTEST_COMPARE(result["provider"],"settings");CTEST_COMPARE(result["rules"].toArray()[0].toObject()["behavior"],"deny");
        CTEST_CHECK(!QJsonDocument(result).toJson().contains("DO_NOT_DISCLOSE"));
        error([&]{call(api,"agent.permissions.get",{{"session_id",id}},secondToken);},ErrorCode::NotFound);
        error([&]{call(api,"agent.permissions.get",{{"session_id",id}},"invalid");},ErrorCode::Unauthorized);
        error([&]{call(api,"agent.permissions.get",{{"session_id",id},{"mode","bypassPermissions"}});},ErrorCode::InvalidArgument);
    }
    void liveProfilesShareHostConfigurationButRespectClientIdentity() {
        QTemporaryDir root;auto config=options(root);config.subagentsEnabled=true;config.subagents.profiles.enabled=true;config.subagents.profiles.projectBoundary=config.workingDirectory;
        a::Api api(std::make_shared<Model>(),std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),config);
        const auto id=call(api,"agent.sessions.create",{{"model","local"}})["session_id"].toString();
        auto catalog=call(api,"agent.agents.profiles",{{"session_id",id}});CTEST_CHECK(!catalog["is_error"].toBool());CTEST_COMPARE(catalog["result"].toObject()["profiles"].toArray().size(),3);
        const auto dir=config.workingDirectory+"/.claude/agents";CTEST_CHECK(QDir().mkpath(dir));QFile file(dir+"/review.md");CTEST_CHECK(file.open(QIODevice::WriteOnly));file.write("---\nname: reviewer\ndescription: API profile\ntools: []\n---\nInspect provided facts.\n");file.close();
        catalog=call(api,"agent.agents.profiles",{{"session_id",id}});CTEST_COMPARE(catalog["result"].toObject()["profiles"].toArray().size(),4);
        const auto result=call(api,"agent.agents.run",{{"session_id",id},{"prompt","API_PROFILE"},{"subagent_type","reviewer"}});CTEST_CHECK(!result["is_error"].toBool());CTEST_COMPARE(result["result"].toObject()["result"].toObject()["text"],"API_PROFILE");
        CTEST_THROWS(Error,call(api,"agent.agents.profiles",{{"session_id",id}},secondToken));
    }
    void childGenerationUsesHostOptionsIndependentlyOfParentRequests() {
        class OptionsModel final : public a::Model {
            a::ModelReply generate(const a::ModelRequest& r,const CancellationToken&,const TextCallback&) override {
                return {QString::number(r.generation.temperature)+":"+QString::number(r.generation.maxTokens),{}};
            }
        };
        QTemporaryDir root;auto o=options(root);o.subagentsEnabled=true;
        o.subagents.generation.temperature=0.31;o.subagents.generation.maxTokens=77;
        a::Api api(std::make_shared<OptionsModel>(),std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),o);
        const auto id=call(api,"agent.sessions.create",{{"model","local"}}).value("session_id");
        const auto child=call(api,"agent.agents.run",{{"session_id",id},{"prompt","report settings"}});
        CTEST_CHECK(!child["is_error"].toBool());CTEST_COMPARE(child["result"].toObject()["result"].toObject()["text"],"0.31:77");
        const auto parent=call(api,"agent.run",{{"session_id",id},{"prompt","report settings"},{"options",QJsonObject{{"temperature",0.2},{"max_tokens",99}}}});
        CTEST_COMPARE(parent["text"],"0.2:99");
        CTEST_CHECK(call(api,"agent.agents.run",{{"session_id",id},{"prompt","replace settings"},{"options",QJsonObject{{"temperature",1.0}}}})["is_error"].toBool());
    }
    void subagentControlsAreAuthenticatedAndRemainResponsive() {
        QTemporaryDir root; auto o=options(root); o.subagentsEnabled=true;
        o.maxConcurrentRequests=1; o.maxQueuedRequests=0;
        auto model=std::make_shared<Model>();
        a::Api api(model,std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),o);
        const auto id=call(api,"agent.sessions.create",{{"model","local"}}).value("session_id");
        CTEST_CHECK(call(api,"agent.info")["subagents_enabled"].toBool());
        auto launched=call(api,"agent.agents.run",{{"session_id",id},{"prompt","wait"},{"run_in_background",true}});
        CTEST_CHECK(!launched["is_error"].toBool()); const auto child=launched["result"].toObject().value("agentId");
        CTEST_WAIT_DEFAULT(model->waiting.load());
        CTEST_THROWS(Error,call(api,"agent.agents.list",{{"session_id",id}},secondToken));
        const auto other=call(api,"agent.sessions.create",{{"model","local"}},secondToken).value("session_id");
        auto foreign=call(api,"agent.agents.output",{{"session_id",other},{"agent_id",child}},secondToken);
        CTEST_CHECK(foreign["is_error"].toBool());
        auto waiting=api.dispatch("agent.agents.output",{{"session_id",id},{"agent_id",child},{"block",true},{"timeout_ms",2000}},firstToken);
        auto stopped=call(api,"agent.agents.stop",{{"session_id",id},{"agent_id",child}}); CTEST_CHECK(!stopped["is_error"].toBool());
        CTEST_CHECK(waiting.result.wait_for(3s)==std::future_status::ready);
        CTEST_COMPARE(waiting.result.get().toObject()["result"].toObject()["status"],"cancelled");
        CTEST_COMPARE(call(api,"agent.inputs.list",{{"session_id",id}})["count"],1);
        auto resumed=call(api,"agent.agents.run",{{"session_id",id},{"resume",child},{"prompt","continue"}});
        CTEST_CHECK(!resumed["is_error"].toBool()); CTEST_COMPARE(resumed["result"].toObject()["status"],"completed");
        CTEST_CHECK(call(api,"agent.sessions.get",{{"session_id",id}})["messages"].toArray().isEmpty());
    }
    void skillsAreClientScopedAndRunWithoutPlaceholderPrompt() {
        for(bool fork:{false,true}) {
        QTemporaryDir root; auto o = options(root);
        o.subagentsEnabled=fork;
        const auto dir = o.workingDirectory + "/.claude/skills/inspect"; CTEST_CHECK(QDir().mkpath(dir));
        QFile file(dir + "/SKILL.md"); CTEST_CHECK(file.open(QIODevice::WriteOnly));
        file.write(QByteArray("---\ndescription: Inspect\ndisable-model-invocation: true\n")+(fork?"context: fork\n":"")+"---\nSKILL_API $ARGUMENTS"); file.close();
        a::Api api(std::make_shared<Model>(), std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        CTEST_CHECK(call(api, "agent.info")["skills_enabled"].toBool());
        auto catalog = call(api, "agent.skills.list", {{"session_id", id}}); CTEST_COMPARE(catalog["skills"].toArray().size(), 1);
        CTEST_CHECK(!catalog["skills"].toArray()[0].toObject().contains("content"));
        error([&] { call(api, "agent.skills.list", {{"session_id", id}}, secondToken); }, ErrorCode::NotFound);
        error([&] { call(api, "agent.skills.list", {{"session_id", id}}, "invalid"); }, ErrorCode::Unauthorized);
        error([&] { call(api, "agent.run", {{"session_id", id}, {"skill", 4}}); }, ErrorCode::InvalidArgument);
        const auto run = call(api, "agent.run", {{"session_id", id}, {"skill", "inspect"}, {"skill_arguments", "literal args"}});
        CTEST_COMPARE(run["status"], "completed"); CTEST_CHECK(run["text"].toString().contains("SKILL_API literal args"));
        CTEST_COMPARE(run["session_id"],id);
        if(fork) {
            const auto jobs=call(api,"agent.agents.list",{{"session_id",id}})["result"].toObject()["agents"].toArray();CTEST_COMPARE(jobs.size(),1);
            const auto messages=call(api,"agent.sessions.get",{{"session_id",id}})["messages"].toArray();CTEST_COMPARE(messages.size(),2);
            CTEST_CHECK(!messages[0].toObject()["text"].toString().contains("SKILL_API"));
        }
        error([&] { call(api, "agent.inputs.run", {{"session_id", id}, {"skill", "inspect"}}); }, ErrorCode::InvalidArgument);
        o.engine.skills.enabled = false; api.close();
        a::Api disabled(std::make_shared<Model>(), std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        CTEST_CHECK(!call(disabled, "agent.info")["skills_enabled"].toBool());
        CTEST_CHECK(call(disabled, "agent.skills.list", {{"session_id", id}})["skills"].toArray().isEmpty());
        CTEST_COMPARE(call(disabled, "agent.run", {{"session_id", id}, {"skill", "inspect"}})["error_code"], "runtime_unavailable");
        }
    }
    void inputsReachAnActiveRunWithoutAnAvailableRunWorker() {
        QTemporaryDir root; auto o = options(root); o.maxConcurrentRequests = 1; o.maxQueuedRequests = 0;
        auto model = std::make_shared<Model>();
        a::Api api(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        auto run = api.dispatch("agent.run", {{"session_id", id}, {"prompt", "wait"}}, firstToken);
        CTEST_WAIT_VERIFY(model->waiting.load(), 3000);
        error([&] { call(api, "agent.inputs.enqueue", {{"session_id", id}, {"text", "wrong app"}}, secondToken); }, ErrorCode::NotFound);
        const auto input = call(api, "agent.inputs.enqueue", {{"session_id", id}, {"text", "urgent direction"}, {"priority", "now"}});
        CTEST_CHECK(!input["input"].toObject()["id"].toString().isEmpty());
        CTEST_CHECK(run.result.wait_for(3s) == std::future_status::ready);
        const auto answer = run.result.get().toObject(); CTEST_COMPARE(answer["status"], "completed");
        CTEST_CHECK(answer["text"].toString().endsWith("urgent direction"));
        CTEST_COMPARE(call(api, "agent.inputs.list", {{"session_id", id}})["count"].toInt(), 0);
        auto queued = call(api, "agent.inputs.enqueue", {{"session_id", id}, {"text", "keep across restart"}})["input"].toObject();
        api.close();
        a::Api restored(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        CTEST_COMPARE(call(restored, "agent.inputs.list", {{"session_id", id}})["inputs"].toArray()[0].toObject()["id"], queued["id"]);
        const auto fork = call(restored, "agent.sessions.fork", {{"session_id", id}}).value("session_id");
        CTEST_COMPARE(call(restored, "agent.inputs.list", {{"session_id", fork}})["count"].toInt(), 0);
        CTEST_COMPARE(call(restored, "agent.inputs.run", {{"session_id", id}})["status"], "completed");
        error([&] { call(restored, "agent.inputs.remove", {{"session_id", id}, {"input_id", queued["id"]}}); }, ErrorCode::NotFound);
    }
    void shellOutputHonorsApiDeadlineWithoutStoppingCommand() {
#if (!defined(Q_OS_UNIX) && !defined(Q_OS_WIN)) || defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
        agent_ctest::skip("Background shell execution requires a desktop host");
#endif
        QTemporaryDir root; auto o = options(root); o.requestTimeoutMs = 200;
        auto shells = std::make_shared<a::ShellTasks>(o.workingDirectory, root.filePath("shell-state"));
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, o.workingDirectory, shells);
        a::Api api(std::make_shared<Model>(), registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass), o);
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        const auto task = call(api, "agent.shell.start", {{"session_id", id}, {"command", "sleep 30"}})["result"].toObject().value("backgroundTaskId");
        error([&] { call(api, "agent.shell.output", {{"session_id", id}, {"task_id", task}, {"timeout", 1000}}); }, ErrorCode::Timeout);
        CTEST_COMPARE(shells->output(id.toString(), task.toString(), false, 0, 0, 1024)["task"].toObject()["status"], "running");
        shells->stop(id.toString(), task.toString());
    }
    void backgroundShellsAreAuthenticatedAndControlledDuringRuns() {
#if (!defined(Q_OS_UNIX) && !defined(Q_OS_WIN)) || defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
        agent_ctest::skip("Background shell execution requires a desktop host");
#endif
        QTemporaryDir root; auto o = options(root); auto model = std::make_shared<Model>();
        auto shells = std::make_shared<a::ShellTasks>(o.workingDirectory, root.filePath("shell-state"));
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, o.workingDirectory, shells);
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::DontAsk,
            QList<a::PermissionRule>{{"Bash", a::PermissionBehavior::Allow}});
        a::Api api(model, registry, policy, o);
        CTEST_CHECK(call(api, "agent.info")["background_tasks_enabled"].toBool());
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        auto run = api.dispatch("agent.run", {{"session_id", id}, {"prompt", "wait"}}, firstToken);
        CTEST_WAIT_VERIFY(model->waiting.load(), 3000);
        const auto started = call(api, "agent.shell.start", {{"session_id", id}, {"command", "printf api-background; sleep 30"}});
        CTEST_CHECK(!started["is_error"].toBool()); const auto task = started["result"].toObject().value("backgroundTaskId");
        CTEST_CHECK(!task.toString().isEmpty());
        error([&] { call(api, "agent.shell.output", {{"session_id", id}, {"task_id", task}}, secondToken); }, ErrorCode::NotFound);
        error([&] { call(api, "agent.shell.list", {{"session_id", id}}, "bad-token"); }, ErrorCode::Unauthorized);
        const auto another = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        CTEST_CHECK(call(api, "agent.shell.stop", {{"session_id", another}, {"task_id", task}})["is_error"].toBool());
        auto waiting = api.dispatch("agent.shell.output", {{"session_id", id}, {"task_id", task}, {"timeout", 30000}}, firstToken);
        const auto stop = call(api, "agent.shell.stop", {{"session_id", id}, {"task_id", task}});
        CTEST_CHECK(!stop["is_error"].toBool());
        CTEST_COMPARE(waiting.result.get().toObject()["result"].toObject()["task"].toObject()["status"], "killed");
        CTEST_COMPARE(call(api, "agent.shell.list", {{"session_id", id}})["result"].toObject()["tasks"].toArray().size(), 1);
        call(api, "agent.cancel", {{"request_id", run.requestId}}); CTEST_COMPARE(run.result.get().toObject()["status"], "cancelled");
    }
    void taskStateIsAuthenticatedAndAccessibleDuringRuns() {
        QTemporaryDir root; auto o = options(root); o.engine.taskToolsEnabled = true;
        auto model = std::make_shared<Model>();
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Default,
            QList<a::PermissionRule>{{"TaskUpdate", a::PermissionBehavior::Deny}});
        a::Api api(model, std::make_shared<a::ToolRegistry>(), policy, o);
        CTEST_CHECK(call(api, "agent.info")["task_tools_enabled"].toBool());
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        auto run = api.dispatch("agent.run", {{"session_id", id}, {"prompt", "wait"}}, firstToken);
        CTEST_WAIT_VERIFY(model->waiting.load(), 3000);
        const QJsonObject input{{"session_id", id}, {"subject", "Verify"}, {"description", "Check the actual result"}};
        const auto created = call(api, "agent.tasks.create", input);
        CTEST_CHECK(!created["is_error"].toBool()); CTEST_COMPARE(created["result"].toObject()["task"].toObject()["id"], "1");
        error([&] { call(api, "agent.tasks.list", {{"session_id", id}}, secondToken); }, ErrorCode::NotFound);
        error([&] { call(api, "agent.tasks.create", input, "invalid"); }, ErrorCode::Unauthorized);
        auto changed = call(api, "agent.tasks.update", {{"session_id", id}, {"taskId", "1"}, {"status", "completed"}});
        CTEST_CHECK(changed["is_error"].toBool());
        CTEST_CHECK(call(api, "agent.tasks.create", {{"session_id", id}, {"subject", "no description"}})["is_error"].toBool());
        CTEST_COMPARE(call(api, "agent.tasks.list", {{"session_id", id}})["result"].toObject()["tasks"].toArray().size(), 1);
        const QJsonArray todos{QJsonObject{{"content", "Test"}, {"activeForm", "Testing"}, {"status", "pending"}}};
        CTEST_CHECK(!call(api, "agent.todos.write", {{"session_id", id}, {"todos", todos}})["is_error"].toBool());
        CTEST_COMPARE(call(api, "agent.todos.get", {{"session_id", id}})["result"].toObject()["todos"].toArray(), todos);
        call(api, "agent.cancel", {{"request_id", run.requestId}});
        CTEST_COMPARE(run.result.get().toObject()["status"], "cancelled");
        api.close();
        a::Api reopened(model, std::make_shared<a::ToolRegistry>(), policy, o);
        CTEST_COMPARE(call(reopened, "agent.tasks.list", {{"session_id", id}})["result"].toObject()["tasks"].toArray().size(), 1);
    }
    void manualCompactionUsesAuthenticatedRunLifecycle() {
        QTemporaryDir root; auto o = options(root); auto model = std::make_shared<Model>(); model->budgets = true;
        a::Api api(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        call(api, "agent.run", {{"session_id", id}, {"prompt", QString(1000, 'a')}});
        call(api, "agent.run", {{"session_id", id}, {"prompt", QString(1000, 'b')}});
        call(api, "agent.run", {{"session_id", id}, {"prompt", "current input"}});
        const QJsonObject p{{"session_id", id}, {"instructions", "Keep pending work and source IDs"}};
        error([&] { call(api, "agent.sessions.compact", p, secondToken); }, ErrorCode::NotFound);
        model->waitForSummary = true;
        auto pending = api.dispatch("agent.sessions.compact", p, firstToken);
        CTEST_WAIT_VERIFY(model->waiting.load(), 3000);
        CTEST_COMPARE(call(api, "agent.status", {{"request_id", pending.requestId}}).value("state").toString(), "running");
        call(api, "agent.cancel", {{"request_id", pending.requestId}});
        CTEST_COMPARE(pending.result.get().toObject().value("status").toString(), "cancelled");
        CTEST_COMPARE(call(api, "agent.sessions.get", {{"session_id", id}}).value("compaction_count").toInt(-1), 0);
        model->waitForSummary = false;
        const auto result = call(api, "agent.sessions.compact", p);
        CTEST_COMPARE(result.value("status").toString(), "completed"); CTEST_COMPARE(result.value("turns").toInt(-1), 0);
        CTEST_COMPARE(result.value("usage").toObject().value("compactions").toInt(), 1);
        const auto session = call(api, "agent.sessions.get", {{"session_id", id}});
        CTEST_COMPARE(session.value("message_count").toInt(), 6); CTEST_COMPARE(session.value("compaction_count").toInt(), 1);
        CTEST_CHECK(!session.value("compaction").toObject().value("summary").toString().isEmpty());
        error([&] { call(api, "agent.sessions.compact", {{"session_id", id}, {"workspace", root.path()}}); }, ErrorCode::InvalidArgument);
    }
    void projectContextIsAuthenticatedAndWorkspaceBound() {
        QTemporaryDir root; auto o = options(root); auto model = std::make_shared<Model>();
        auto put = [](const QString& path, const QByteArray& text) { QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); CTEST_CHECK(f.open(QIODevice::WriteOnly)); CTEST_COMPARE(f.write(text), text.size()); };
        put(QDir(o.workingDirectory).filePath("AGENTS.md"), "Root rule");
        put(QDir(o.workingDirectory).filePath("src/AGENTS.md"), "Nested rule");
        a::Api api(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        const QJsonObject scope{{"session_id", id}, {"context_paths", QJsonArray{"src/new.cpp"}}};
        CTEST_COMPARE(call(api, "agent.context.get", scope).value("files").toArray().size(), 2);
        error([&] { call(api, "agent.context.get", scope, secondToken); }, ErrorCode::NotFound);
        error([&] { call(api, "agent.context.get", scope, "wrong"); }, ErrorCode::Unauthorized);
        error([&] { call(api, "agent.context.get", {{"session_id", id}, {"context_paths", QJsonArray{"../private"}}}); }, ErrorCode::InvalidArgument);
        error([&] { call(api, "agent.context.get", {{"session_id", id}, {"context_paths", "src/new.cpp"}}); }, ErrorCode::InvalidArgument);
        error([&] { call(api, "agent.context.get", {{"session_id", id}, {"context_paths", QJsonArray{1}}}); }, ErrorCode::InvalidArgument);
        auto run = scope; run["prompt"] = "inspect rules";
        CTEST_CHECK(call(api, "agent.run", run).value("text").toString().contains("Nested rule"));
        CTEST_COMPARE(call(api, "agent.context.get", {{"session_id", id}}).value("files").toArray().size(), 2);
        auto invalid = o; invalid.engine.projectContext.rootDirectory = root.path();
        error([&] { a::Api rejected(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), invalid); }, ErrorCode::InvalidArgument);
    }
    void authenticationIsolationAndRestart() {
        QTemporaryDir root; auto o = options(root); auto model = std::make_shared<Model>();
        const auto registry = std::make_shared<a::ToolRegistry>(); auto policy = std::make_shared<a::RulePolicy>();
        QString id;
        {
            a::Api api(model, registry, policy, o);
            error([&] { call(api, "agent.info", {}, "wrong"); }, ErrorCode::Unauthorized);
            CTEST_COMPARE(call(api, "agent.info")["client_id"].toString(), "society");
            id = call(api, "agent.sessions.create", {{"model", "fixture"}, {"system", "private instruction"}})["session_id"].toString();
            CTEST_CHECK(!id.isEmpty());
            CTEST_COMPARE(call(api, "agent.run", {{"session_id", id}, {"prompt", "first"}})["text"].toString(), "first");
            error([&] { call(api, "agent.sessions.get", {{"session_id", id}}, secondToken); }, ErrorCode::NotFound);
            CTEST_COMPARE(call(api, "agent.sessions.list", {}, secondToken)["sessions"].toArray().size(), 0);
            error([&] { call(api, "agent.sessions.create", {{"model", "fixture"}, {"workspace", root.path()}}); }, ErrorCode::InvalidArgument);
        }
        a::Api restored(model, registry, policy, o);
        CTEST_COMPARE(call(restored, "agent.sessions.list")["sessions"].toArray().size(), 1);
        CTEST_COMPARE(call(restored, "agent.run", {{"session_id", id}, {"prompt", "second"}})["text"].toString(), "first|second");
    }
    void cancellationAndAdmission() {
        QTemporaryDir root; auto o = options(root); o.maxConcurrentRequests = 1; o.maxQueuedRequests = 0;
        auto model = std::make_shared<Model>();
        a::Api api(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        auto pending = api.dispatch("agent.run", {{"session_id", id}, {"prompt", "wait"}}, firstToken);
        CTEST_WAIT_DEFAULT(model->waiting.load());
        error([&] { call(api, "agent.info"); }, ErrorCode::QueueFull);
        error([&] { call(api, "agent.cancel", {{"request_id", pending.requestId}}, secondToken); }, ErrorCode::NotFound);
        CTEST_CHECK(call(api, "agent.cancel", {{"request_id", pending.requestId}})["cancel_requested"].toBool());
        CTEST_CHECK(pending.result.wait_for(3s) == std::future_status::ready);
        CTEST_CHECK(model->cancelled.load());
        CTEST_COMPARE(pending.result.get().toObject()["status"].toString(), "cancelled");
    }
    void forkPreservesCompleteBoundary() {
        QTemporaryDir root; a::SessionStore store(root.path());
        const auto original = store.create("fixture", "system", root.path());
        {
            auto lease = store.acquire(original.id);
            lease->append({"u", a::MessageRole::User, "read"});
            lease->append({"a", a::MessageRole::Assistant, {}, {{"call", "Read", {{"path", "value"}}}}});
            lease->append({"t", a::MessageRole::Tool, "observed", {}, "call"});
            lease->append({"done", a::MessageRole::Assistant, "answer"});
            error([&] { store.fork(original.id); }, ErrorCode::ModelInUse);
        }
        error([&] { store.fork(original.id, "a"); }, ErrorCode::InvalidArgument);
        error([&] { store.fork(original.id, "missing"); }, ErrorCode::NotFound);
        const auto fork = store.fork(original.id, "t");
        CTEST_CHECK(fork.id != original.id); CTEST_COMPARE(fork.messages.size(), 3);
        CTEST_CHECK(a::pendingToolCalls(fork.messages).isEmpty()); CTEST_COMPARE(store.list().size(), 2);
        CTEST_COMPARE(store.load(original.id).messages.size(), 4); CTEST_COMPARE(store.load(fork.id).messages, fork.messages);
        auto lease = store.acquire(fork.id); lease->append({"next", a::MessageRole::User, "continue"});
        CTEST_COMPARE(store.load(original.id).messages.size(), 4);
    }
    void runAdmissionStillValidatesTranscriptBody() {
        QTemporaryDir root; auto o = options(root); auto model = std::make_shared<Model>();
        a::Api api(model, std::make_shared<a::ToolRegistry>(), std::make_shared<a::RulePolicy>(), o);
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}})["session_id"].toString();
        QDirIterator files(o.stateDirectory, {"transcript.jsonl"}, QDir::Files, QDirIterator::Subdirectories);
        CTEST_CHECK(files.hasNext()); const auto transcript = files.next(); CTEST_CHECK(!files.hasNext());
        QFile file(transcript); CTEST_CHECK(file.open(QIODevice::Append));
        const QByteArray invalid("{\"type\":\"message\",\"parent_id\":\"missing\"}\n");
        CTEST_COMPARE(file.write(invalid), qint64(invalid.size())); file.close();
        const auto result = call(api, "agent.run", {{"session_id", id}, {"prompt", "wait"}});
        CTEST_COMPARE(result["error_code"], "protocol_error"); CTEST_COMPARE(result["status"], "failed");
        CTEST_CHECK(!model->waiting.load());
    }
    void limitsQueueAndPrivateState() {
        QTemporaryDir root; auto o = options(root); auto model = std::make_shared<Model>();
        auto registry = std::make_shared<a::ToolRegistry>(); auto policy = std::make_shared<a::RulePolicy>();
        auto bad = o; bad.workingDirectory = QDir::rootPath();
        error([&] { a::Api api(model, registry, policy, bad); }, ErrorCode::InvalidArgument);
        bad = o; bad.stateDirectory = QDir(o.workingDirectory).filePath("state");
        error([&] { a::Api api(model, registry, policy, bad); }, ErrorCode::InvalidArgument);
        o.maxConcurrentRequests = 1; o.maxQueuedRequests = 1; o.maxSessionsPerClient = 2; o.requestTimeoutMs = 150;
        a::Api api(model, registry, policy, o);
        error([&] { a::Api duplicate(model, registry, policy, o); }, ErrorCode::AlreadyExists);
        const auto id = call(api, "agent.sessions.create", {{"model", "fixture"}}).value("session_id");
        const auto fork = call(api, "agent.sessions.fork", {{"session_id", id}}).value("session_id"); CTEST_CHECK(id != fork);
        error([&] { call(api, "agent.sessions.create", {{"model", "fixture"}}); }, ErrorCode::ResourceLimit);
        const auto page = call(api, "agent.sessions.list", {{"limit", 1}}); CTEST_COMPARE(page["sessions"].toArray().size(), 1);
        const auto next = call(api, "agent.sessions.list", {{"cursor", page.value("next_cursor")}, {"limit", 1}});
        CTEST_COMPARE(next["sessions"].toArray().size(), 1); CTEST_CHECK(!next.contains("next_cursor"));
        auto active = api.dispatch("agent.run", {{"session_id", id}, {"prompt", "wait"}}, firstToken); CTEST_WAIT_DEFAULT(model->waiting.load());
        auto queued = api.dispatch("agent.sessions.create", {{"model", "fixture"}}, secondToken); queued.cancel();
        try {
            CTEST_COMPARE(call(api, "agent.status", {{"request_id", active.requestId}})["state"].toString(), "running");
        } catch (const Error& e) {
            // The deliberate 150 ms deadline may expire before this status
            // request is scheduled. Completion removes active request IDs.
            CTEST_COMPARE(e.code(), ErrorCode::NotFound);
            CTEST_CHECK(active.result.wait_for(1s) == std::future_status::ready);
        }
        error([&] { (void)active.result.get(); }, ErrorCode::Timeout);
        error([&] { (void)queued.result.get(); }, ErrorCode::Cancelled);
        CTEST_COMPARE(call(api, "agent.sessions.list", {}, secondToken)["sessions"].toArray().size(), 0);
        api.close(); error([&] { call(api, "agent.info"); }, ErrorCode::ShuttingDown);
    }
    void forkCopiesArtifactsAndOversizedHeaderIsAtomic() {
        QTemporaryDir root; a::SessionStore store(root.path(), 1024);
        error([&] { store.create("fixture", QString(2048, 'x'), root.path()); }, ErrorCode::ResourceLimit);
        CTEST_CHECK(store.list().isEmpty());
        CTEST_COMPARE(QDir(root.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        const auto source = store.create("fixture", {}, root.path());
        { auto lease = store.acquire(source.id); QDir().mkpath(lease->artifactsDirectory());
            QFile file(QDir(lease->artifactsDirectory()).filePath("result.txt")); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("observation"); }
        const auto child=store.fork(source.id);QFile copy(root.filePath(child.id+"/artifacts/result.txt"));
        CTEST_CHECK(copy.open(QIODevice::ReadOnly));CTEST_COMPARE(copy.readAll(),"observation");
        CTEST_COMPARE(store.list().size(), 2);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    AgentApiTests tests;
    runner.run("graphqlDirectApiUsesOwnedSessionsAndPermission", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.graphqlDirectApiUsesOwnedSessionsAndPermission(); });
    runner.run("teamRoutesUseAuthenticatedSessionIdentity", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.teamRoutesUseAuthenticatedSessionIdentity(); });
    runner.run("permissionsInspectionIsAuthenticatedAndCannotChangePolicy", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.permissionsInspectionIsAuthenticatedAndCannotChangePolicy(); });
    runner.run("liveProfilesShareHostConfigurationButRespectClientIdentity", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.liveProfilesShareHostConfigurationButRespectClientIdentity(); });
    runner.run("childGenerationUsesHostOptionsIndependentlyOfParentRequests", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.childGenerationUsesHostOptionsIndependentlyOfParentRequests(); });
    runner.run("subagentControlsAreAuthenticatedAndRemainResponsive", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.subagentControlsAreAuthenticatedAndRemainResponsive(); });
    runner.run("skillsAreClientScopedAndRunWithoutPlaceholderPrompt", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.skillsAreClientScopedAndRunWithoutPlaceholderPrompt(); });
    runner.run("inputsReachAnActiveRunWithoutAnAvailableRunWorker", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.inputsReachAnActiveRunWithoutAnAvailableRunWorker(); });
    runner.run("shellOutputHonorsApiDeadlineWithoutStoppingCommand", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.shellOutputHonorsApiDeadlineWithoutStoppingCommand(); });
    runner.run("backgroundShellsAreAuthenticatedAndControlledDuringRuns", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.backgroundShellsAreAuthenticatedAndControlledDuringRuns(); });
    runner.run("taskStateIsAuthenticatedAndAccessibleDuringRuns", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.taskStateIsAuthenticatedAndAccessibleDuringRuns(); });
    runner.run("manualCompactionUsesAuthenticatedRunLifecycle", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.manualCompactionUsesAuthenticatedRunLifecycle(); });
    runner.run("projectContextIsAuthenticatedAndWorkspaceBound", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.projectContextIsAuthenticatedAndWorkspaceBound(); });
    runner.run("authenticationIsolationAndRestart", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.authenticationIsolationAndRestart(); });
    runner.run("cancellationAndAdmission", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.cancellationAndAdmission(); });
    runner.run("forkPreservesCompleteBoundary", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.forkPreservesCompleteBoundary(); });
    runner.run("runAdmissionStillValidatesTranscriptBody", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.runAdmissionStillValidatesTranscriptBody(); });
    runner.run("limitsQueueAndPrivateState", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.limitsQueueAndPrivateState(); });
    runner.run("forkCopiesArtifactsAndOversizedHeaderIsAtomic", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.forkCopiesArtifactsAndOversizedHeaderIsAtomic(); });
    return runner.result();
}

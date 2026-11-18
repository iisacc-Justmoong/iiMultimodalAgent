#include "CTestSupport.h"
#include "QtEventWait.h"
#include "iiMultimodalAgent/agent/Engine.h"
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QDir>
#include <atomic>
#include <chrono>
#include <thread>
#include <condition_variable>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a = iiMultimodalAgent::agent;
using namespace std::chrono_literals;

static QJsonObject schema() {
    return {{"type", "object"}, {"properties", QJsonObject{{"value", QJsonObject{{"type", "integer"}, {"minimum", 1}}}}},
        {"required", QJsonArray{"value"}}, {"additionalProperties", false}};
}
static a::Tool echoTool(QString name = "echo") {
    a::Tool tool;
    tool.definition = {name, "Return the supplied integer", schema(), schema(), true, true};
    tool.execute = [](const QJsonObject& input, const a::ToolContext&) { return a::ToolResult{QString::number(input["value"].toInt()), input}; };
    return tool;
}
class ScriptModel : public a::Model {
public:
    QList<a::ModelReply> replies;
    QList<a::ModelRequest> requests;
    std::function<void(const a::ModelRequest&)> observe;
    std::function<std::optional<ContextBudget>(const a::ModelRequest&)> inspectMeasure;
    std::optional<ContextBudget> measure(const a::ModelRequest& request,const CancellationToken&) override {
        return inspectMeasure?inspectMeasure(request):std::nullopt;
    }
    a::ModelReply generate(const a::ModelRequest& request, const CancellationToken& token,
            const std::function<bool(const QString&)>& delta) override {
        token.throwIfCancelled();
        requests.append(request);
        if (observe) observe(request);
        if (replies.isEmpty()) throw Error(ErrorCode::RuntimeFailure, "Unexpected model call");
        auto reply = replies.takeFirst();
        if (delta && !delta(reply.text)) token.throwIfCancelled();
        return reply;
    }
};
#include "agent_tests.h"
void AgentTests::catalogBudgetUsesCurrentInputAndDoesNotReplaceGenerationHistory() {
    QTemporaryDir root;auto model=std::make_shared<ScriptModel>();auto registry=std::make_shared<a::ToolRegistry>();
    registry->add(echoTool());registry->add(echoTool("another"));bool validProbe=false,completeTurnProbe=false;
    model->inspectMeasure=[&](const a::ModelRequest& request)->std::optional<ContextBudget> {
        if(request.messages.isEmpty()||(request.messages.last().role!=a::MessageRole::User&&request.messages.last().role!=a::MessageRole::Tool))
            throw Error(ErrorCode::InvalidArgument,"Native measurement requires a complete conversation boundary");
        if(request.messages.size()==1){validProbe=request.messages.last().text=="current request";if(!validProbe)throw Error(ErrorCode::InvalidArgument,"Catalog probe changed current input");}
        if(request.messages.size()==3&&request.messages.first().text=="current request") {
            completeTurnProbe=request.messages.last().role==a::MessageRole::Tool&&a::pendingToolCalls(request.messages).isEmpty();
            if(!completeTurnProbe)throw Error(ErrorCode::ProtocolError,"Catalog probe split the current tool turn");
        }
        return ContextBudget{100,4096};
    };
    model->replies={{{},{{"echo-call","echo",{{"value",1}}}}},{"done",{}}};a::EngineOptions options{.decision={.enabled=false}};
    options.sessionsDirectory=root.filePath("sessions");options.skills.enabled=false;options.taskToolsEnabled=false;
    options.projectContext.enabled=false;options.compaction.automatic=false;
    a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(),options);const auto session=engine.createSession("fixture",root.path());
    {a::SessionStore store(options.sessionsDirectory);auto lease=store.acquire(session.id);lease->append({{},a::MessageRole::User,"old history"});lease->append({{},a::MessageRole::Assistant,"old answer"});}
    const auto result=engine.run({session.id,"current request"}).result.get();CTEST_COMPARE(result.status,a::RunStatus::Completed);CTEST_CHECK(validProbe);CTEST_CHECK(completeTurnProbe);
    CTEST_COMPARE(model->requests.size(),2);CTEST_COMPARE(model->requests.first().messages.size(),3);CTEST_COMPARE(model->requests.last().messages.size(),5);
    CTEST_COMPARE(model->requests.first().messages.first().text,"old history");CTEST_COMPARE(model->requests.first().messages.last().text,"current request");
}
void AgentTests::exactObservationRepairUsesTheExistingTurnLimit() {
    for(const int limit:{2,3})for(const auto& wrong:QStringList{"auxiliary-hash","The contents are: observed\n\"한글\""}) {
        QTemporaryDir root;auto model=std::make_shared<ScriptModel>();auto registry=std::make_shared<a::ToolRegistry>();
        a::Tool read;read.definition={"Read","Read current contents",{{"type","object"}}};
        read.execute=[](const QJsonObject&,const a::ToolContext&){return a::ToolResult{"observed\n\"한글\"",{{"sha256","auxiliary-hash"},{"complete",true}}};};registry->add(read);
        model->replies={{{},{{"read","Read",{}}}},{wrong,{}},{"observed\n\"한글\"",{}}};
        a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=root.filePath("sessions");options.projectContext.enabled=false;
        a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),options);
        const auto session=engine.createSession("fixture",root.path());a::RunRequest request{session.id,"Use Read. Return only the exact file contents."};request.maxTurns=limit;
        const auto result=engine.run(request).result.get();CTEST_COMPARE(result.turns,limit);
        CTEST_COMPARE(result.status,limit==2?a::RunStatus::TurnLimit:a::RunStatus::Completed);
        if(limit==3)CTEST_COMPARE(result.text,"observed\n\"한글\"");
        CTEST_CHECK(a::pendingToolCalls(engine.session(session.id).messages).isEmpty());
        bool recorded=false;for(const auto& message:engine.session(session.id).messages)recorded|=message.metadata.contains("iilocal.observation_repair");CTEST_CHECK(recorded);
    }
}
void AgentTests::exactFieldRepairRequiresAnUnambiguousObservation() {
    for(const bool ambiguous:{false,true}) {
        QTemporaryDir root;auto model=std::make_shared<ScriptModel>();auto registry=std::make_shared<a::ToolRegistry>();
        a::Tool lookup;lookup.definition={"Inspect","Inspect an object",{{"type","object"}}};
        lookup.execute=[ambiguous](const QJsonObject&,const a::ToolContext&){
            QJsonObject data{{"item",QJsonObject{{"name","auxiliary-name"},{"description","actual description 한글"}}}};
            if(ambiguous)data.insert("another",QJsonObject{{"description","another candidate"}});
            return a::ToolResult{QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)),data};
        };registry->add(lookup);
        model->replies={{{},{{"inspect","Inspect",{}}}},{"auxiliary-name",{}},{"actual description 한글",{}}};
        a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=root.filePath("sessions");options.projectContext.enabled=false;
        a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),options);
        const auto session=engine.createSession("fixture",root.path());a::RunRequest request{session.id,"Use Inspect. Return only the exact description from its actual result."};request.maxTurns=3;
        const auto result=engine.run(request).result.get();CTEST_COMPARE(result.status,a::RunStatus::Completed);
        CTEST_COMPARE(result.turns,ambiguous?2:3);CTEST_COMPARE(result.text,ambiguous?"auxiliary-name":"actual description 한글");
        bool repaired=false;for(const auto& message:engine.session(session.id).messages)repaired|=message.metadata.contains("iilocal.observation_repair");
        CTEST_COMPARE(repaired,!ambiguous);CTEST_CHECK(a::pendingToolCalls(engine.session(session.id).messages).isEmpty());
    }
}
void AgentTests::exactValueUsesDecodedTextBeforeProtocolMetadata() {
    QTemporaryDir root;auto model=std::make_shared<ScriptModel>();auto registry=std::make_shared<a::ToolRegistry>();
    a::Tool inspect;inspect.definition={"Inspect","Read a value",{{"type","object"}}};
    inspect.execute=[](const QJsonObject&,const a::ToolContext&){return a::ToolResult{R"({"value":"actual value"})",{{"protocol","auxiliary"}}};};registry->add(inspect);
    model->replies={{{},{{"inspect","Inspect",{}}}},{"auxiliary",{}},{"actual value",{}}};
    a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=root.filePath("sessions");options.projectContext.enabled=false;
    a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),options);
    const auto session=engine.createSession("fixture",root.path());a::RunRequest request{session.id,"Use Inspect. Return only the exact value."};request.maxTurns=3;
    const auto result=engine.run(request).result.get();CTEST_COMPARE(result.status,a::RunStatus::Completed);CTEST_COMPARE(result.turns,3);CTEST_COMPARE(result.text,"actual value");
}
void AgentTests::exactJsonGeneratedByCustomModelsIsDecodedWithoutAnotherToolCall() {
    for(const bool correct:{false,true}) {
        QTemporaryDir root;auto model=std::make_shared<ScriptModel>();auto registry=std::make_shared<a::ToolRegistry>();
        a::Tool inspect;inspect.definition={"Inspect","Read a field",{{"type","object"}}};int calls=0;
        inspect.execute=[&](const auto&,const auto&){++calls;return a::ToolResult{R"({"secret":"actual value","metadata":"unrelated"})"};};registry->add(inspect);
        model->replies={{{},{{"inspect","Inspect",{}}}},{correct?R"({"secret":"actual value","metadata":"unrelated"})":R"({"secret":"wrong value","metadata":"actual value"})",{}},{"actual value",{}}};
        a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=root.filePath("sessions");options.projectContext.enabled=false;
        a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),options);
        const auto session=engine.createSession("fixture",root.path());a::RunRequest request{session.id,"Use Inspect. Return only the exact secret field."};request.maxTurns=3;
        const auto result=engine.run(request).result.get();CTEST_COMPARE(result.status,a::RunStatus::Completed);CTEST_COMPARE(result.text,"actual value");
        CTEST_COMPARE(result.turns,correct?2:3);CTEST_COMPARE(calls,1);
    }
}
void AgentTests::singleToolLimitIsPassedToModels(){
    for(const int limit:{1,8}){
        QTemporaryDir root;auto model=std::make_shared<ScriptModel>();auto registry=std::make_shared<a::ToolRegistry>();registry->add(echoTool());
        model->replies={{{},{{"first","echo",{{"value",1}}}}},{"done",{}}};
        a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=root.filePath("sessions");options.maxToolCallsPerTurn=limit;
        a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(),options);const auto session=engine.createSession("fixture",root.path());
        CTEST_COMPARE(engine.run({session.id,"Observe and finish"}).result.get().status,a::RunStatus::Completed);
        CTEST_COMPARE(model->requests.size(),2);for(const auto& request:model->requests)CTEST_COMPARE(request.parallelToolCalls,limit>1);
    }
}
void AgentTests::completionToolStopsBeforeLaterTools() {
    QTemporaryDir root;auto model=std::make_shared<ScriptModel>();auto registry=std::make_shared<a::ToolRegistry>();
    QList<int> executed;auto regular=echoTool();regular.execute=[&](const QJsonObject& args,const auto&) {
        executed.append(args["value"].toInt());return a::ToolResult{QString::number(args["value"].toInt()),args,false,{},{{"completes_run",true}}};
    };registry->add(regular);
    auto terminal=echoTool("StructuredOutput");terminal.completesRun=true;registry->add(terminal);
    a::ToolRunner runner(registry,std::make_shared<a::RulePolicy>());
    CTEST_CHECK(!runner.concurrencySafe({"done","StructuredOutput",{{"value",2}}}));
    model->replies={{{},{{"before","echo",{{"value",1}}},{"done","StructuredOutput",{{"value",2}}},{"after","echo",{{"value",3}}}}}};
    a::EngineOptions options{.decision={.enabled=false}};options.sessionsDirectory=root.filePath("sessions");options.skills.enabled=false;options.projectContext.enabled=false;
    a::Engine engine(model,registry,std::make_shared<a::RulePolicy>(),options);auto session=engine.createSession("model://test",root.path());
    const auto result=engine.run({session.id,"Return the verified result"}).result.get();
    CTEST_COMPARE(result.status,a::RunStatus::Completed);CTEST_COMPARE(result.text,"2");CTEST_COMPARE(model->requests.size(),1);CTEST_COMPARE(executed,QList<int>{1});
    CTEST_CHECK(a::pendingToolCalls(engine.session(session.id).messages).isEmpty());
    terminal.execute=[](const QJsonObject&,const auto&){return a::ToolResult{"INVALID",{},true};};registry->remove("StructuredOutput");registry->add(terminal);
    model->replies={{{},{{"invalid","StructuredOutput",{{"value",2}}},{"regular","echo",{{"value",4}}}}},{"NEXT_TURN",{}}};
    session=engine.createSession("model://test",root.path());
    CTEST_COMPARE(engine.run({session.id,"Retry an invalid result"}).result.get().text,"NEXT_TURN");CTEST_COMPARE(executed,QList<int>({1,4}));
}
void AgentTests::verifierPermissionModeDoesNotInheritBypass() {
    auto tool=echoTool();tool.definition.readOnly=false;tool.definition.editsFiles=true;
    a::ToolContext verifier;verifier.permissionMode=a::PermissionMode::DontAsk;
    for(const auto mode:{a::PermissionMode::AcceptEdits,a::PermissionMode::Bypass}) {
        a::RulePolicy policy(mode);CTEST_COMPARE(policy.decide(tool.definition,{},{}).behavior,a::PermissionBehavior::Allow);
        CTEST_COMPARE(policy.decide(tool.definition,{},verifier).behavior,a::PermissionBehavior::Deny);
        CTEST_COMPARE(policy.describe(verifier)["mode"],"dontAsk");CTEST_CHECK(policy.describe({})["mode"]!="dontAsk");
        a::RulePolicy granted(mode,{{"echo",a::PermissionBehavior::Allow}});
        CTEST_COMPARE(granted.decide(tool.definition,{},verifier).behavior,a::PermissionBehavior::Allow);
        a::RulePolicy denied(mode,{{"echo",a::PermissionBehavior::Allow},{"echo",a::PermissionBehavior::Deny}});
        CTEST_COMPARE(denied.decide(tool.definition,{},verifier).behavior,a::PermissionBehavior::Deny);
    }
}
void AgentTests::registrySnapshotDuringExecution() {
    auto registry = std::make_shared<a::ToolRegistry>(); registry->add(echoTool());
    a::ToolRunnerOptions runnerOptions;
    runnerOptions.hooks.append([&](const a::HookInput& input, const CancellationToken&) {
        if (input.kind == a::HookKind::BeforeTool) registry->remove("echo");
        return a::HookResult{};
    });
    a::ToolRunner runner(registry, std::make_shared<a::RulePolicy>(), runnerOptions);
    const auto result = runner.run({"bound-call", "echo", {{"value", 7}}}, {});
    CTEST_CHECK(!result.isError); CTEST_COMPARE(result.text, "7"); CTEST_CHECK(registry->definitions().isEmpty());
    registry->add(echoTool());
    auto model = std::make_shared<ScriptModel>();
    model->replies = {{{}, {{"frozen-call", "echo", {{"value", 9}}}}}, {"done", {}}};
    model->observe = [&](const a::ModelRequest&) {
        if (model->requests.size() == 1) {
            registry->remove("echo"); auto replacement = echoTool(); replacement.definition.readOnly = false;
            replacement.execute = [](const auto&, const auto&) { return a::ToolResult{"wrong replacement", {}, true}; };
            registry->add(std::move(replacement));
        }
    };
    QTemporaryDir root;
    a::EngineOptions options{.decision={.enabled=false}}; options.sessionsDirectory = root.filePath("sessions");
    a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
    const auto session = engine.createSession("model://test", root.path());
    CTEST_COMPARE(engine.run({session.id, "call the registered tool"}).result.get().status, a::RunStatus::Completed);
    CTEST_COMPARE(engine.session(session.id).messages[2].text, "9");
    CTEST_CHECK(!model->requests.last().tools.first().readOnly); // Registry changes become visible next model turn.
}
void AgentTests::schemaAndRegistry() {
        a::ToolRegistry registry;
        registry.add(echoTool());
        registry.validateInput("echo", {{"value", 2}});
        CTEST_THROWS(Error, registry.validateInput("echo", {{"value", "2"}}));
        CTEST_THROWS(Error, registry.validateInput("echo", {{"value", 2}, {"extra", true}}));
        CTEST_THROWS(Error, registry.validateOutput("echo", {{"value", 0}}));
        CTEST_THROWS(Error, registry.add(echoTool()));
        auto conditional = echoTool("conditional");
        conditional.definition.inputSchema = QJsonDocument::fromJson(R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","allOf":[{"properties":{"value":{"$ref":"#/$defs/positive"}}}],"$defs":{"positive":{"type":"integer","minimum":1}},"required":["value"],"unevaluatedProperties":false})").object();
        registry.add(conditional);
        registry.validateInput("conditional", {{"value", 1}});
        CTEST_THROWS(Error, registry.validateInput("conditional", {{"value", 1}, {"x", 0}}));
    }
void AgentTests::permissionsAndHookRevalidation() {
        auto registry = std::make_shared<a::ToolRegistry>();
        int executions = 0;
        auto tool = echoTool();
        tool.execute = [&](const auto& input, const auto&) { ++executions; return a::ToolResult{"ok", input}; };
        registry->add(tool);
        auto policy = std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass,
            QList<a::PermissionRule>{{"*", a::PermissionBehavior::Allow}, {"echo", a::PermissionBehavior::Deny}});
        a::ToolRunner denied(registry, policy);
        CTEST_CHECK(denied.run({"a", "echo", {{"value", 1}}}, {}).isError);
        CTEST_COMPARE(executions, 0);
        a::ToolRunnerOptions options;
        options.hooks.append([](const a::HookInput& input, const CancellationToken&) {
            a::HookResult r;
            if (input.kind == a::HookKind::BeforeTool) r.updatedArguments = QJsonObject{{"value", -1}};
            return r;
        });
        a::ToolRunner invalid(registry, std::make_shared<a::RulePolicy>(), options);
        CTEST_CHECK(invalid.run({"b", "echo", {{"value", 1}}}, {}).isError);
        CTEST_COMPARE(executions, 0);
        tool.definition.readOnly = false;
        a::RulePolicy dontAsk(a::PermissionMode::DontAsk);
        CTEST_CHECK(dontAsk.decide(tool.definition, {}, {}).behavior == a::PermissionBehavior::Deny);
    }
void AgentTests::transcriptInvariantsAndRecovery() {
        QTemporaryDir dir;
        a::SessionStore store(dir.path());
        const auto session = store.create("model://test", "system", dir.path());
        {
            auto lease = store.acquire(session.id);
            lease->append({{}, a::MessageRole::User, "hello"});
            lease->append({{}, a::MessageRole::Assistant, {}, {{"call-a", "echo", {{"value", 1}}}}});
            CTEST_THROWS(Error, store.acquire(session.id));
            CTEST_THROWS(Error, lease->append({{}, a::MessageRole::Tool, "bad", {}, "other"}));
        }
        QFile file(dir.filePath(session.id + "/transcript.jsonl"));
        CTEST_CHECK(file.open(QIODevice::Append));
        file.write("{\"incomplete\":");
        file.close();
        const auto loaded = store.load(session.id);
        CTEST_COMPARE(loaded.messages.size(), 2);
        CTEST_COMPARE(a::pendingToolCalls(loaded.messages).size(), 1);
        CTEST_THROWS(Error, store.load("../outside"));
    }
void AgentTests::toolLoopAndPersistentResume() {
        QTemporaryDir dir;
        auto registry = std::make_shared<a::ToolRegistry>(); registry->add(echoTool());
        auto model = std::make_shared<ScriptModel>();
        model->replies = {{"", {{"c1", "echo", {{"value", 7}}}}}, {"seven", {}}};
        a::EngineOptions options{.decision={.enabled=false}}; options.sessionsDirectory = dir.path();
        auto engine = std::make_unique<a::Engine>(model, registry, std::make_shared<a::RulePolicy>(), options);
        auto session = engine->createSession("model://test", dir.path());
        QList<a::EventKind> events;
        auto handle = engine->run({session.id, "Return seven"}, [&](const auto& e) { events.append(e.kind); });
        const auto result = handle.result.get();
        CTEST_CHECK_MESSAGE(result.status == a::RunStatus::Completed, qPrintable(result.errorMessage));
        CTEST_COMPARE(result.text, QString("seven"));
        CTEST_COMPARE(result.turns, 2);
        CTEST_COMPARE(model->requests.size(), 2);
        CTEST_COMPARE(model->requests[1].messages.back().toolCallId, QString("c1"));
        CTEST_CHECK(events.contains(a::EventKind::ToolStarted));
        CTEST_CHECK(events.back() == a::EventKind::Finished);
        engine.reset();
        a::SessionStore store(dir.path());
        const auto restored = store.load(session.id);
        CTEST_COMPARE(restored.messages.size(), 4);
        CTEST_CHECK(a::pendingToolCalls(restored.messages).isEmpty());
    }
void AgentTests::interruptedToolIsNotExecutedAgain() {
        QTemporaryDir dir;
        a::SessionStore store(dir.path());
        auto session = store.create("model://test", {}, dir.path());
        {
            auto lease = store.acquire(session.id);
            lease->append({{}, a::MessageRole::User, "previous action"});
            lease->append({{}, a::MessageRole::Assistant, {}, {{"interrupted", "echo", {{"value", 9}}}}});
        }
        auto registry = std::make_shared<a::ToolRegistry>(); registry->add(echoTool());
        auto model = std::make_shared<ScriptModel>(); model->replies = {{"resumed", {}}};
        a::EngineOptions options{.decision={.enabled=false}}; options.sessionsDirectory = dir.path();
        a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
        const auto result = engine.run({session.id, "continue"}).result.get();
        CTEST_CHECK(result.status == a::RunStatus::Completed);
        const auto history = engine.session(session.id).messages;
        CTEST_COMPARE(history[2].toolCallId, QString("interrupted"));
        CTEST_CHECK(history[2].isError);
        CTEST_CHECK(a::pendingToolCalls(history).isEmpty());
    }
void AgentTests::schemaFailureBecomesToolResult() {
        QTemporaryDir dir;
        auto registry = std::make_shared<a::ToolRegistry>(); registry->add(echoTool());
        auto model = std::make_shared<ScriptModel>();
        model->replies = {{"", {{"bad", "echo", {{"value", "wrong"}}}}}, {"handled", {}}};
        a::EngineOptions options{.decision={.enabled=false}}; options.sessionsDirectory = dir.path();
        a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
        auto session = engine.createSession("model://test", dir.path());
        CTEST_CHECK(engine.run({session.id, "test"}).result.get().status == a::RunStatus::Completed);
        CTEST_CHECK(model->requests[1].messages.back().isError);
    }
void AgentTests::cancellationAndConsumerFailure() {
        class WaitingModel : public a::Model {
        public:
            std::atomic_bool started = false;
            a::ModelReply generate(const a::ModelRequest&, const CancellationToken& c,
                    const std::function<bool(const QString&)>&) override {
                started = true;
                for (;;) { c.throwIfCancelled(); std::this_thread::sleep_for(1ms); }
            }
        };
        QTemporaryDir dir;
        auto model = std::make_shared<WaitingModel>();
        auto registry = std::make_shared<a::ToolRegistry>();
        a::EngineOptions options{.decision={.enabled=false}}; options.sessionsDirectory = dir.path();
        a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
        auto session = engine.createSession("model://test", dir.path());
        auto handle = engine.run({session.id, "wait"});
        CTEST_WAIT_VERIFY(model->started.load(), 3000);
        handle.cancel();
        CTEST_CHECK(handle.result.wait_for(2s) == std::future_status::ready);
        CTEST_CHECK(handle.result.get().status == a::RunStatus::Cancelled);
        auto failing = engine.run({session.id, "fail"}, [](const auto&) { throw std::runtime_error("consumer"); });
        CTEST_CHECK(failing.result.get().errorCode == ErrorCode::ConsumerFailure);
    }
void AgentTests::workspaceReadEditAndStaleGuard() {
        QTemporaryDir dir;
        auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, dir.path());
        a::ToolRunner runner(registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
        a::ToolContext context; context.sessionId = "test"; context.workingDirectory = dir.path();
        context.artifactsDirectory = dir.filePath("artifacts");
        auto call = [&](QString name, QJsonObject args) { return runner.run({name, name, args}, context); };
        CTEST_CHECK(!call("Write", {{"path", "a.txt"}, {"content", "alpha\nbeta\n"}}).isError);
        CTEST_CHECK(!call("Read", {{"path", "a.txt"}}).isError);
        QFile file(dir.filePath("a.txt")); CTEST_CHECK(file.open(QIODevice::WriteOnly)); file.write("external\n"); file.close();
        CTEST_CHECK(call("Edit", {{"path", "a.txt"}, {"old_string", "alpha"}, {"new_string", "changed"}}).isError);
        CTEST_CHECK(!call("Read", {{"path", "a.txt"}}).isError);
        CTEST_CHECK(!call("Edit", {{"path", "a.txt"}, {"old_string", "external"}, {"new_string", "changed"}}).isError);
        CTEST_CHECK(file.open(QIODevice::ReadOnly)); CTEST_COMPARE(file.readAll(), QByteArray("changed\n")); file.close();
        CTEST_CHECK(call("Read", {{"path", "../outside"}}).isError);
        CTEST_CHECK(!call("Grep", {{"pattern", "changed"}}).isError);
    }
void AgentTests::parallelToolsAndExclusiveBarrier() {
    QTemporaryDir dir;
    auto registry = std::make_shared<a::ToolRegistry>();
    std::mutex mutex; std::condition_variable changed; int active = 0, arrived = 0, peak = 0; bool exclusive = false;
    for (const auto* name : {"first", "second", "exclusive"}) {
        auto tool = echoTool(name);
        const bool isExclusive = QString(name) == "exclusive";
        tool.definition.concurrencySafe = !isExclusive;
        tool.execute = [&, isExclusive](const QJsonObject& input, const a::ToolContext& c) {
            std::unique_lock lock(mutex);
            if (isExclusive) exclusive = active == 0 && arrived == 2;
            ++active; peak = std::max(peak, active);
            if (!isExclusive) {
                ++arrived; changed.notify_all();
                changed.wait_for(lock, 2s, [&] { return arrived == 2 || c.cancellation.isCancelled(); });
            }
            --active;
            return a::ToolResult{"ok", input};
        };
        registry->add(std::move(tool));
    }
    auto model = std::make_shared<ScriptModel>();
    model->replies = {{"", {{"one", "first", {{"value", 1}}}, {"two", "second", {{"value", 2}}}, {"three", "exclusive", {{"value", 3}}}}}, {"done", {}}};
    a::EngineOptions options{.decision={.enabled=false}}; options.sessionsDirectory = dir.path();
    a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
    auto session = engine.createSession("model://test", dir.path());
    CTEST_CHECK(engine.run({session.id, "parallel"}).result.get().status == a::RunStatus::Completed);
    CTEST_COMPARE(peak, 2); CTEST_CHECK(exclusive);
    CTEST_CHECK(a::pendingToolCalls(engine.session(session.id).messages).isEmpty());
}
void AgentTests::stopHookAndTurnLimit() {
    QTemporaryDir dir;
    auto registry = std::make_shared<a::ToolRegistry>(); registry->add(echoTool());
    auto model = std::make_shared<ScriptModel>(); model->replies = {{"first", {}}, {"revised", {}}};
    a::EngineOptions options{.decision={.enabled=false}}; options.sessionsDirectory = dir.path(); int stops = 0;
    options.hooks.append([&](const a::HookInput& input, const CancellationToken&) {
        a::HookResult r;
        if (input.kind == a::HookKind::Stop && ++stops == 1) { r.block = true; r.feedback = "Revise the answer"; }
        return r;
    });
    a::Engine engine(model, registry, std::make_shared<a::RulePolicy>(), options);
    auto session = engine.createSession("model://test", dir.path());
    const auto revised = engine.run({session.id, "answer"}).result.get();
    CTEST_CHECK(revised.status == a::RunStatus::Completed); CTEST_COMPARE(revised.text, QString("revised")); CTEST_COMPARE(revised.turns, 2);
    model->replies = {{"", {{"last-call", "echo", {{"value", 5}}}}}};
    a::RunRequest request{session.id, "one turn"}; request.maxTurns = 1;
    const auto limited = engine.run(request).result.get();
    CTEST_CHECK(limited.status == a::RunStatus::TurnLimit);
    CTEST_CHECK(a::pendingToolCalls(engine.session(session.id).messages).isEmpty());
}
void AgentTests::shellTimeoutAndExitStatus() {
#ifdef Q_OS_UNIX
    QTemporaryDir dir;
    auto registry = std::make_shared<a::ToolRegistry>(); a::registerWorkspaceTools(*registry, dir.path());
    a::ToolRunner runner(registry, std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
    a::ToolContext c; c.workingDirectory = dir.path(); c.sessionId = "shell-test";
    auto success = runner.run({"ok", "Bash", {{"command", "printf 'native-shell'"}}}, c);
    CTEST_CHECK(!success.isError); CTEST_COMPARE(success.text, QString("native-shell"));
    auto error = runner.run({"err", "Bash", {{"command", "printf 'failed' >&2; exit 7"}}}, c);
    CTEST_CHECK(error.isError); CTEST_COMPARE(error.data["exit_code"].toInt(), 7);
    auto timeout = runner.run({"timeout", "Bash", {{"command", "sleep 30"}, {"timeout_ms", 30}}}, c);
    CTEST_CHECK(timeout.isError); CTEST_COMPARE(timeout.data["error_code"].toString(), QString("timeout"));
#endif
}
void AgentTests::invalidResultsAndDuplicateCalls() {
    auto registry = std::make_shared<a::ToolRegistry>();
    auto tool = echoTool(); tool.execute = [](const auto&, const auto&) { return a::ToolResult{"invalid", {{"value", -5}}}; };
    registry->add(tool);
    a::ToolRunner runner(registry, std::make_shared<a::RulePolicy>());
    CTEST_CHECK(runner.run({"bad-result", "echo", {{"value", 1}}}, {}).isError);
    const auto duplicate = a::toJson(a::ToolCall{"same", "echo", {}});
    CTEST_THROWS(Error, a::replyFromJson(QJsonObject{{"tool_calls", QJsonArray{duplicate, duplicate}}}));
    CTEST_THROWS(Error, a::replyFromJson({{"text", "ok"}, {"unknown", true}}));
}
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    AgentTests tests;
    runner.run("exactObservationRepairUsesTheExistingTurnLimit", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.exactObservationRepairUsesTheExistingTurnLimit(); });
    runner.run("exactFieldRepairRequiresAnUnambiguousObservation", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.exactFieldRepairRequiresAnUnambiguousObservation(); });
    runner.run("exactValueUsesDecodedTextBeforeProtocolMetadata", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.exactValueUsesDecodedTextBeforeProtocolMetadata(); });
    runner.run("exactJsonGeneratedByCustomModelsIsDecodedWithoutAnotherToolCall", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.exactJsonGeneratedByCustomModelsIsDecodedWithoutAnotherToolCall(); });
    runner.run("catalogBudgetUsesCurrentInputAndDoesNotReplaceGenerationHistory", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.catalogBudgetUsesCurrentInputAndDoesNotReplaceGenerationHistory(); });
    runner.run("singleToolLimitIsPassedToModels", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.singleToolLimitIsPassedToModels(); });
    runner.run("completionToolStopsBeforeLaterTools", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.completionToolStopsBeforeLaterTools(); });
    runner.run("verifierPermissionModeDoesNotInheritBypass", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.verifierPermissionModeDoesNotInheritBypass(); });
    runner.run("schemaAndRegistry", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.schemaAndRegistry(); });
    runner.run("permissionsAndHookRevalidation", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.permissionsAndHookRevalidation(); });
    runner.run("transcriptInvariantsAndRecovery", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.transcriptInvariantsAndRecovery(); });
    runner.run("toolLoopAndPersistentResume", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.toolLoopAndPersistentResume(); });
    runner.run("interruptedToolIsNotExecutedAgain", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.interruptedToolIsNotExecutedAgain(); });
    runner.run("schemaFailureBecomesToolResult", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.schemaFailureBecomesToolResult(); });
    runner.run("cancellationAndConsumerFailure", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.cancellationAndConsumerFailure(); });
    runner.run("workspaceReadEditAndStaleGuard", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.workspaceReadEditAndStaleGuard(); });
    runner.run("parallelToolsAndExclusiveBarrier", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.parallelToolsAndExclusiveBarrier(); });
    runner.run("stopHookAndTurnLimit", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.stopHookAndTurnLimit(); });
    runner.run("shellTimeoutAndExitStatus", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.shellTimeoutAndExitStatus(); });
    runner.run("invalidResultsAndDuplicateCalls", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.invalidResultsAndDuplicateCalls(); });
    runner.run("registrySnapshotDuringExecution", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.registrySnapshotDuringExecution(); });
    return runner.result();
}

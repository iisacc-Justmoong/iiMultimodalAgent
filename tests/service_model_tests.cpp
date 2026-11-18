#include <iiLocalLLM.h>
#include <iiMultimodalAgent/agent/Engine.h>
#include "fixtures/Utf8Stream.h"
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QUuid>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QLocalSocket>
#include <atomic>
#include <chrono>
#include <thread>

using namespace iiLocalLLM; using namespace iiMultimodalAgent;
using namespace std::chrono_literals;

struct Probe {
    std::atomic<int> loads{0}, unloads{0}, contexts{0}, destroyed{0}, active{0}, peak{0};
    std::atomic<int> preparedDestroyedAfterUnload{0};
    quint64 memoryBytes = 128 * 1024 * 1024;
    std::atomic<bool> started{false}, release{true}, fail{false};
    QString answer = QStringLiteral("가나다");
    QList<ConversationRequest> preparedConversations;
    qint64 nativeInputTokens=0;
};
class FakePromptState final : public RuntimePromptState {
public:
    explicit FakePromptState(std::shared_ptr<Probe> probe) : p(std::move(probe)), unloaded(p->unloads.load()) {}
    ~FakePromptState() override { if (p->unloads.load() > unloaded) ++p->preparedDestroyedAfterUnload; }
private:
    std::shared_ptr<Probe> p;
    int unloaded;
};

class FakeContext final : public RuntimeContext {
public:
    explicit FakeContext(std::shared_ptr<Probe> probe) : p(std::move(probe)) { ++p->contexts; }
    ~FakeContext() override { ++p->destroyed; }
    RuntimeResult generateConversation(const RuntimeConversationPrompt& prompt, const GenerationOptions& options,
        const CancellationToken& cancel, const TextCallback& emitText) override {
        return generate(prompt.tokens, options, cancel, emitText);
    }
    RuntimeResult generate(const TokenList& prompt, const GenerationOptions&,
                           const CancellationToken& cancel, const TextCallback& emitText) override
    {
        ++p->active;
        p->peak = std::max(p->peak.load(), p->active.load());
        struct Exit { Probe* p; ~Exit() { --p->active; } } exit{p.get()};
        p->started = true;
        while (!p->release) { cancel.throwIfCancelled(); std::this_thread::sleep_for(1ms); }
        cancel.throwIfCancelled();
        if (p->fail) throw Error(ErrorCode::RuntimeFailure, QStringLiteral("injected failure"));
        int reuse = 0;
        while (reuse < std::min(previous.size(), prompt.size()) && previous[reuse] == prompt[reuse]) ++reuse;
        for (const QChar ch : p->answer) {
            cancel.throwIfCancelled();
            if (!emitText(QString(ch))) break;
        }
        previous = prompt;
        return {FinishReason::Stop, 3, reuse};
    }
private:
    std::shared_ptr<Probe> p;
    TokenList previous;
};

class FakeModel final : public RuntimeModel {
public:
    explicit FakeModel(std::shared_ptr<Probe> probe) : p(std::move(probe)) {}
    ~FakeModel() override { ++p->unloads; }
    RuntimeConversationPrompt prepareConversation(const ConversationRequest& request, const CancellationToken&) override {
        p->preparedConversations.append(request);
        TokenList tokens;
        for (const auto& message : request.messages)
            for (const auto ch : message.toObject()["content"].toString()) tokens.append(ch.unicode());
        tokens.append(3);
        return {tokens, {}, std::make_shared<FakePromptState>(p),p->nativeInputTokens};
    }
    RuntimeConversationReply parseConversation(const RuntimeConversationPrompt&, const QString& text) override {
        const auto reply = QJsonDocument::fromJson(text.toUtf8()).object();
        return {reply["text"].toString(), reply["reasoning"].toString(), reply["tool_calls"].toArray()};
    }
    TokenList tokenize(const QList<ChatMessage>& messages, const CancellationToken&) override
    {
        TokenList tokens;
        for (const auto& message : messages) {
            tokens.push_back(message.role == Role::System ? 1 : message.role == Role::User ? 2 : 3);
            for (const auto ch : message.content) tokens.push_back(ch.unicode());
        }
        tokens.push_back(3);
        return tokens;
    }
    std::unique_ptr<RuntimeContext> createContext(const CancellationToken&) override
    { return std::make_unique<FakeContext>(p); }
private:
    std::shared_ptr<Probe> p;
};

class FakeRuntime final : public Runtime {
public:
    explicit FakeRuntime(std::shared_ptr<Probe> probe) : p(std::move(probe)) {}
    QString id() const override { return QStringLiteral("test"); }
    bool supportsModel(const ModelSpec& spec) const override { return spec.format == QStringLiteral("test"); }
    QList<RuntimeDevice> devices(const HardwareInfo&) const override { return {{ComputeBackend::Cpu, {}}}; }
    MemoryEstimate estimateMemory(const ModelSpec&, int) const override { return {p->memoryBytes, 0, 0, "test reservation"}; }
    std::shared_ptr<RuntimeModel> load(const ModelSpec&, const RuntimeDevice&, const CancellationToken&) override
    { ++p->loads; return std::make_shared<FakeModel>(p); }
private:
    std::shared_ptr<Probe> p;
};

class ServiceModelTests : public QObject {
private:
    std::unique_ptr<QTemporaryDir> storage;
    ServiceOptions options() const
    {
        ServiceOptions result;
        result.modelsDirectory = storage->filePath(QStringLiteral("Models"));
        result.keepAliveMs = 300000;
        return result;
    }
    static void installFixture(Service& service, const QString& id = QStringLiteral("small"), int context = 128,
                               const QStringList& capabilities = {"text-generation", "chat"})
    {
        QTemporaryDir package(QDir::current().filePath(QStringLiteral("bundle-XXXXXX")));
        if (!package.isValid()) throw std::runtime_error("Cannot create test package");
        ModelManifest manifest{id, "test", "test", "none", context, capabilities, "model.test"};
        QFile metadata(package.filePath(QStringLiteral("manifest.json")));
        if (!metadata.open(QIODevice::WriteOnly) || metadata.write(QJsonDocument(manifestObject(manifest)).toJson()) < 1)
            throw std::runtime_error("Cannot write test manifest");
        metadata.close();
        QFile weights(package.filePath(QStringLiteral("model.test")));
        if (!weights.open(QIODevice::WriteOnly) || weights.write("fixture") != 7) throw std::runtime_error("Cannot write test weights");
        weights.close();
        (void)service.installModel(package.path()).get();
    }
    static void setup(Service& service, const std::shared_ptr<Probe>& p, int context = 128)
    {
        service.registerRuntime(std::make_shared<FakeRuntime>(p)).get();
        installFixture(service, QStringLiteral("small"), context);
        (void)service.loadModel({QStringLiteral("model://small"), context}).get();
    }
    static ChatRequest request(const QString& session, const QString& prompt = QStringLiteral("hello"))
    {
        ChatRequest r;
        r.sessionId = session;
        r.prompt = prompt;
        r.options.maxTokens = 8;
        return r;
    }
public:
    void init()
    {
        storage = std::make_unique<QTemporaryDir>(QDir::current().filePath(QStringLiteral("store-XXXXXX")));
        CTEST_CHECK(storage->isValid());
    }
    void cleanup() { storage.reset(); }
    void copiedObservationsUseModelGeneratedJsonAndPreserveThePublicStream() {
        auto p=std::make_shared<Probe>();Service service(options());setup(service,p,8192);agent::ServiceModel model(service);
        const auto observed=QString::fromUtf8("actual\n한글");
        const auto serialized=QString::fromUtf8(QJsonDocument(QJsonObject{{"value",observed}}).toJson(QJsonDocument::Compact));
        p->answer=QString::fromUtf8(QJsonDocument(QJsonObject{{"text",serialized}}).toJson(QJsonDocument::Compact));
        agent::ModelRequest request;request.model="model://small";request.generation.maxTokens=64;
        agent::Message repair{{},agent::MessageRole::User,"Copy the actual observation"};
        repair.metadata={{"iilocal.observation_repair",true},{"iilocal.observation_value",observed}};
        request.messages={{{},agent::MessageRole::User,"Return only the exact value"},repair};
        CTEST_CHECK(model.measure(request,{}));const auto measured=p->preparedConversations.last().responseSchema;
        QString streamed;CTEST_COMPARE(model.generate(request,{},[&](const auto& piece){streamed+=piece;return true;}).text,observed);
        CTEST_COMPARE(streamed,observed);CTEST_COMPARE(p->preparedConversations.last().responseSchema,measured);
        CTEST_COMPARE(measured["properties"].toObject()["value"].toObject()["enum"].toArray(),QJsonArray{observed});
        p->answer=R"({"text":"not generated JSON"})";
        try{model.generate(request,{},{});agent_ctest::fail(__FILE__, __LINE__, "Invalid generated output was replaced by the observation");}
        catch(const Error& error){CTEST_COMPARE(error.code(),ErrorCode::ProtocolError);}
        request.responseSchema={{"type","object"}};p->answer="{\"text\":\"host format\"}";
        CTEST_COMPARE(model.generate(request,{},{}).text,"host format");CTEST_COMPARE(p->preparedConversations.last().responseSchema,request.responseSchema);
    }
    void singularAndPluralFileContentsUseTheSameMeasuredCompletionContract() {
        auto p=std::make_shared<Probe>();Service service(options());setup(service,p,2048);agent::ServiceModel model(service);
        agent::ModelRequest request;request.model="model://small";request.generation.maxTokens=192;
        request.tools={{"Read","Read a file",{{"type","object"}}},{"Other","Another tool",{{"type","object"}}}};
        for(const auto& suffix:QStringList{"content","contents"}) {
            request.messages={{{},agent::MessageRole::User,"Use the Read tool to read secret.txt. Then reply with the exact file "+suffix+"."},
                {{},agent::MessageRole::Assistant,{},{{"read","Read",{{"path","secret.txt"}}}}},
                {{},agent::MessageRole::Tool,"actual observed value",{},"read",false,{{"sha256","auxiliary digest"}}}};
            CTEST_CHECK(model.measure(request,{}));const auto native=p->preparedConversations.last();
            CTEST_COMPARE(native.toolChoice,"none");CTEST_CHECK(native.tools.isEmpty());
            CTEST_COMPARE(native.responseSchema["properties"].toObject()["value"].toObject()["enum"].toArray(),QJsonArray{"actual observed value"});
        }
        request.messages.first().text="Use the Read tool to read secret.txt. Then reply with the exact file content hash.";
        model.measure(request,{});CTEST_CHECK(p->preparedConversations.last().responseSchema.isEmpty());
    }
    void directFileReadIsExplicitButGeneralOrQuotedProseIsNot() {
        auto p=std::make_shared<Probe>();Service service(options());setup(service,p,8192);agent::ServiceModel model(service);
        agent::ModelRequest request;request.model="model://small";request.generation.maxTokens=64;
        request.tools={{"Read","Read a file",{{"type","object"}}},{"ToolSearch","Find definitions",{{"type","object"}}}};
        for(const auto& text:QStringList{"Read secret.txt and return only its exact contents.","Read the file \"folder/evidence.txt\"."}) {
            request.messages={{{},agent::MessageRole::User,text}};CTEST_CHECK(model.measure(request,{}));
            const auto native=p->preparedConversations.last();CTEST_COMPARE(native.toolChoice,"required");CTEST_COMPARE(native.tools.size(),1);
            CTEST_COMPARE(native.tools.first().toObject()["function"].toObject()["name"],"Read");
            request.messages.append({{},agent::MessageRole::Assistant,{},{{"read","Read",{{"path","secret.txt"}}}}});
            request.messages.append({{},agent::MessageRole::Tool,"file content",{},"read"});
            model.measure(request,{});CTEST_COMPARE(p->preparedConversations.last().toolChoice,text.contains("exact contents")?QString("none"):QString("auto"));
        }
        for(const auto& text:QStringList{"Read about tools.","Do not Read secret.txt.","Example: `Read secret.txt`.","\"Read secret.txt\" is a quotation."}) {
            request.messages={{{},agent::MessageRole::User,text}};model.measure(request,{});CTEST_COMPARE(p->preparedConversations.last().toolChoice,"auto");
        }
    }
    void loadedSkillMayPutContextBeforeAnExplicitToolSentence() {
        auto p=std::make_shared<Probe>();Service service(options());setup(service,p,8192);agent::ServiceModel model(service);
        agent::ModelRequest request;request.model="model://small";request.generation.maxTokens=64;
        request.tools={{"Read","Read a file",{{"type","object"}}}};
        agent::Message skill{{},agent::MessageRole::User,"Skill: inspect\n\nPrivate context parent=123. Use Read to read fork.txt and return its exact contents."};
        skill.metadata={{"iilocal.skill",QJsonObject{{"name","inspect"}}}};request.messages={skill};
        model.measure(request,{});CTEST_COMPARE(p->preparedConversations.last().toolChoice,"required");
        request.messages[0].metadata={};model.measure(request,{});CTEST_COMPARE(p->preparedConversations.last().toolChoice,"auto");
        request.messages[0].metadata=skill.metadata;request.messages[0].text="Skill: inspect\n\nContext. Do not use Read. Example: `Use Read`.";
        model.measure(request,{});CTEST_COMPARE(p->preparedConversations.last().toolChoice,"auto");
    }
    void explicitReadPathsUseOnlyTheCurrentInstructionAndIntersectHostSchemas() {
        auto p=std::make_shared<Probe>();Service service(options());setup(service,p,8192);agent::ServiceModel model(service);
        const QJsonObject schema{{"type","object"},{"properties",QJsonObject{
            {"path",QJsonObject{{"type","string"}}},{"offset",QJsonObject{{"type","integer"}}},{"limit",QJsonObject{{"type","integer"}}}}},
            {"required",QJsonArray{"path"}},{"additionalProperties",false}};
        agent::ModelRequest request;request.model="model://small";request.generation.maxTokens=64;request.tools={{"Read","Read a file",schema}};
        auto parameters=[&] {model.measure(request,{});return p->preparedConversations.last().tools.first().toObject()["function"].toObject()["parameters"].toObject();};
        for(const auto& path:QStringList{"fork.txt",R"(C:\Folder With Spaces\Evidence.txt)"}) {
            agent::Message skill{{},agent::MessageRole::User,"Skill: inspect\n\nContext. Use Read to read \""+path+"\" and return its exact contents.\n\nSkill resource directory: /unrelated/resources"};
            skill.metadata={{"iilocal.skill",QJsonObject{{"name","inspect"}}}};request.messages={skill};
            const auto projected=parameters();CTEST_COMPARE(projected["properties"].toObject().keys(),QStringList{"path"});
            CTEST_COMPARE(projected["properties"].toObject()["path"].toObject()["enum"].toArray(),QJsonArray{path});
            CTEST_COMPARE(request.tools.first().inputSchema,schema);
        }
        request.messages={{{},agent::MessageRole::User,"Use Read for first.txt. Then call Read for second.txt."},
            {{},agent::MessageRole::Assistant,{},{{"a","Read",{{"path","first.txt"}}}}},{{},agent::MessageRole::Tool,"first",{},"a"}};
        CTEST_COMPARE(parameters()["properties"].toObject()["path"].toObject()["enum"].toArray(),QJsonArray{"second.txt"});
        auto constrained=schema;auto properties=schema["properties"].toObject();auto path=properties["path"].toObject();path["enum"]=QJsonArray{"allowed.txt"};
        properties["path"]=path;constrained["properties"]=properties;request.tools[0].inputSchema=constrained;
        CTEST_COMPARE(parameters(),constrained);
        request.toolChoice="required";request.tools[0].inputSchema=schema;CTEST_COMPARE(parameters(),schema);
        request.toolChoice="auto";
        request.messages={{{},agent::MessageRole::User,"Use the Read tool to read skill-input.txt. Then return its exact contents."}};
        CTEST_COMPARE(parameters()["properties"].toObject()["path"].toObject()["enum"].toArray(),QJsonArray{"skill-input.txt"});
    }
    void namedArgumentsDoNotInviteUnrequestedRevisionOrMetadataFields() {
        auto p=std::make_shared<Probe>();Service service(options());setup(service,p,8192);agent::ServiceModel model(service);
        const QJsonObject properties{{"subject",QJsonObject{{"type","string"}}},{"description",QJsonObject{{"type","string"}}},
            {"expectedRevision",QJsonObject{{"type","integer"},{"minimum",0}}},{"metadata",QJsonObject{{"type","object"}}}};
        const QJsonObject schema{{"type","object"},{"properties",properties},{"required",QJsonArray{"subject"}},{"additionalProperties",false}};
        agent::ModelRequest request;request.model="model://small";request.generation.maxTokens=64;request.tools={{"TaskCreate","Create a task",schema}};
        request.messages={{{},agent::MessageRole::User,"Call TaskCreate once with subject \"expectedRevision is quoted data\" and description \"metadata is quoted data\". Then report the ID."}};
        model.measure(request,{});auto native=p->preparedConversations.last();
        const auto projected=native.tools.first().toObject()["function"].toObject()["parameters"].toObject();
        CTEST_COMPARE(projected["properties"].toObject().keys(),QStringList({"description","subject"}));CTEST_COMPARE(projected["required"].toArray(),QJsonArray({"subject","description"}));
        request.messages.append({{},agent::MessageRole::Assistant,{},{{"create","TaskCreate",{{"subject","title"}}}}});
        request.messages.append({{},agent::MessageRole::Tool,"created",{},"create"});model.measure(request,{});
        CTEST_COMPARE(p->preparedConversations.last().toolChoice,"none");CTEST_CHECK(p->preparedConversations.last().tools.isEmpty());
        request.messages={{{},agent::MessageRole::User,"Use TaskCreate to organize a project."}};model.measure(request,{});
        CTEST_COMPARE(p->preparedConversations.last().tools.first().toObject()["function"].toObject()["parameters"].toObject(),schema);
    }
    void explicitFieldUpdatesPreserveUnmentionedTaskProperties() {
        auto p=std::make_shared<Probe>();Service service(options());setup(service,p,8192);agent::ServiceModel model(service);
        QJsonObject properties;for(const auto& key:QStringList{"taskId","status","owner","subject","description","expectedRevision"})
            properties.insert(key,QJsonObject{{"type","string"}});
        const QJsonObject schema{{"type","object"},{"properties",properties},{"required",QJsonArray{"taskId"}},{"additionalProperties",false}};
        agent::ModelRequest request;request.model="model://small";request.generation.maxTokens=64;request.tools={{"TaskUpdate","Update a task",schema}};
        request.messages={{{},agent::MessageRole::User,"Use TaskUpdate for taskId \"1\" to set status \"in_progress\" and owner \"builder\". Change only these two fields, then confirm the update."}};
        model.measure(request,{});const auto native=p->preparedConversations.last();
        const auto projected=native.tools.first().toObject()["function"].toObject()["parameters"].toObject();
        CTEST_COMPARE(projected["properties"].toObject().keys(),QStringList({"owner","status","taskId"}));
        CTEST_COMPARE(projected["required"].toArray(),QJsonArray({"taskId","owner","status"}));CTEST_COMPARE(projected["additionalProperties"],false);
    }
    void repeatedNamedCommandsAreCountedAndHistoricalReadsNeedNoNewTool() {
        auto p=std::make_shared<Probe>();Service service(options());setup(service,p,8192);agent::ServiceModel model(service);
        agent::ModelRequest request;request.model="model://small";request.generation.maxTokens=64;
        request.tools={{"Read","Read a file",{{"type","object"}}}};
        request.messages={{{},agent::MessageRole::User,"Use Read for first.txt. Then call Read for second.txt."},
            {{},agent::MessageRole::Assistant,{},{{"a","Read",{{"path","first.txt"}}}}},{{},agent::MessageRole::Tool,"first",{},"a"}};
        model.measure(request,{});CTEST_COMPARE(p->preparedConversations.last().toolChoice,"required");
        request.messages.append({{},agent::MessageRole::Assistant,{},{{"b","Read",{{"path","second.txt"}}}}});
        request.messages.append({{},agent::MessageRole::Tool,"old observation",{},"b"});
        request.messages.append({{},agent::MessageRole::User,"What text did you read from second.txt? Repeat it."});
        model.measure(request,{});const auto native=p->preparedConversations.last();CTEST_COMPARE(native.toolChoice,"none");CTEST_CHECK(native.tools.isEmpty());
        CTEST_COMPARE(native.responseSchema["properties"].toObject()["value"].toObject()["enum"].toArray(),QJsonArray{"old observation"});
    }
    void nativeMediaBudgetAndToolObservationsUseThePublicServiceAdapter() {
        auto p=std::make_shared<Probe>();p->nativeInputTokens=80;p->answer="{\"text\":\"observed\"}";
        Service service(options());setup(service,p,128);agent::ServiceModel model(service);
        const auto image=QJsonObject{{"type","image"},{"mimeType","image/png"},{"data","aW1hZ2U="}};
        agent::ModelRequest request;request.model="model://small";request.generation.maxTokens=8;
        request.messages={{"user",agent::MessageRole::User,"inspect"},{"call",agent::MessageRole::Assistant,{},{{"c","Observe",{}}}},
            {"result",agent::MessageRole::Tool,"exact observation",{},"c",false,{{"verified",true}},QJsonArray{image}}};
        const auto budget=model.measure(request,{});CTEST_CHECK(budget);CTEST_COMPARE(budget->inputTokens,80);CTEST_COMPARE(p->contexts.load(),0);
        const auto result=model.generate(request,{},{});CTEST_COMPARE(result.text,"observed");CTEST_COMPARE(result.usage.promptTokens,80);
        const auto content=p->preparedConversations.last().messages.last().toObject()["content"].toArray();
        CTEST_COMPARE(content.last().toObject(),image);
        const auto observation=QJsonDocument::fromJson(content.first().toObject()["text"].toString().toUtf8()).object();
        CTEST_COMPARE(observation["text"],"exact observation");CTEST_COMPARE(observation["data"].toObject()["verified"],true);
        p->nativeInputTokens=200;
        try{model.generate(request,{},{});agent_ctest::fail(__FILE__, __LINE__, "Native media exceeded context without rejection");}
        catch(const Error& error){CTEST_COMPARE(error.code(),ErrorCode::ContextOverflow);}
    }



    void agentModelHookControlsReachMeasurementAndGeneration()
    {
        auto p=std::make_shared<Probe>();Service service(options());setup(service,p,8192);
        p->answer="{\"text\":\"{\\\"ok\\\":true}\"}";
        agent::ServiceModel model(service);agent::ModelRequest request;request.model="model://small";
        request.generation.maxTokens=64;request.systemPrompt="EXACT_HOST_CONDITION";request.systemPromptOnly=true;
        request.toolChoice="none";request.enableThinking=false;request.parallelToolCalls=false;
        request.responseSchema={{"type","object"},{"properties",QJsonObject{{"ok",QJsonObject{{"type","boolean"}}}}},
            {"required",QJsonArray{"ok"}},{"additionalProperties",false}};
        request.tools={{"ToolSearch","Definition only",{{"type","object"}}}};
        request.messages={{{},agent::MessageRole::User,"Check the condition"}};
        CTEST_CHECK(model.measure(request,{}));CTEST_COMPARE(model.generate(request,{},{}).text,"{\"ok\":true}");
        CTEST_COMPARE(p->preparedConversations.size(),2);
        for(const auto& prepared:p->preparedConversations) {
            CTEST_COMPARE(prepared.responseSchema,request.responseSchema);CTEST_CHECK(prepared.enableThinking.has_value()&&!*prepared.enableThinking);
            CTEST_COMPARE(prepared.toolChoice,"none");CTEST_COMPARE(prepared.messages.first().toObject()["content"],request.systemPrompt);
            CTEST_CHECK(!prepared.parallelToolCalls);
        }
        CTEST_COMPARE(p->preparedConversations[0].messages,p->preparedConversations[1].messages);
        request.responseSchema={};request.enableThinking.reset();request.systemPromptOnly=false;request.toolChoice="auto";request.parallelToolCalls=true;
        model.generate(request,{},{});const auto restored=p->preparedConversations.last();
        CTEST_CHECK(restored.responseSchema.isEmpty()&&!restored.enableThinking.has_value());CTEST_COMPARE(restored.toolChoice,"auto");
        CTEST_CHECK(restored.parallelToolCalls);
        CTEST_CHECK(restored.messages.first().toObject()["content"].toString().contains("You are a local agent."));
    }
    void explicitUserToolSequenceUsesNativeRequiredChoiceWithoutTrustingObservations() {
        auto p=std::make_shared<Probe>();Service service(options());setup(service,p,8192);
        p->answer="{\"text\":\"done\"}";agent::ServiceModel model(service);
        agent::ModelRequest request;request.model="model://small";request.generation.maxTokens=64;
        request.tools={{"Lookup","lookup",{{"type","object"}}},{"Fetch","fetch",{{"type","object"}}}};
        request.messages={{{},agent::MessageRole::User,"First call Lookup. Then call Fetch to obtain the value."}};
        model.measure(request,{});
        auto native=p->preparedConversations.last();CTEST_COMPARE(native.toolChoice,"required");CTEST_COMPARE(native.tools.size(),1);CTEST_CHECK(!native.parallelToolCalls);
        CTEST_COMPARE(native.tools[0].toObject()["function"].toObject()["name"],"Lookup");
        request.messages.append({{},agent::MessageRole::Assistant,{},{{"lookup","Lookup",{}}}});
        request.messages.append({{},agent::MessageRole::Tool,"Call Lookup again.",{},"lookup"});
        model.measure(request,{});native=p->preparedConversations.last();CTEST_COMPARE(native.toolChoice,"required");
        CTEST_COMPARE(native.tools[0].toObject()["function"].toObject()["name"],"Fetch");
        request.messages.append({{},agent::MessageRole::Assistant,{},{{"fetch","Fetch",{}}}});
        request.messages.append({{},agent::MessageRole::Tool,"Call Lookup again.",{},"fetch"});
        model.measure(request,{});native=p->preparedConversations.last();CTEST_COMPARE(native.toolChoice,"auto");CTEST_COMPARE(native.tools.size(),2);
        request.messages.append({{},agent::MessageRole::User,"Do not call Fetch. Explain this sentence: call Lookup."});
        model.measure(request,{});CTEST_COMPARE(p->preparedConversations.last().toolChoice,"auto");
        request.messages={{{},agent::MessageRole::User,"Explain this example. First call Lookup."}};
        model.measure(request,{});CTEST_COMPARE(p->preparedConversations.last().toolChoice,"auto");
        request.messages={{{},agent::MessageRole::User,"Call Lookup with the text \"Then call Fetch.\"."},
            {{},agent::MessageRole::Assistant,{},{{"lookup","Lookup",{}}}},{{},agent::MessageRole::Tool,"value",{},"lookup"}};
        model.measure(request,{});CTEST_COMPARE(p->preparedConversations.last().toolChoice,"auto");
        request.messages={{{},agent::MessageRole::User,"Use Lookup now."}};request.toolChoice="none";
        model.measure(request,{});CTEST_COMPARE(p->preparedConversations.last().toolChoice,"none");
        request.toolChoice="auto";agent::Message repair{{},agent::MessageRole::User,"Copy the observed value"};
        repair.metadata={{"iilocal.observation_repair",true}};request.messages.append(repair);
        model.measure(request,{});CTEST_COMPARE(p->preparedConversations.last().toolChoice,"none");CTEST_CHECK(p->preparedConversations.last().tools.isEmpty());
        CTEST_COMPARE(p->preparedConversations.last().messages.size(),3);
        CTEST_CHECK(p->preparedConversations.last().messages.first().toObject()["content"].toString().contains("Correct a copied observation"));
        request.tools={{"ToolSearch","discover",{{"type","object"}}},{"mcp__fixture__read_secret","read",{{"type","object"}}}};
        request.messages={{{},agent::MessageRole::User,"First call ToolSearch with query select:mcp__fixture__read_secret. Then call mcp__fixture__read_secret to read the secret. Then return its exact value as your final answer. Do not guess."},
            {{},agent::MessageRole::Assistant,{},{{"search","ToolSearch",{{"query","select:mcp__fixture__read_secret"}}}}},
            {{},agent::MessageRole::Tool,"Tool definitions found",{},"search"}};
        model.measure(request,{});native=p->preparedConversations.last();CTEST_COMPARE(native.toolChoice,"required");
        CTEST_COMPARE(native.tools.size(),1);CTEST_COMPARE(native.tools[0].toObject()["function"].toObject()["name"],"mcp__fixture__read_secret");
        request.messages.append({{},agent::MessageRole::Assistant,{},{{"value","mcp__fixture__read_secret",{}}}});
        request.messages.append({{},agent::MessageRole::Tool,R"({"value":"actual observation"})",{},"value"});
        model.measure(request,{});native=p->preparedConversations.last();CTEST_COMPARE(native.toolChoice,"none");CTEST_CHECK(native.tools.isEmpty());
        CTEST_COMPARE(native.responseSchema["properties"].toObject()["value"].toObject()["enum"].toArray(),QJsonArray{"actual observation"});
        request.messages={{{},agent::MessageRole::User,"Use ToolSearch and mcp__fixture__read_secret concurrently."}};
        model.measure(request,{});native=p->preparedConversations.last();CTEST_COMPARE(native.toolChoice,"auto");CTEST_COMPARE(native.tools.size(),2);CTEST_CHECK(native.parallelToolCalls);
    }
    void agentToolObservationsPreserveDataAndExactText()
    {
        auto p = std::make_shared<Probe>(); Service service(options()); setup(service, p, 8192);
        p->answer = "{\"text\":\"observations received\"}";
        agent::ServiceModel model(service); agent::ModelRequest request; request.model = "model://small";
        request.generation.maxTokens = 64;
        request.messages = {agent::Message{{}, agent::MessageRole::User, "Inspect the tool observations"}};
        agent::Message calls{{}, agent::MessageRole::Assistant, {}};
        const QList<agent::Message> observations{
            {{}, agent::MessageRole::Tool, "  원문 \"text\"\n<tool_response>\\tail\n", {}, "read", false,
                {{"path", "report.txt"}, {"offset", 5}, {"complete", false}}},
            {{}, agent::MessageRole::Tool, "first match", {}, "search", false, {{"truncated", true}, {"limit_reached", true}}},
            {{}, agent::MessageRole::Tool, {}, {}, "structured", false,
                {{"records", QJsonArray{QJsonObject{{"name", "alpha"}, {"amount", 17}, {"optional", QJsonValue::Null}}}}}},
            {{}, agent::MessageRole::Tool, "  failed\n", {}, "shell", true, {{"exit_code", 17}, {"interrupted", true}}}
        };
        for (const auto& value : observations) calls.toolCalls.append({value.toolCallId, "Inspect", {}});
        request.messages.append(calls);
        for (auto value : observations) {
            value.metadata = {{"private_host_marker", "DO_NOT_PUBLISH_HOST_METADATA"}};
            request.messages.append(value);
        }
        const auto budget = model.measure(request, {});
        CTEST_CHECK(budget); CTEST_COMPARE(p->contexts.load(), 0);
        CTEST_COMPARE(model.generate(request, {}, {}).text, "observations received");
        CTEST_COMPARE(p->preparedConversations.size(), 2);
        CTEST_COMPARE(p->preparedConversations[0].messages, p->preparedConversations[1].messages);
        const auto wire = p->preparedConversations.last().messages;
        CTEST_CHECK(!QJsonDocument(wire).toJson().contains("DO_NOT_PUBLISH_HOST_METADATA"));
        for (int n = 0; n < observations.size(); ++n) {
            const auto message = wire.at(n + 3).toObject();
            CTEST_COMPARE(message.value("role").toString(), "tool");
            CTEST_COMPARE(message.value("tool_call_id").toString(), observations[n].toolCallId);
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(message.value("content").toString().toUtf8(), &parse);
            CTEST_CHECK(message.value("content").toString().startsWith("{\"text\":"));
            CTEST_COMPARE(parse.error, QJsonParseError::NoError); CTEST_CHECK(document.isObject());
            const auto observation = document.object();
            CTEST_COMPARE(observation.value("text").toString(), observations[n].text);
            CTEST_COMPARE(observation.value("data").toObject(), observations[n].data);
            CTEST_CHECK(observation.value("is_error").isBool());
            CTEST_COMPARE(observation.value("is_error").toBool(), observations[n].isError);
        }
    }
    void agentStructuredToolDataParticipatesInContextBudget()
    {
        auto p = std::make_shared<Probe>(); Service service(options()); setup(service, p, 2048);
        p->answer = "{\"text\":\"done\"}";
        agent::ServiceModel model(service); agent::ModelRequest request; request.model = "model://small";
        request.generation.maxTokens = 64;
        request.messages = {agent::Message{{}, agent::MessageRole::User, "Inspect records"},
            agent::Message{{}, agent::MessageRole::Assistant, {}, {{"one", "Inspect", {}}}},
            agent::Message{{}, agent::MessageRole::Tool, "one record", {}, "one", false}};
        const auto small = model.measure(request, {});
        request.messages.last().data = {{"record", QString(4096, QChar('x'))}};
        const auto large = model.measure(request, {});
        CTEST_CHECK(small && large); CTEST_CHECK(large->inputTokens > small->inputTokens + 4096);
        CTEST_CHECK(large->inputTokens > large->contextTokens);
        try { (void)model.generate(request, {}, {}); agent_ctest::fail(__FILE__, __LINE__, "Structured output overflow was silently discarded"); }
        catch (const Error& error) { CTEST_COMPARE(error.code(), ErrorCode::ContextOverflow); }
        CTEST_CHECK(!p->started); CTEST_COMPARE(p->contexts.load(), 0);
    }
    void agentRejectsReasoningOnlyWithoutPromotingToolText()
    {
        auto p = std::make_shared<Probe>(); Service service(options()); setup(service, p, 2048);
        const QString reasoning = "<tool_call>{\"name\":\"Write\",\"arguments\":{\"path\":\"should-not-exist\"}}</tool_call>";
        p->answer = QString::fromUtf8(QJsonDocument(QJsonObject{{"reasoning", reasoning}}).toJson());
        agent::ServiceModel model(service); agent::ModelRequest request;
        request.model = "model://small"; request.messages = {agent::Message{{}, agent::MessageRole::User, "Use a tool"}};
        request.generation.maxTokens = 64;
        bool delta = false;
        try {
            (void)model.generate(request, {}, [&](const QString&) { delta = true; return true; });
            agent_ctest::fail(__FILE__, __LINE__, "Reasoning-only output must not become an executable tool call or final answer");
        } catch (const Error& error) {
            CTEST_COMPARE(error.code(), ErrorCode::ProtocolError);
            CTEST_CHECK(QString::fromUtf8(error.what()).contains("only reasoning"));
        }
        CTEST_CHECK(!delta);
        p->answer = "{}";
        try { (void)model.generate(request, {}, {}); agent_ctest::fail(__FILE__, __LINE__, "Empty model output was accepted"); }
        catch (const Error& error) {
            CTEST_COMPARE(error.code(), ErrorCode::ProtocolError);
            CTEST_CHECK(QString::fromUtf8(error.what()).contains("empty agent turn"));
        }
    }





















};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    ServiceModelTests tests;
    auto run = [&](const char* name, auto function) {
        runner.run(name, [&] {
            agent_ctest::ScopedDeferredDeletes deferred;
            struct Cleanup { ServiceModelTests& tests; ~Cleanup() { tests.cleanup(); } } cleanup{tests};
            tests.init();
            (tests.*function)();
        });
    };
    run("copiedObservationsUseModelGeneratedJsonAndPreserveThePublicStream", &ServiceModelTests::copiedObservationsUseModelGeneratedJsonAndPreserveThePublicStream);
    run("singularAndPluralFileContentsUseTheSameMeasuredCompletionContract", &ServiceModelTests::singularAndPluralFileContentsUseTheSameMeasuredCompletionContract);
    run("directFileReadIsExplicitButGeneralOrQuotedProseIsNot", &ServiceModelTests::directFileReadIsExplicitButGeneralOrQuotedProseIsNot);
    run("loadedSkillMayPutContextBeforeAnExplicitToolSentence", &ServiceModelTests::loadedSkillMayPutContextBeforeAnExplicitToolSentence);
    run("explicitReadPathsUseOnlyTheCurrentInstructionAndIntersectHostSchemas", &ServiceModelTests::explicitReadPathsUseOnlyTheCurrentInstructionAndIntersectHostSchemas);
    run("namedArgumentsDoNotInviteUnrequestedRevisionOrMetadataFields", &ServiceModelTests::namedArgumentsDoNotInviteUnrequestedRevisionOrMetadataFields);
    run("explicitFieldUpdatesPreserveUnmentionedTaskProperties", &ServiceModelTests::explicitFieldUpdatesPreserveUnmentionedTaskProperties);
    run("repeatedNamedCommandsAreCountedAndHistoricalReadsNeedNoNewTool", &ServiceModelTests::repeatedNamedCommandsAreCountedAndHistoricalReadsNeedNoNewTool);
    run("nativeMediaBudgetAndToolObservationsUseThePublicServiceAdapter", &ServiceModelTests::nativeMediaBudgetAndToolObservationsUseThePublicServiceAdapter);
    run("agentModelHookControlsReachMeasurementAndGeneration", &ServiceModelTests::agentModelHookControlsReachMeasurementAndGeneration);
    run("explicitUserToolSequenceUsesNativeRequiredChoiceWithoutTrustingObservations", &ServiceModelTests::explicitUserToolSequenceUsesNativeRequiredChoiceWithoutTrustingObservations);
    run("agentToolObservationsPreserveDataAndExactText", &ServiceModelTests::agentToolObservationsPreserveDataAndExactText);
    run("agentStructuredToolDataParticipatesInContextBudget", &ServiceModelTests::agentStructuredToolDataParticipatesInContextBudget);
    run("agentRejectsReasoningOnlyWithoutPromotingToolText", &ServiceModelTests::agentRejectsReasoningOnlyWithoutPromotingToolText);
    return runner.result();
}

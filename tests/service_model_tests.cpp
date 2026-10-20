#include <iiLocalLLM.h>
#include <iiMultimodalAgent/agent/Engine.h>
#include "fixtures/Utf8Stream.h"
#include <QtTest/QtTest>
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
    Q_OBJECT
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
private slots:
    void init()
    {
        storage = std::make_unique<QTemporaryDir>(QDir::current().filePath(QStringLiteral("store-XXXXXX")));
        QVERIFY(storage->isValid());
    }
    void cleanup() { storage.reset(); }
    void nativeMediaBudgetAndToolObservationsUseThePublicServiceAdapter() {
        auto p=std::make_shared<Probe>();p->nativeInputTokens=80;p->answer="{\"text\":\"observed\"}";
        Service service(options());setup(service,p,128);agent::ServiceModel model(service);
        const auto image=QJsonObject{{"type","image"},{"mimeType","image/png"},{"data","aW1hZ2U="}};
        agent::ModelRequest request;request.model="model://small";request.generation.maxTokens=8;
        request.messages={{"user",agent::MessageRole::User,"inspect"},{"call",agent::MessageRole::Assistant,{},{{"c","Observe",{}}}},
            {"result",agent::MessageRole::Tool,"exact observation",{},"c",false,{{"verified",true}},QJsonArray{image}}};
        const auto budget=model.measure(request,{});QVERIFY(budget);QCOMPARE(budget->inputTokens,80);QCOMPARE(p->contexts.load(),0);
        const auto result=model.generate(request,{},{});QCOMPARE(result.text,"observed");QCOMPARE(result.usage.promptTokens,80);
        const auto content=p->preparedConversations.last().messages.last().toObject()["content"].toArray();
        QCOMPARE(content.last().toObject(),image);
        const auto observation=QJsonDocument::fromJson(content.first().toObject()["text"].toString().toUtf8()).object();
        QCOMPARE(observation["text"],"exact observation");QCOMPARE(observation["data"].toObject()["verified"],true);
        p->nativeInputTokens=200;
        try{model.generate(request,{},{});QFAIL("Native media exceeded context without rejection");}
        catch(const Error& error){QCOMPARE(error.code(),ErrorCode::ContextOverflow);}
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
        QVERIFY(model.measure(request,{}));QCOMPARE(model.generate(request,{},{}).text,"{\"ok\":true}");
        QCOMPARE(p->preparedConversations.size(),2);
        for(const auto& prepared:p->preparedConversations) {
            QCOMPARE(prepared.responseSchema,request.responseSchema);QVERIFY(prepared.enableThinking.has_value()&&!*prepared.enableThinking);
            QCOMPARE(prepared.toolChoice,"none");QCOMPARE(prepared.messages.first().toObject()["content"],request.systemPrompt);
            QVERIFY(!prepared.parallelToolCalls);
        }
        QCOMPARE(p->preparedConversations[0].messages,p->preparedConversations[1].messages);
        request.responseSchema={};request.enableThinking.reset();request.systemPromptOnly=false;request.toolChoice="auto";request.parallelToolCalls=true;
        model.generate(request,{},{});const auto restored=p->preparedConversations.last();
        QVERIFY(restored.responseSchema.isEmpty()&&!restored.enableThinking.has_value());QCOMPARE(restored.toolChoice,"auto");
        QVERIFY(restored.parallelToolCalls);
        QVERIFY(restored.messages.first().toObject()["content"].toString().contains("You are a local agent."));
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
        QVERIFY(budget); QCOMPARE(p->contexts.load(), 0);
        QCOMPARE(model.generate(request, {}, {}).text, "observations received");
        QCOMPARE(p->preparedConversations.size(), 2);
        QCOMPARE(p->preparedConversations[0].messages, p->preparedConversations[1].messages);
        const auto wire = p->preparedConversations.last().messages;
        QVERIFY(!QJsonDocument(wire).toJson().contains("DO_NOT_PUBLISH_HOST_METADATA"));
        for (int n = 0; n < observations.size(); ++n) {
            const auto message = wire.at(n + 3).toObject();
            QCOMPARE(message.value("role").toString(), "tool");
            QCOMPARE(message.value("tool_call_id").toString(), observations[n].toolCallId);
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(message.value("content").toString().toUtf8(), &parse);
            QCOMPARE(parse.error, QJsonParseError::NoError); QVERIFY(document.isObject());
            const auto observation = document.object();
            QCOMPARE(observation.value("text").toString(), observations[n].text);
            QCOMPARE(observation.value("data").toObject(), observations[n].data);
            QVERIFY(observation.value("is_error").isBool());
            QCOMPARE(observation.value("is_error").toBool(), observations[n].isError);
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
        QVERIFY(small && large); QVERIFY(large->inputTokens > small->inputTokens + 4096);
        QVERIFY(large->inputTokens > large->contextTokens);
        try { (void)model.generate(request, {}, {}); QFAIL("Structured output overflow was silently discarded"); }
        catch (const Error& error) { QCOMPARE(error.code(), ErrorCode::ContextOverflow); }
        QVERIFY(!p->started); QCOMPARE(p->contexts.load(), 0);
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
            QFAIL("Reasoning-only output must not become an executable tool call or final answer");
        } catch (const Error& error) {
            QCOMPARE(error.code(), ErrorCode::ProtocolError);
            QVERIFY(QString::fromUtf8(error.what()).contains("only reasoning"));
        }
        QVERIFY(!delta);
        p->answer = "{}";
        try { (void)model.generate(request, {}, {}); QFAIL("Empty model output was accepted"); }
        catch (const Error& error) {
            QCOMPARE(error.code(), ErrorCode::ProtocolError);
            QVERIFY(QString::fromUtf8(error.what()).contains("empty agent turn"));
        }
    }





















};

QTEST_GUILESS_MAIN(ServiceModelTests)
#include "service_model_tests.moc"

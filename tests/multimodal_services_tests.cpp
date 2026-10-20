#include <iiMultimodalAgent.h>
#include <iiFileProviderNative.h>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>
#include <atomic>
#include <fstream>
#include <limits>

namespace a = iiMultimodalAgent::agent;
using iiLocalLLM::CancellationToken;
namespace {
class Audio final : public iiAudioGen::Backend {
public:
    std::atomic_int calls = 0;
    bool invalid = false;
    std::function<void()> during;
    iiAudioGen::GenerationOutcome generate(const iiAudioGen::GenerationRequest& request) override {
        ++calls;
        if (during) during();
        iiAudioGen::GenerationResult result;
        result.sampleRate = request.sampleRate; result.channels = request.channels;
        result.samples.assign(std::size_t(request.sampleRate * request.durationSeconds) * request.channels, 0.1F);
        if (invalid) result.samples.front() = std::numeric_limits<float>::infinity();
        return result;
    }
};
class Model final : public a::Model {
public:
    a::ModelReply generate(const a::ModelRequest& request, const CancellationToken&, const iiLocalLLM::TextCallback&) override {
        if (request.messages.back().role == a::MessageRole::Tool) {
            if (request.messages.back().content.isEmpty()) throw std::runtime_error("Generated media did not reach the next model turn");
            return {"image received"};
        }
        return {{}, {{"image", "GenerateImage", {{"prompt", "a red square"}, {"width", 8}, {"height", 8}}}}};
    }
};
a::MultimodalServices services(const QTemporaryDir& root, std::atomic_int& calls) {
    a::MultimodalServices result;
    result.outputDirectory = std::filesystem::path(root.path().toStdU16String());
    result.image = iiLocalDiffusion::NativeGenerationRequest{};
    result.image->modelPath = "host-owned-model.gguf";
    result.imageExecutor = [&calls](const auto& request, const auto& token, const auto& progress) {
        token.throwIfCancelled(); ++calls;
        if (progress) progress({iiLocalDiffusion::NativeGenerationStage::Denoising, 1, 1});
        iiLocalDiffusion::NativeGenerationResult image;
        image.width = request.width; image.height = request.height;
        image.rgb.assign(std::size_t(request.width) * request.height * 3, 127);
        return image;
    };
    return result;
}
a::ToolContext context(const QTemporaryDir& root) {
    a::ToolContext result; result.sessionId="services-test"; result.runId="run"; result.workingDirectory=root.path();
    return result;
}
}

class MultimodalServicesTests final : public QObject {
    Q_OBJECT
private slots:
    void disabledServicesDoNotAdvertiseGeneration() {
        a::MultimodalServices empty;
        QVERIFY(a::multimodalTools(empty).isEmpty());
        QVERIFY(!a::multimodalCapabilities(empty)["image_generation"].toBool());
        QVERIFY(!a::multimodalCapabilities(empty)["speech_synthesis"].toBool());
    }
    void imageIsAValidPngAndProviderFileWithInlineMedia() {
        QTemporaryDir root; std::atomic_int calls=0;
        auto tools=a::multimodalTools(services(root,calls));
        QCOMPARE(tools.size(),1);
        auto result=tools[0].execute({{"prompt","red square"},{"width",8},{"height",8}},context(root));
        QCOMPARE(calls.load(),1); QVERIFY(!result.isError);
        QCOMPARE(result.data["provider"],"iiLocalDiffusion");
        const auto path=std::filesystem::path(result.data["path"].toString().toStdU16String());
        const auto bytes=iiFileProvider::native::File::read(path,65536);
        const auto png=QByteArray(bytes.data(),qsizetype(bytes.size()));
        QVERIFY(png.startsWith(QByteArray::fromHex("89504e470d0a1a0a")));
        QCOMPARE(png.mid(12,4),QByteArray("IHDR"));
        const auto compressedLength=quint32(static_cast<unsigned char>(png[33]))*16777216U
            +quint32(static_cast<unsigned char>(png[34]))*65536U+quint32(static_cast<unsigned char>(png[35]))*256U
            +quint32(static_cast<unsigned char>(png[36]));
        const auto raw=qUncompress(QByteArray::fromHex("000000c8")+png.mid(41,compressedLength));
        QCOMPARE(raw.size(),200); QCOMPARE(raw[1],char(127));
        QCOMPARE(QByteArray::fromBase64(result.content.first().toObject()["data"].toString().toLatin1()),png);
    }
    void validationCancellationAndPermissionPreventSideEffects() {
        QTemporaryDir root; std::atomic_int calls=0;
        auto registry=std::make_shared<a::ToolRegistry>();
        for(auto& tool:a::multimodalTools(services(root,calls)))registry->add(tool);
        a::ToolRunner runner(registry,std::make_shared<a::RulePolicy>());
        auto c=context(root);
        auto denied=runner.run({"denied","GenerateImage",{{"prompt","red"},{"width",8},{"height",8}}},c);
        QVERIFY(denied.isError); QCOMPARE(calls.load(),0);
        a::ToolRunner allowed(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
        auto invalid=allowed.run({"invalid","GenerateImage",{{"prompt","red"},{"model_path","untrusted"}}},c);
        QVERIFY(invalid.isError); QCOMPARE(calls.load(),0);
        c.cancellation.cancel();
        QVERIFY_THROWS_EXCEPTION(iiLocalLLM::Error,allowed.run({"cancelled","GenerateImage",{{"prompt","red"}}},c));
        QCOMPARE(calls.load(),0);
        QVERIFY(QDir(root.path()).entryList(QDir::Files).isEmpty());
    }
    void pcmUsesTheBoundAudioBackendAndRejectsInvalidOutput() {
        QTemporaryDir root;
        auto backend=std::make_shared<Audio>(); a::MultimodalServices config;
        config.outputDirectory=std::filesystem::path(root.path().toStdU16String());
        config.audio=std::make_shared<iiAudioGen::Generator>(backend);
        auto tools=a::multimodalTools(config); QCOMPARE(tools.size(),1);
        auto result=tools[0].execute({{"prompt","a bell"},{"duration_seconds",0.02},{"sample_rate",16000}},context(root));
        QCOMPARE(result.data["provider"],"iiAudioGen"); QCOMPARE(backend->calls.load(),1);
        QCOMPARE(result.data["frames"].toInteger(),320);
        const auto bytes=QByteArray::fromBase64(result.content[0].toObject()["data"].toString().toLatin1());
        QVERIFY(bytes.startsWith("RIFF")); QCOMPARE(bytes.mid(8,4),QByteArray("WAVE"));
        backend->invalid=true;
        QVERIFY_THROWS_EXCEPTION(iiLocalLLM::Error,tools[0].execute({{"prompt","bell"},{"duration_seconds",0.02}},context(root)));
    }
    void cancellationAfterAnAudioCallDoesNotPublishPartialOutput() {
        QTemporaryDir root; auto c=context(root); auto backend=std::make_shared<Audio>();
        backend->during=[token=c.cancellation]{token.cancel();};
        a::MultimodalServices config;config.outputDirectory=std::filesystem::path(root.path().toStdU16String());
        config.audio=std::make_shared<iiAudioGen::Generator>(backend);
        auto tools=a::multimodalTools(config);
        QVERIFY_THROWS_EXCEPTION(iiLocalLLM::Error,tools[0].execute({{"prompt","bell"},{"duration_seconds",0.02}},c));
        QVERIFY(QDir(root.path()).entryList(QDir::Files).isEmpty());
    }
    void requestedPcmBudgetIsEnforcedBeforeBackendExecution() {
        QTemporaryDir root;auto backend=std::make_shared<Audio>();a::MultimodalServices config;
        config.outputDirectory=std::filesystem::path(root.path().toStdU16String());
        config.audio=std::make_shared<iiAudioGen::Generator>(backend);
        config.maximumArtifactBytes=1024;config.maximumInlineBytes=512;
        auto tools=a::multimodalTools(config);
        QVERIFY_THROWS_EXCEPTION(iiLocalLLM::Error,tools[0].execute({{"prompt","bell"},{"duration_seconds",1}},context(root)));
        QCOMPARE(backend->calls.load(),0);QVERIFY(QDir(root.path()).entryList(QDir::Files).isEmpty());
    }
    void realNativeDiffusionAdapterReturnsItsModelError() {
        QTemporaryDir root; std::atomic_int calls=0;auto config=services(root,calls);
        config.imageExecutor={};config.image->modelPath=std::filesystem::path(root.path().toStdU16String())/"missing.gguf";
        auto tools=a::multimodalTools(config);
        try {
            tools[0].execute({{"prompt","red"},{"width",8},{"height",8}},context(root));
            QFAIL("The missing native model unexpectedly generated an image");
        } catch(const iiLocalLLM::Error& error) {
            // RuntimeUnavailable would indicate a broken adapter/dependency
            // installation rather than reaching the real native model call.
            QCOMPARE(error.code(),iiLocalLLM::ErrorCode::RuntimeFailure);
        }
        QCOMPARE(calls.load(),0);QVERIFY(QDir(root.path()).entryList(QDir::Files).isEmpty());
    }
    void engineCarriesGeneratedMediaAndHostDecisionAcrossTurns() {
        QTemporaryDir root; std::atomic_int calls=0;
        auto registry=std::make_shared<a::ToolRegistry>();
        a::EngineOptions options;options.sessionsDirectory=root.filePath("sessions");
        options.multimodal=services(root,calls);
        options.skills.enabled=false;options.projectContext.enabled=false;options.compaction.automatic=false;
        options.decision.valueUnit="USD";
        options.decision.profiles={{"run",100,10,1,0.95L},{"GenerateImage",100,10,1,0.95L}};
        a::Engine engine(std::make_shared<Model>(),registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),options);
        const auto session=engine.createSession("host-text-model",root.path());
        const auto result=engine.run({session.id,"make a red image"}).result.get();
        QVERIFY2(result.status==a::RunStatus::Completed,qPrintable(result.errorMessage));
        QCOMPARE(result.text,"image received");QCOMPARE(calls.load(),1);
        auto stored=engine.session(session.id);bool found=false;
        for(const auto& message:stored.messages)if(message.role==a::MessageRole::Tool&&!message.content.isEmpty())found=true;
        QVERIFY(found);QVERIFY(!result.decision.isEmpty());
    }
    void authenticatedClientsGetSeparateProviderDirectories() {
        QTemporaryDir root;std::atomic_int calls=0;QDir().mkpath(root.filePath("work"));
        a::ApiOptions config;config.workingDirectory=root.filePath("work");config.stateDirectory=root.filePath("state");
        config.clientTokens={{"first",QString(48,'a')},{"second",QString(48,'b')}};
        config.engine.multimodal=services(root,calls);
        config.engine.skills.enabled=false;config.engine.projectContext.enabled=false;config.engine.compaction.automatic=false;
        config.engine.decision.valueUnit="USD";
        config.engine.decision.profiles={{"run",100,10,1,0.95L},{"GenerateImage",100,10,1,0.95L}};
        a::Api api(std::make_shared<Model>(),std::make_shared<a::ToolRegistry>(),std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),config);
        for(const auto& token:{QString(48,'a'),QString(48,'b')}) {
            const auto session=api.dispatch("agent.sessions.create",{{"model","host-model"}},token).result.get().toObject();
            const auto result=api.dispatch("agent.run",{{"session_id",session["session_id"]},{"prompt","make red"}},token).result.get().toObject();
            QVERIFY2(result["status"]=="completed",qPrintable(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))));
            const auto info=api.dispatch("agent.info",{},token).result.get().toObject();
            QVERIFY(info["multimodal"].toObject()["image_generation"].toBool());
        }
        QCOMPARE(calls.load(),2);
        const auto clients=QDir(root.filePath("state")).entryList(QDir::Dirs|QDir::NoDotAndDotDot);
        QCOMPARE(clients.size(),2);
        for(const auto& client:clients)
            QCOMPARE(QDir(root.filePath("state/")+client+"/generated").entryList({"*.png"},QDir::Files).size(),1);
        QVERIFY(QDir(root.path()).entryList({"*.png"},QDir::Files).isEmpty());
    }
};
QTEST_GUILESS_MAIN(MultimodalServicesTests)
#include "multimodal_services_tests.moc"

#include <iiMultimodalAgent.h>
#include <iiFileProviderNative.h>
#include <iiFileProviderPresentation.h>
#include <QtCore/QDir>
#include <QtCore/QFile>
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
a::ToolContext context(const std::filesystem::path& root) {
    const auto utf8 = root.u8string();
    a::ToolContext result; result.sessionId="services-test"; result.runId="run";
    result.workingDirectory=QString::fromUtf8(reinterpret_cast<const char*>(utf8.data()), qsizetype(utf8.size()));
    return result;
}
std::filesystem::path path(const QString& value) { return std::filesystem::path(value.toStdU16String()); }
struct PresentationCleanup {
    std::filesystem::path root;
    ~PresentationCleanup() { try { iiFileProvider::native::Presentation::cancelPending(root); } catch (...) {} }
};
void write(const QString& name, const QByteArray& bytes) {
    iiFileProvider::native::File::create(path(name), std::string_view(bytes.constData(), std::size_t(bytes.size())));
}
QByteArray json(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
void little(QByteArray& bytes, quint64 value, int count) {
    for (int i=0;i<count;++i) bytes.append(char((value >> (8*i)) & 255));
}
QByteArray safetensors(const QJsonObject& metadata, const QJsonObject& assets = {}) {
    QJsonObject header{{"__metadata__", metadata}};
    QByteArray payload;
    for (auto it=assets.begin();it!=assets.end();++it) {
        const auto bytes=json(it.value().toObject());
        const auto begin=payload.size(); payload+=bytes;
        header.insert(it.key(),QJsonObject{{"dtype","U8"},{"shape",QJsonArray{bytes.size()}},
            {"data_offsets",QJsonArray{begin,payload.size()}}});
    }
    const auto body=json(header); QByteArray result; little(result,body.size(),8); result+=body; result+=payload; return result;
}
QByteArray gguf(const QList<QPair<QByteArray,QByteArray>>& fields) {
    QByteArray result("GGUF");little(result,3,4);little(result,1,8);little(result,fields.size(),8);
    for(const auto& [key,value]:fields) {
        little(result,key.size(),8);result+=key;little(result,8,4);little(result,value.size(),8);result+=value;
    }
    return result;
}
QByteArray storedZip(const QList<QPair<QByteArray,QByteArray>>& entries, bool zip64=false) {
    QByteArray result, directory;
    for (const auto& entry:entries) {
        const auto offset=result.size(); const auto& name=entry.first; const auto& data=entry.second;
        little(result,0x04034b50,4); little(result,20,2); little(result,0,2); little(result,0,2);
        little(result,0,4); little(result,0,4); little(result,data.size(),4); little(result,data.size(),4);
        little(result,name.size(),2); little(result,0,2); result+=name; result+=data;
        little(directory,0x02014b50,4); little(directory,zip64?45:20,2); little(directory,zip64?45:20,2); little(directory,0,2);
        little(directory,0,2); little(directory,0,4); little(directory,0,4);
        little(directory,zip64?0xffffffff:data.size(),4); little(directory,zip64?0xffffffff:data.size(),4); little(directory,name.size(),2);
        little(directory,zip64?28:0,2); little(directory,0,2); little(directory,0,2); little(directory,0,2);
        little(directory,0,4); little(directory,zip64?0xffffffff:offset,4); directory+=name;
        if(zip64) {little(directory,1,2);little(directory,24,2);little(directory,data.size(),8);
            little(directory,data.size(),8);little(directory,offset,8);}
    }
    const auto offset=result.size();result+=directory;
    if(zip64) {
        const auto zip64Offset=result.size();little(result,0x06064b50,4);little(result,44,8);
        little(result,45,2);little(result,45,2);little(result,0,4);little(result,0,4);
        little(result,entries.size(),8);little(result,entries.size(),8);little(result,directory.size(),8);little(result,offset,8);
        little(result,0x07064b50,4);little(result,0,4);little(result,zip64Offset,8);little(result,1,4);
    }
    little(result,0x06054b50,4);little(result,0,2);little(result,0,2);
    little(result,zip64?0xffff:entries.size(),2);little(result,zip64?0xffff:entries.size(),2);
    little(result,zip64?0xffffffff:directory.size(),4);little(result,zip64?0xffffffff:offset,4);
    little(result,0,2);return result;
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
    void imageModelSelectionUsesPackageMetadataAndExplicitOptions() {
        QTemporaryDir root(QStringLiteral(IIMULTIMODAL_TEST_RESOURCES "/image-profile-XXXXXX"));
        PresentationCleanup cleanup{path(root.path())};
        QVERIFY(root.isValid());std::atomic_int calls=0;
        QVERIFY(QDir().mkpath(root.filePath("model-a/transformer")));
        QVERIFY(QDir().mkpath(root.filePath("MOSS-Image-not-a-model")));
        write(root.filePath("model-a/model_index.json"),json({{"_class_name","MOSSImagePipeline"}}));
        write(root.filePath("MOSS-Image-not-a-model/model_index.json"),json({{"_class_name","OtherPipeline"}}));
        std::error_code linkError;
        std::filesystem::create_symlink(path(root.filePath("model-a/model_index.json")),
            path(root.filePath("MOSS-Image-not-a-model/config.json")),linkError);
        auto config=services(root,calls);config.image->steps=27;
        config.imageModels={{"moss",path(root.filePath("model-a"))},
            {"renamed",path(root.filePath("MOSS-Image-not-a-model"))}};
        iiLocalDiffusion::NativeGenerationRequest observed;
        config.imageExecutor=[&](const auto& request,const auto&,const auto&) {
            ++calls;observed=request;iiLocalDiffusion::NativeGenerationResult result;
            result.width=request.width;result.height=request.height;result.rgb.assign(std::size_t(request.width)*request.height*3,1);return result;
        };
        auto tool=a::multimodalTools(config)[0];const auto c=context(root);
        const auto choices=tool.definition.inputSchema["properties"].toObject()["model"].toObject()["enum"].toArray();
        QVERIFY(choices.contains("default"));QVERIFY(choices.contains("moss"));QVERIFY(choices.contains("renamed"));
        auto result=tool.execute({{"prompt","x"},{"model","moss"},{"width",8},{"height",8}},c);
        QCOMPARE(observed.modelPath,path(root.filePath("model-a")));QCOMPARE(observed.steps,8);
        QCOMPARE(result.data["model_family"],"moss-image");QCOMPARE(result.data["steps"],8);
        QCOMPARE(result.data["steps_source"],"model_family");
        const auto explicitResult=tool.execute({{"prompt","x"},{"model","moss"},{"width",8},{"height",8},{"steps",50}},c);
        QCOMPARE(observed.steps,50);QCOMPARE(explicitResult.data["steps_source"],"request");
        tool.execute({{"prompt","x"},{"model","renamed"},{"width",8},{"height",8}},c);
        QCOMPARE(observed.steps,27);
        tool.execute({{"prompt","x"},{"width",8},{"height",8}},c);
        QCOMPARE(observed.modelPath,config.image->modelPath);QCOMPARE(observed.steps,27);
        auto defaultMoss=config;defaultMoss.image->modelPath=path(root.filePath("model-a"));
        a::multimodalTools(defaultMoss)[0].execute({{"prompt","x"},{"width",8},{"height",8}},c);
        QCOMPARE(observed.steps,8);
        auto registry=std::make_shared<a::ToolRegistry>();registry->add(tool);
        a::ToolRunner runner(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass));
        const auto admitted=runner.run({"alias","GenerateImage",{{"prompt","x"},{"model","moss"},{"width",8},{"height",8}}},c);
        QVERIFY(!admitted.isError);QCOMPARE(observed.steps,8);
        const auto rejected=runner.run({"unknown","GenerateImage",{{"prompt","x"},{"model","missing"}}},c);
        QVERIFY(rejected.isError);
        const auto before=calls.load();
        QVERIFY_THROWS_EXCEPTION(iiLocalLLM::Error,tool.execute({{"prompt","x"},{"model","../model-a"}},c));
        QVERIFY_THROWS_EXCEPTION(iiLocalLLM::Error,tool.execute({{"prompt","x"},{"model",root.filePath("model-a")}},c));
        QVERIFY_THROWS_EXCEPTION(iiLocalLLM::Error,tool.execute({{"prompt","x"},{"model",true}},c));
        QCOMPARE(calls.load(),before);
    }
    void embeddedDefaultsAreBoundedAndOverrideFamilyFallback() {
        QTemporaryDir root(QStringLiteral(IIMULTIMODAL_TEST_RESOURCES "/image-profile-XXXXXX"));
        PresentationCleanup cleanup{path(root.path())};
        QVERIFY(root.isValid());std::atomic_int calls=0;
        const auto model=root.filePath("opaque.safetensors");
        write(model,safetensors({},{{"__iild_assets__/other/model_index.json",QJsonObject{{"_class_name","MOSSImagePipeline"}}},
            {"__iild_assets__/other/generation_config.json",QJsonObject{{"num_inference_steps",12},{"width",16},{"height",24}}}}));
        auto config=services(root,calls);config.image->modelPath=path(model);config.maximumImageDimension=64;
        iiLocalDiffusion::NativeGenerationRequest observed;
        config.imageExecutor=[&](const auto& request,const auto&,const auto&) {++calls;observed=request;
            iiLocalDiffusion::NativeGenerationResult result;result.width=request.width;result.height=request.height;
            result.rgb.assign(std::size_t(request.width)*request.height*3,1);return result;};
        config.image->width=8;config.image->height=8;
        auto tool=a::multimodalTools(config)[0];
        auto result=tool.execute({{"prompt","x"}},context(root));
        QCOMPARE(observed.steps,12);QCOMPARE(observed.width,16);QCOMPARE(observed.height,24);
        QCOMPARE(result.data["steps_source"],"package");
        tool.execute({{"prompt","x"},{"steps",33},{"width",8}},context(root));
        QCOMPARE(observed.steps,33);QCOMPARE(observed.width,8);QCOMPARE(observed.height,24);
    }
    void legacyZipReadsFirstStageMetadataWithoutExtractingWeights() {
        QTemporaryDir root(QStringLiteral(IIMULTIMODAL_TEST_RESOURCES "/image-profile-XXXXXX"));
        PresentationCleanup cleanup{path(root.path())};
        QVERIFY(root.isValid());std::atomic_int calls=0;
        const auto model=root.filePath("opaque.iildmodel");
        const auto zip64=root.filePath("opaque64.iildmodel"),duplicate=root.filePath("duplicate.iildmodel");
        const auto manifest=json({{"schema","iild-unified-model-v1"},{"stages",QJsonArray{QJsonObject{{"model","members/000/model.safetensors"}}}}});
        const QList<QPair<QByteArray,QByteArray>> entries{{"model_index.json",manifest},
            {"members/000/model.safetensors",safetensors({{"modelspec.architecture","moss-image"}})}};
        write(model,storedZip(entries));write(zip64,storedZip(entries,true));
        write(duplicate,storedZip({entries[0],entries[0],entries[1]}));
        auto config=services(root,calls);config.image->modelPath=path(model);config.image->width=8;config.image->height=8;
        config.imageModels={{"zip64",path(zip64)},{"duplicate",path(duplicate)}};
        int steps=0;config.imageExecutor=[&](const auto& request,const auto&,const auto&) {++calls;steps=request.steps;
            iiLocalDiffusion::NativeGenerationResult result;result.width=request.width;result.height=request.height;
            result.rgb.assign(std::size_t(request.width)*request.height*3,1);return result;};
        const auto tool=a::multimodalTools(config)[0];
        const auto result=tool.execute({{"prompt","x"}},context(root));
        QCOMPARE(steps,8);QCOMPARE(result.data["model_family"],"moss-image");
        tool.execute({{"prompt","x"},{"model","zip64"}},context(root));QCOMPARE(steps,8);
        tool.execute({{"prompt","x"},{"model","duplicate"}},context(root));QCOMPARE(steps,20);
        QCOMPARE(QDir(root.path()).entryList(QDir::Dirs|QDir::NoDotAndDotDot).size(),0);
    }
    void ggufMetadataIsInspectedWithoutUsingNamesOrInvalidHints() {
        QTemporaryDir root(QStringLiteral(IIMULTIMODAL_TEST_RESOURCES "/image-profile-XXXXXX"));
        PresentationCleanup cleanup{path(root.path())};
        QVERIFY(root.isValid());std::atomic_int calls=0;
        const auto moss=root.filePath("opaque.gguf"),other=root.filePath("MOSS-Image.gguf"),broken=root.filePath("broken.gguf"),krea=root.filePath("other.gguf");
        write(moss,gguf({{"general.architecture","moss_image"},
            {"iild.generation_parameters","{\"steps\":999,\"width\":1024}"}}));
        write(other,gguf({{"general.name","MOSS-Image-Base"},{"general.architecture","other"}}));
        write(other+".model.json",json({{"iild.model_family","moss-image"}}));
        write(krea,gguf({{"general.architecture","krea2"}}));
        auto truncated=gguf({{"general.architecture","moss_image"},{"another","x"}});truncated.chop(2);write(broken,truncated);
        auto config=services(root,calls);config.image->width=8;config.image->height=8;config.image->steps=23;
        config.maximumImageDimension=64;config.image->modelPath=path(moss);
        config.imageModels={{"other",path(other)},{"broken",path(broken)},{"krea",path(krea)}};
        iiLocalDiffusion::NativeGenerationRequest observed;
        config.imageExecutor=[&](const auto& request,const auto&,const auto&) {++calls;observed=request;
            iiLocalDiffusion::NativeGenerationResult result;result.width=request.width;result.height=request.height;
            result.rgb.assign(std::size_t(request.width)*request.height*3,1);return result;};
        auto tool=a::multimodalTools(config)[0];
        tool.execute({{"prompt","x"}},context(root));QCOMPARE(observed.steps,8);QCOMPARE(observed.width,8);
        tool.execute({{"prompt","x"},{"model","other"}},context(root));QCOMPARE(observed.steps,23);
        tool.execute({{"prompt","x"},{"model","broken"}},context(root));QCOMPARE(observed.steps,23);
        tool.execute({{"prompt","x"},{"model","krea"}},context(root));QCOMPARE(observed.steps,52);
    }
    void pcmUsesTheBoundAudioBackendAndRejectsInvalidOutput() {
        iiFileProvider::native::TemporaryDirectory root(IIMULTIMODAL_TEST_RESOURCES);
        QVERIFY(root.isValid());
        auto backend=std::make_shared<Audio>(); a::MultimodalServices config;
        config.outputDirectory=root.path();
        config.audio=std::make_shared<iiAudioGen::Generator>(backend);
        auto tools=a::multimodalTools(config); QCOMPARE(tools.size(),1);
        auto result=tools[0].execute({{"prompt","a bell"},{"duration_seconds",0.02},{"sample_rate",16000}},context(root.path()));
        QCOMPARE(result.data["provider"],"iiAudioGen"); QCOMPARE(backend->calls.load(),1);
        QCOMPARE(result.data["frames"].toInteger(),320);
        const auto bytes=QByteArray::fromBase64(result.content[0].toObject()["data"].toString().toLatin1());
        QVERIFY(bytes.startsWith("RIFF")); QCOMPARE(bytes.mid(8,4),QByteArray("WAVE"));
        const auto path=std::filesystem::path(result.data["path"].toString().toStdU16String());
        const auto published=iiFileProvider::native::File::read(path);
        QCOMPARE(QByteArray(published.data(),qsizetype(published.size())),bytes);
        backend->invalid=true;
        QVERIFY_THROWS_EXCEPTION(iiLocalLLM::Error,tools[0].execute({{"prompt","bell"},{"duration_seconds",0.02}},context(root.path())));
        iiFileProvider::native::Presentation::cancelPending(path);
        QVERIFY(iiFileProvider::native::File::remove(path));
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

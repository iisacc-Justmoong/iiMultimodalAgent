#include <iiMultimodalAgent/agent/MultimodalTools.h>
#include <iiMultimodalAgent/detail/ImageLoader.h>
#include <iiFileProviderNative.h>
#include <QtCore/QByteArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <string_view>
#include <thread>

namespace iiMultimodalAgent::agent {
namespace {
void require(bool condition, const char* message, ErrorCode code = ErrorCode::InvalidArgument) {
    if (!condition) throw Error(code, QString::fromUtf8(message));
}
QString pathText(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(utf8.data()), qsizetype(utf8.size()));
}
QString prompt(const QJsonObject& args, const char* key) {
    const auto value = args.value(QLatin1String(key));
    require(value.isString(), "A text prompt is required");
    const auto text = value.toString();
    require(!text.trimmed().isEmpty() && !text.contains(QChar(0)) && text.toUtf8().size() <= 4096,
        "Prompt must contain 1..4096 UTF-8 bytes without NUL");
    return text;
}
int integer(const QJsonObject& args, const char* key, int fallback, int minimum, int maximum) {
    const auto value = args.value(QLatin1String(key));
    if (value.isUndefined()) return fallback;
    require(value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() == value.toInt()
        && value.toInt() >= minimum && value.toInt() <= maximum, "Invalid integer generation option");
    return value.toInt();
}
std::int64_t imageSeed(const QJsonObject& args, std::int64_t fallback) {
    const auto value = args.value("seed");
    if (value.isUndefined()) return fallback;
    const auto number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    const auto parsed = value.toInteger();
    require(value.isDouble() && std::isfinite(number) && std::trunc(number) == number
        && value == QJsonValue(parsed), "Image seed must be a signed 64-bit integer");
    return parsed;
}
void fields(const QJsonObject& args, const QStringList& allowed) {
    for (auto it = args.begin(); it != args.end(); ++it)
        require(allowed.contains(it.key()), "Unknown generation option");
}
QJsonObject numberSchema(double minimum, double maximum, bool integral = true) {
    return {{"type", integral ? "integer" : "number"}, {"minimum", minimum}, {"maximum", maximum}};
}
QJsonObject schema(QJsonObject properties, const char* required) {
    return {{"type", "object"}, {"properties", properties}, {"required", QJsonArray{required}}, {"additionalProperties", false}};
}
void append32(QByteArray& bytes, quint32 value) {
    for (int shift : {24, 16, 8, 0}) bytes.append(char((value >> shift) & 255));
}
void chunk(QByteArray& png, const char* kind, const QByteArray& payload) {
    append32(png, quint32(payload.size()));
    const QByteArray input = QByteArray(kind, 4) + payload;
    png += input;
    quint32 crc = 0xffffffffU;
    for (const auto byte : input) {
        crc ^= static_cast<unsigned char>(byte);
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    append32(png, crc ^ 0xffffffffU);
}
QByteArray pngBytes(const iiLocalDiffusion::NativeGenerationResult& image, const QJsonObject& settings) {
    QByteArray header;
    append32(header, quint32(image.width)); append32(header, quint32(image.height));
    header += QByteArray::fromHex("0802000000"); // 8-bit RGB, PNG filter/compression/interlace 0.
    QByteArray scanlines;
    scanlines.reserve(qsizetype(image.rgb.size()) + image.height);
    for (int row = 0; row < image.height; ++row) {
        scanlines.append(char(0));
        scanlines.append(reinterpret_cast<const char*>(image.rgb.data()) + std::size_t(row) * image.width * 3,
            qsizetype(image.width) * 3);
    }
    QByteArray png = QByteArray::fromHex("89504e470d0a1a0a");
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", qCompress(scanlines, 6).mid(4)); // qCompress's first 4 bytes are its length, not zlib.
    QByteArray metadata("iiSDK.Generation");
    metadata.append(5, char(0)); // keyword terminator, uncompressed, method, language, translated keyword
    metadata += QJsonDocument(settings).toJson(QJsonDocument::Compact);
    chunk(png, "iTXt", metadata);
    chunk(png, "IEND", {});
    return png;
}
struct State {
    MultimodalServices services;
    ReportingImageExecutor reportingImageExecutor;
    std::timed_mutex mutex;
};
class Lease {
public:
    Lease(State& state, const CancellationToken& token) : lock(state.mutex, std::defer_lock) {
        while (!lock.try_lock_for(std::chrono::milliseconds(10))) token.throwIfCancelled();
        token.throwIfCancelled();
    }
private:
    std::unique_lock<std::timed_mutex> lock;
};
std::filesystem::path destination(const State& state, const char* extension) {
    return state.services.outputDirectory / (QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString() + extension);
}
ToolResult artifact(const State& state, const std::filesystem::path& path, const QByteArray& bytes,
    const char* mime, const char* modality, const char* provider, QJsonObject details = {}) {
    details.insert("path", pathText(path)); details.insert("mime_type", mime);
    details.insert("bytes", qint64(bytes.size())); details.insert("provider", provider);
    details.insert("modality", modality);
    const bool inlineMedia = state.services.inlineMedia && std::size_t(bytes.size()) <= state.services.maximumInlineBytes;
    details.insert("inline_media", inlineMedia);
    ToolResult result{QStringLiteral("Generated ") + QLatin1String(modality) + ": " + pathText(path), details};
    if (inlineMedia)
        result.content.append(QJsonObject{{"type", modality}, {"mimeType", mime}, {"data", QString::fromLatin1(bytes.toBase64())}});
    return result;
}
ToolResult waveArtifact(State& state, const iiVoiceOver::SpeechAudio& audio, const ToolContext& context, const char* provider) {
    require(audio.sample_rate >= 8000 && audio.sample_rate <= 384000 && audio.channels >= 1 && audio.channels <= 8,
        "Backend returned an invalid PCM format", ErrorCode::ProtocolError);
    require(!audio.samples.empty() && audio.samples.size() % audio.channels == 0
        && audio.samples.size() <= (state.services.maximumArtifactBytes - 44) / 2,
        "PCM output exceeds the artifact budget", ErrorCode::ResourceLimit);
    require(double(audio.samples.size()) / audio.channels / audio.sample_rate <= state.services.maximumAudioSeconds + 1.0 / audio.sample_rate,
        "PCM output exceeds the duration budget", ErrorCode::ResourceLimit);
    for (const auto sample : audio.samples)
        require(std::isfinite(sample), "Backend returned non-finite PCM", ErrorCode::ProtocolError);
    context.cancellation.throwIfCancelled();
    const auto path = destination(state, ".wav");
    const auto utf8Path = path.u8string();
    const std::string_view pathView(reinterpret_cast<const char*>(utf8Path.data()), utf8Path.size());
    require(!pathView.empty() && pathView.find('\0') == std::string_view::npos && iiVoiceOver::isValidUtf8(pathView),
        "Invalid WAV destination", ErrorCode::RuntimeFailure);
    const auto wave = audio.waveBytes();
    require(bool(wave), wave ? "" : wave.error().message.c_str(), ErrorCode::RuntimeFailure);
    try { iiFileProvider::native::File::create(path, *wave); }
    catch (const std::exception& error) { require(false, error.what(), ErrorCode::RuntimeFailure); }
    const auto bytes = iiFileProvider::native::File::read(path, state.services.maximumArtifactBytes);
    return artifact(state, path, QByteArray(bytes.data(), qsizetype(bytes.size())), "audio/wav", "audio", provider,
        {{"sample_rate", qint64(audio.sample_rate)}, {"channels", audio.channels}, {"frames", qint64(audio.samples.size() / audio.channels)}});
}
iiLocalDiffusion::NativeImageExecution nativeImage(const iiLocalDiffusion::NativeGenerationRequest& request,
    const CancellationToken& token, const iiLocalDiffusion::NativeProgressCallback& progress, int maximumImageDimension) {
    std::atomic_bool cancelled{token.isCancelled()};
    std::jthread bridge([token, &cancelled](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (token.isCancelled()) { cancelled.store(true); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    iiLocalDiffusion::NativeImageExecution result;
    detail::imageEntry()(&request, &cancelled, &progress, maximumImageDimension, &result);
    return result;
}
QJsonObject stageSettings(const iiLocalDiffusion::NativeImageStageSettings& stage) {
    QJsonArray loras;
    for (const auto& lora : stage.loras)
        loras.append(QJsonObject{{"name",pathText(lora.path.filename())},{"strength",double(lora.strength)}});
    QJsonArray sigmas;
    for (const auto sigma : stage.sigmas) sigmas.append(double(sigma));
    QJsonArray hiresSigmas;
    for (const auto sigma : stage.hiresSigmas) hiresSigmas.append(double(sigma));
    QJsonObject result{{"model",pathText(stage.modelPath.filename())},
        {"effective_model",pathText(stage.effectiveModelPath.filename())},{"q8_cache_used",stage.q8CacheUsed},
        {"model_family",QString::fromStdString(stage.modelFamily)},
        {"vae",pathText(stage.vaePath.filename())},{"prompt",QString::fromStdString(stage.prompt)},
        {"negative_prompt",QString::fromStdString(stage.negativePrompt)},
        {"base_width",stage.baseWidth},{"base_height",stage.baseHeight},
        {"canvas_width",stage.canvasWidth},{"canvas_height",stage.canvasHeight},
        {"steps",stage.steps},{"seed",qint64(stage.seed)},
        {"sampler",QString::fromStdString(stage.samplerName)}, {"scheduler",QString::fromStdString(stage.schedulerName)},
        {"cfg_scale",double(stage.cfgScale)},{"image_cfg_scale",double(stage.imageCfgScale)},
        {"distilled_guidance",double(stage.distilledGuidance)},{"hires",stage.hires},
        {"hires_upscaler",QString::fromStdString(stage.hiresUpscalerName)},
        {"hires_steps",stage.hiresResolved ? QJsonValue(stage.hiresSteps) : QJsonValue(QJsonValue::Null)},
        {"hires_strength",double(stage.hiresStrength)}, {"loras",loras},
        {"image_to_image",stage.imageToImage},{"sigmas",sigmas}};
    if (stage.hiresResolved) {
        result.insert("hires_scheduler_steps",stage.hiresSchedulerSteps);
        result.insert("hires_sigmas",hiresSigmas);
    }
    if (stage.imageToImage) result.insert("image_strength",double(stage.imageStrength));
    if (std::isfinite(stage.flowShift)) result.insert("flow_shift",double(stage.flowShift));
    return result;
}
Tool tool(const char* name, const char* description, const char* provider, const char* modality, QJsonObject input) {
    Tool result;
    result.definition = {name, description, std::move(input)};
    result.definition.editsFiles = true;
    result.definition.metadata = {{"provider", provider}, {"output_modality", modality}};
    // Default permission rules ask for these tools; host Allow rules still apply.
    return result;
}
}

QJsonObject multimodalCapabilities(const MultimodalServices& services) {
    return {{"text", true}, {"image_input", true}, {"audio_input", true}, {"video_frames_input", true},
        {"image_generation", services.image.has_value()}, {"audio_generation", bool(services.audio)},
        {"speech_synthesis", bool(services.speech)}};
}

QList<Tool> multimodalTools(MultimodalServices services) {
    return multimodalTools(std::move(services), {});
}

QList<Tool> multimodalTools(MultimodalServices services, ReportingImageExecutor imageExecutor) {
    require(services.maximumImageDimension >= 8 && services.maximumImageDimension <= 4096
        && std::isfinite(services.maximumAudioSeconds) && services.maximumAudioSeconds > 0 && services.maximumAudioSeconds <= 300
        && services.maximumArtifactBytes >= 1024 && services.maximumArtifactBytes <= 128 * 1024 * 1024
        && services.maximumInlineBytes <= services.maximumArtifactBytes,
        "Invalid multimodal generation budgets");
    require(services.image || services.imageModels.empty(), "Image model IDs require an image provider");
    require(!imageExecutor || (services.image && !services.imageExecutor), "A reporting image executor requires a single image provider");
    require(services.imageModels.size() <= 128, "Too many image model IDs");
    for (const auto& [id,path] : services.imageModels)
        require(validImageModelId(id) && !path.empty(), "Invalid host image model ID or path");
    if (!services.image && !services.audio && !services.speech) return {};
    require(!services.outputDirectory.empty() && std::filesystem::is_directory(services.outputDirectory),
        "An existing host-owned generation directory is required");
    services.outputDirectory = std::filesystem::canonical(services.outputDirectory);
    if (services.image) {
        require(!services.image->modelPath.empty(), "A host-owned diffusion model path is required");
        const auto validDimension = [&](int value) {
            return value == 0 || (value >= 8 && value <= services.maximumImageDimension && value % 8 == 0);
        };
        require(validDimension(services.image->width) && validDimension(services.image->height),
            "Default image dimensions exceed the host budget");
        require(services.image->steps >= 0 && services.image->steps <= 1000 && services.image->timeoutMilliseconds > 0,
            "Invalid host diffusion sampling options");
    }
    auto state = std::make_shared<State>(); state->services = std::move(services); state->reportingImageExecutor = std::move(imageExecutor);
    QList<Tool> tools;
    if (state->services.image) {
        QJsonArray modelIds{"default"};
        for (const auto& [id,path] : state->services.imageModels) {
            (void)path;modelIds.append(QString::fromStdString(id));
        }
        auto image = tool("GenerateImage", "Generate an image using the host's local diffusion model", "iiLocalDiffusion", "image",
            schema({{"prompt", QJsonObject{{"type", "string"}}},
                {"model", QJsonObject{{"type", "string"}, {"enum", modelIds},
                    {"description", "Host-configured image model ID; default uses the host's default model"}}},
                {"width", numberSchema(8, state->services.maximumImageDimension)},
                {"height", numberSchema(8, state->services.maximumImageDimension)}, {"steps", numberSchema(1, 1000)},
                {"seed", QJsonObject{{"type", "integer"}}}}, "prompt"));
        image.definition.metadata.insert("iimagent.preserve_arguments", true);
        image.execute = [state](const QJsonObject& args, const ToolContext& context) {
            Lease lease(*state, context.cancellation);
            fields(args, {"prompt", "model", "width", "height", "steps", "seed"});
            auto request = *state->services.image;
            request.prompt = prompt(args, "prompt").toStdString();
            QString modelId="default";
            if (args.contains("model")) {
                require(args["model"].isString(), "Image model must be a registered model ID");
                modelId=args["model"].toString();
                if (modelId!="default") {
                    const auto found=state->services.imageModels.find(modelId.toUtf8().toStdString());
                    require(found!=state->services.imageModels.end(), "Image model ID is not registered by the host");
                    request.modelPath=found->second;
                }
            }
            const auto source = [&](const char* key, int value) -> QString {
                return args.contains(QLatin1String(key)) ? "request" : value == 0 ? "backend_default" : "host";
            };
            const QString widthSource=source("width",request.width), heightSource=source("height",request.height),
                stepsSource=source("steps",request.steps);
            request.width = integer(args, "width", request.width, 8, state->services.maximumImageDimension);
            request.height = integer(args, "height", request.height, 8, state->services.maximumImageDimension);
            request.steps = integer(args, "steps", request.steps, 1, 1000);
            request.seed = imageSeed(args, request.seed);
            require(request.width % 8 == 0 && request.height % 8 == 0, "Image dimensions must use an 8-pixel grid");
            context.cancellation.throwIfCancelled();
            const auto progress = [&](const iiLocalDiffusion::NativeGenerationProgress& p) {
                if (context.progress) context.progress({{"provider", "iiLocalDiffusion"}, {"step", p.step}, {"total", p.total}});
            };
            iiLocalDiffusion::NativeImageExecution execution;
            const bool reported = !state->services.imageExecutor;
            if (state->services.imageExecutor)
                execution.image = state->services.imageExecutor(request, context.cancellation, progress);
            else if (state->reportingImageExecutor)
                execution = state->reportingImageExecutor(request, context.cancellation, progress);
            else
                execution = nativeImage(request, context.cancellation, progress, state->services.maximumImageDimension);
            const auto& result = execution.image;
            context.cancellation.throwIfCancelled();
            require(!result.cancelled, "Image generation cancelled", ErrorCode::Cancelled);
            require(result.error.empty(), result.error.c_str(), ErrorCode::RuntimeFailure);
            require(result.width >= 8 && result.height >= 8
                && result.width <= state->services.maximumImageDimension && result.height <= state->services.maximumImageDimension
                && (request.width == 0 || result.width == request.width) && (request.height == 0 || result.height == request.height)
                && result.rgb.size() == std::size_t(result.width) * result.height * 3,
                "Backend returned an invalid RGB image", ErrorCode::ProtocolError);
            QJsonObject requested{{"model",modelId},{"prompt",QString::fromStdString(request.prompt)},
                {"width",request.width ? QJsonValue(request.width) : QJsonValue(QJsonValue::Null)},
                {"height",request.height ? QJsonValue(request.height) : QJsonValue(QJsonValue::Null)},
                {"steps",request.steps ? QJsonValue(request.steps) : QJsonValue(QJsonValue::Null)},
                {"seed",qint64(request.seed)},{"width_source",widthSource},{"height_source",heightSource},
                {"steps_source",stepsSource},{"seed_source",args.contains("seed") ? "request" : "host"}};
            QJsonObject settings{{"report_status",reported ? "backend_reported" : "unreported"},
                {"requested",requested},{"output",QJsonObject{{"width",result.width},{"height",result.height}}}};
            if (reported) {
                require(!execution.report.stages.empty(), "Image backend omitted applied generation settings", ErrorCode::ProtocolError);
                const auto& resolved = execution.report.resolvedRequest;
                require(resolved.width == result.width && resolved.height == result.height
                    && (request.steps == 0 || resolved.steps == request.steps)
                    && resolved.seed == request.seed && resolved.prompt == request.prompt
                    && resolved.modelPath == request.modelPath, "Image backend changed an explicit request", ErrorCode::ProtocolError);
                QJsonArray stages;
                for (const auto& stage : execution.report.stages) {
                    require(stage.resolved, "Image backend returned unresolved generation settings", ErrorCode::ProtocolError);
                    stages.append(stageSettings(stage));
                }
                settings.insert("resolved_request",QJsonObject{{"width",resolved.width},{"height",resolved.height},
                    {"steps",resolved.steps},{"seed",qint64(resolved.seed)}});
                settings.insert("stages",stages);
                settings.insert("reported_by",state->reportingImageExecutor ? "host_executor" : "iiLocalDiffusion");
            }
            const auto bytes = pngBytes(result, settings);
            require(std::size_t(bytes.size()) <= state->services.maximumArtifactBytes, "PNG exceeds the artifact budget", ErrorCode::ResourceLimit);
            const auto path = destination(*state, ".png");
            iiFileProvider::native::File::create(path, std::string_view(bytes.constData(), std::size_t(bytes.size())));
            const auto effectiveSteps = reported ? execution.report.resolvedRequest.steps : request.steps;
            QJsonObject details{{"width", result.width}, {"height", result.height}, {"model",modelId},
                {"steps",effectiveSteps ? QJsonValue(effectiveSteps) : QJsonValue(QJsonValue::Null)},
                {"steps_source",stepsSource}, {"generation_settings",settings}};
            return artifact(*state, path, bytes, "image/png", "image", "iiLocalDiffusion", details);
        };
        tools.append(std::move(image));
    }
    if (state->services.audio) {
        auto audio = tool("GenerateAudio", "Generate sound using the host's loaded audio model", "iiAudioGen", "audio",
            schema({{"prompt", QJsonObject{{"type", "string"}}}, {"duration_seconds", numberSchema(0.01, state->services.maximumAudioSeconds, false)},
                {"sample_rate", numberSchema(8000, 192000)}, {"channels", numberSchema(1, 8)}, {"seed", numberSchema(0, 2147483647)}}, "prompt"));
        audio.execute = [state](const QJsonObject& args, const ToolContext& context) {
            Lease lease(*state, context.cancellation);
            fields(args, {"prompt", "duration_seconds", "sample_rate", "channels", "seed"});
            iiAudioGen::GenerationRequest request;
            request.prompt = prompt(args, "prompt").toStdString();
            if (args.contains("duration_seconds")) {
                require(args["duration_seconds"].isDouble(), "Audio duration must be numeric");
                request.durationSeconds = args["duration_seconds"].toDouble();
            } else request.durationSeconds = std::min(5.0, state->services.maximumAudioSeconds);
            require(std::isfinite(request.durationSeconds) && request.durationSeconds > 0 && request.durationSeconds <= state->services.maximumAudioSeconds,
                "Audio duration exceeds the host budget");
            request.sampleRate = std::uint32_t(integer(args, "sample_rate", 44100, 8000, 192000));
            request.channels = std::uint16_t(integer(args, "channels", 1, 1, 8));
            require(std::ceil(request.durationSeconds * request.sampleRate) * request.channels
                <= double((state->services.maximumArtifactBytes - 44) / 2),
                "Requested PCM exceeds the artifact budget", ErrorCode::ResourceLimit);
            if (args.contains("seed")) request.seed = std::uint64_t(integer(args, "seed", 0, 0, 2147483647));
            const auto result = state->services.audio->generate(request);
            context.cancellation.throwIfCancelled();
            require(bool(result), result ? "" : result.error().message.c_str(), ErrorCode::RuntimeFailure);
            require(result->sampleRate == request.sampleRate && result->channels == request.channels,
                "Audio backend changed the requested PCM format", ErrorCode::ProtocolError);
            require(double(result->samples.size()) / result->channels / result->sampleRate <= request.durationSeconds + 1.0 / result->sampleRate,
                "Audio backend exceeded the requested duration", ErrorCode::ProtocolError);
            require(double(result->samples.size()) / result->channels / result->sampleRate <= state->services.maximumAudioSeconds + 1.0 / result->sampleRate,
                "Audio backend exceeded the duration budget", ErrorCode::ResourceLimit);
            return waveArtifact(*state, {result->samples, result->sampleRate, result->channels}, context, "iiAudioGen");
        };
        tools.append(std::move(audio));
    }
    if (state->services.speech) {
        auto speech = tool("Speak", "Synthesize speech with the host's local voice model", "iiVoiceOver", "audio",
            schema({{"text", QJsonObject{{"type", "string"}}}}, "text"));
        speech.execute = [state](const QJsonObject& args, const ToolContext& context) {
            Lease lease(*state, context.cancellation);
            fields(args, {"text"});
            const auto text = prompt(args, "text").toStdString();
            std::stop_source stop;
            std::jthread bridge([token = context.cancellation, &stop](std::stop_token end) {
                while (!end.stop_requested()) {
                    if (token.isCancelled()) { stop.request_stop(); return; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            });
            auto speechOptions = state->services.speechOptions;
            speechOptions.maximum_samples = std::min({speechOptions.maximum_samples,
                (state->services.maximumArtifactBytes - 44) / 2,
                std::size_t(state->services.maximumAudioSeconds * state->services.speech->model().metadata().sample_rate)});
            const auto result = state->services.speech->synthesize(text, speechOptions, stop.get_token());
            context.cancellation.throwIfCancelled();
            require(bool(result), result ? "" : result.error().message.c_str(), ErrorCode::RuntimeFailure);
            return waveArtifact(*state, *result, context, "iiVoiceOver");
        };
        tools.append(std::move(speech));
    }
    return tools;
}
} // namespace iiMultimodalAgent::agent

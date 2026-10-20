#pragma once
#include "PrivateFile.h"
#include <iiMultimodalAgent/agent/MultimodalTools.h>
#include <iiAudioGen/Diffusers.h>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>

namespace iiLocalLLMClient {
inline iiMultimodalAgent::agent::MultimodalServices multimodalConfig(const QString& path, const QString& workspace) {
    using iiLocalLLM::Error; using iiLocalLLM::ErrorCode;
    iiMultimodalAgent::agent::MultimodalServices services;
    if (path.isEmpty()) return services;
    auto require=[](bool ok,const char* message){if(!ok)throw Error(ErrorCode::InvalidArgument,message);};
    const auto canonical=QFileInfo(path).canonicalFilePath();
    require(!canonical.isEmpty() && canonical!=workspace && !canonical.startsWith(workspace.endsWith('/')?workspace:workspace+'/'),
        "Multimodal host configuration must be outside the agent workspace");
    QJsonParseError error;const auto document=QJsonDocument::fromJson(readPrivateFile(path,1024*1024),&error);
    require(error.error==QJsonParseError::NoError && document.isObject(),"Multimodal configuration must be an object");
    auto fields=[&](const QJsonObject& object,QStringList allowed){for(auto it=object.begin();it!=object.end();++it)require(allowed.contains(it.key()),"Unknown multimodal configuration field");};
    auto file=[&](const QJsonObject& object,const char* key){
        const auto value=object.value(QLatin1String(key));require(value.isString()&&!value.toString().isEmpty(),"A configured model/runtime path is required");
        const auto resolved=QFileInfo(value.toString()).canonicalFilePath();require(!resolved.isEmpty(),"Configured model/runtime path does not exist");
        return std::filesystem::path(resolved.toStdU16String());
    };
    const auto root=document.object();fields(root,{"image","audio","speech"});
    for(auto it=root.begin();it!=root.end();++it)require(it.value().isObject(),"Multimodal providers must be objects");
    if(root.contains("image")) {
        const auto image=root["image"].toObject();fields(image,{"model"});
        services.image=iiLocalDiffusion::NativeGenerationRequest{};services.image->modelPath=file(image,"model");
    }
    if(root.contains("audio")) {
        const auto audio=root["audio"].toObject();fields(audio,{"backend","model","python","device"});
        require(audio["backend"].toString()=="diffusers","The CLI audio backend must explicitly select diffusers");
        iiAudioGen::DiffusersOptions options;options.pythonExecutable=file(audio,"python");
        if(audio.contains("device")){require(audio["device"].isString(),"Audio device must be a string");options.device=audio["device"].toString().toStdString();}
        const auto model=file(audio,"model");
        services.audio=std::make_shared<iiAudioGen::Generator>(iiAudioGen::createDiffusersBackend(options));
        const auto loaded=services.audio->loadModel(model);
        if(!loaded)throw Error(ErrorCode::RuntimeFailure,QString::fromStdString(loaded.error().message));
    }
    if(root.contains("speech")) {
        const auto speech=root["speech"].toObject();fields(speech,{"bundled","model","configuration","phonemizer_data"});
        auto voice=[&] {
            if(speech.contains("bundled")) {
                require(speech.size()==1&&speech["bundled"].isBool()&&speech["bundled"].toBool(),"Bundled speech cannot be mixed with custom model paths");
                return iiVoiceOver::VoiceModel::bundled();
            }
            iiVoiceOver::ModelFiles files;files.model=file(speech,"model");files.phonemizer_data=file(speech,"phonemizer_data");
            if(speech.contains("configuration"))files.configuration=file(speech,"configuration");
            return iiVoiceOver::VoiceModel::load(std::move(files));
        }();
        if(!voice)throw Error(ErrorCode::RuntimeFailure,QString::fromStdString(voice.error().message));
        services.speech=std::make_shared<iiVoiceOver::SpeechSynthesizer>(std::move(*voice));
    }
    return services;
}
}

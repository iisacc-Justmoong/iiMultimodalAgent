#include <iiMultimodalAgent.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <iostream>
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        if(iiVoiceOver::SpeechSynthesizer::available())
            throw std::runtime_error("Retired speech model backend advertised as available");
        auto voice=iiVoiceOver::VoiceModel::bundled();
        if(voice || voice.error().code!=iiVoiceOver::ErrorCode::unsupported)
            throw std::runtime_error("Retired speech model was not rejected explicitly");
        QTemporaryDir root(QCoreApplication::applicationDirPath()+"/retired-speech-XXXXXX");
        iiMultimodalAgent::agent::MultimodalServices config;
        config.outputDirectory=std::filesystem::path(root.path().toStdU16String());
        auto tools=iiMultimodalAgent::agent::multimodalTools(std::move(config));
        for(const auto& tool:tools)
            if(tool.definition.name=="Speak")
                throw std::runtime_error("Speech tool exposed without a supported provider");
        std::cout<<"Retired speech backend is unavailable and its tool is omitted\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}

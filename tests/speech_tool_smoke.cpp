#include <iiMultimodalAgent.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <iostream>
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        auto voice=iiVoiceOver::VoiceModel::bundled();
        if(!voice)throw std::runtime_error(voice.error().message);
        QTemporaryDir root;
        iiMultimodalAgent::agent::MultimodalServices config;
        config.outputDirectory=std::filesystem::path(root.path().toStdU16String());
        config.speech=std::make_shared<iiVoiceOver::SpeechSynthesizer>(std::move(*voice));
        auto tools=iiMultimodalAgent::agent::multimodalTools(std::move(config));
        iiMultimodalAgent::agent::ToolContext context;
        auto result=tools.front().execute({{"text","Hello World"}},context);
        if(result.isError||result.data["frames"].toInteger()<1000||result.content.size()!=1)return 2;
        std::cout<<"Native iiVoiceOver tool produced Hello World: "<<result.data["frames"].toInteger()<<" PCM frames\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}

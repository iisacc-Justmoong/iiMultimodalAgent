#include "../tools/MultimodalConfig.h"
#include <iiFileProviderNative.h>
#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtTest/QtTest>

class MultimodalConfigTests final:public QObject {
    Q_OBJECT
private slots:
    void hostConfigIsPrivateOutsideTheWorkspaceAndRejectsUnknownFields() {
        QTemporaryDir root;const auto workspace=root.filePath("work");QDir().mkpath(workspace);
        auto write=[&](QString path,std::string_view bytes){iiFileProvider::native::File::create(std::filesystem::path(path.toStdU16String()),bytes);QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner);};
        const auto path=root.filePath("multimodal.json");write(path,"{}");
        QVERIFY(iiLocalLLMClient::multimodalConfig(path,workspace).image==std::nullopt);
        const auto inside=root.filePath("work/inside.json");write(inside,"{}");
        QVERIFY_THROWS_EXCEPTION(iiLocalLLM::Error,iiLocalLLMClient::multimodalConfig(inside,workspace));
        const auto invalid=root.filePath("invalid.json");write(invalid,"{\"output\":\"/arbitrary/path\"}");
        QVERIFY_THROWS_EXCEPTION(iiLocalLLM::Error,iiLocalLLMClient::multimodalConfig(invalid,workspace));
    }
    void imageBindsAnExistingHostPathAndAudioRequiresExplicitRuntime() {
        QTemporaryDir root;QDir().mkpath(root.filePath("work"));
        const auto model=root.filePath("test.gguf");iiFileProvider::native::File::create(std::filesystem::path(model.toStdU16String()),"fixture");
        const auto config=root.filePath("host.json");
        const auto bytes=QJsonDocument(QJsonObject{{"image",QJsonObject{{"model",model}}}}).toJson();
        iiFileProvider::native::File::create(std::filesystem::path(config.toStdU16String()),std::string_view(bytes.constData(),std::size_t(bytes.size())));
        QFile::setPermissions(config,QFileDevice::ReadOwner|QFileDevice::WriteOwner);
        const auto loaded=iiLocalLLMClient::multimodalConfig(config,root.filePath("work"));
        QVERIFY(loaded.image);QCOMPARE(loaded.image->modelPath,std::filesystem::path(model.toStdU16String()));
        const auto invalid=root.filePath("audio.json");iiFileProvider::native::File::create(std::filesystem::path(invalid.toStdU16String()),"{\"audio\":{\"model\":\"model\"}}");
        QFile::setPermissions(invalid,QFileDevice::ReadOwner|QFileDevice::WriteOwner);
        QVERIFY_THROWS_EXCEPTION(iiLocalLLM::Error,iiLocalLLMClient::multimodalConfig(invalid,root.filePath("work")));
    }
};
QTEST_GUILESS_MAIN(MultimodalConfigTests)
#include "multimodal_config_tests.moc"

#include "native_link.h"
#include <iiMultimodalAgent/agent/SessionHistory.h>
#include <iiMultimodalAgent/agent/SessionStore.h>
#include <iiFileProviderDevice.h>
#include <iiFileProviderPresentation.h>
#include "QtEventWait.h"
#include <QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <chrono>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
struct Fixture {
    iiFileProvider::TemporaryDirectory root{QDir::temp().filePath("history-XXXXXX")};
    QString work=root.filePath("work"),other=root.filePath("other"),directory=root.filePath("sessions");
    a::SessionStore store{directory};QString viewer;
    Fixture(){QDir().mkpath(work);QDir().mkpath(other);viewer=store.create("fixture","private owner header",work).id;}
    QString session(QString text,QString workspace={}) {
        auto s=store.create("fixture","private-system-needle",workspace.isEmpty()?work:workspace);
        auto lease=store.acquire(s.id);a::Message m{{},a::MessageRole::User,text};m.metadata={{"secret","private-metadata-needle"}};lease->append(m);return s.id;
    }
    QString path(const QString& id){return QDir(directory).filePath(id+"/transcript.jsonl");}
};
QJsonArray matches(const QJsonObject& page){return page["matches"].toArray();}
}
class SessionHistoryTests final {
public:
    void previewPublicationKeepsCursorButContentEditsInvalidateIt() {
        Fixture f;
        const auto id=f.session("needle first");
        {auto lease=f.store.acquire(id);lease->append({{},a::MessageRole::User,"needle second"});}
        CTEST_CHECK(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(15)));
        const auto native=std::filesystem::path(f.path(id).toStdU16String());
        (void)iiFileProvider::native::Presentation::prepare(native);
        const iiFileProvider::native::FilePreview preview{"image/png","image",QByteArray::fromBase64(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR4nGNgAAIAAAUAAXpeqz8AAAAASUVORK5CYII=").toStdString(),1,1};
        a::SessionHistory history(f.directory);
        const auto first=history.search(f.viewer,f.work,{{"query","needle"},{"limit",1}});
        const auto cursor=first["next_cursor"].toString();CTEST_CHECK(!cursor.isEmpty());
        for(int publication=0;publication<3;++publication)
            (void)iiFileProvider::native::Presentation::attach(native,preview,{{"publication",std::to_string(publication)}});
        const auto next=history.search(f.viewer,f.work,{{"cursor",cursor}});
        CTEST_COMPARE(matches(next).size(),1);
        const auto changed=history.search(f.viewer,f.work,{{"query","needle"},{"limit",1}})["next_cursor"].toString();
        CTEST_CHECK(!changed.isEmpty());
        iiFileProvider::native::Presentation::cancelPending(native);
        const auto modified=std::filesystem::last_write_time(native);
        const auto size=std::filesystem::file_size(native);
        QFile file(f.path(id));CTEST_CHECK(file.open(QIODevice::ReadWrite));
        auto bytes=file.readAll();CTEST_CHECK(bytes.contains("needle first"));bytes.replace("needle first","needle other");
        CTEST_CHECK(file.seek(0));CTEST_COMPARE(file.write(bytes),bytes.size());CTEST_CHECK(file.flush());
        file.close();std::filesystem::last_write_time(native,modified);
        CTEST_CHECK(std::filesystem::file_size(native)==size);
        CTEST_CHECK(std::filesystem::last_write_time(native)==modified);
        CTEST_THROWS(Error,history.search(f.viewer,f.work,{{"cursor",changed}}));
    }
    void literalUnicodeSearchAndOwnedWorkspace() {
        try {
        Fixture f;const auto own=f.session("Use 한글 and Qt 6.8 [ready].");f.session("foreign 한글",f.other);
        a::SessionHistory history(f.directory);
        auto page=history.search(f.viewer,f.work,{{"query","한글"}});CTEST_COMPARE(matches(page).size(),1);
        const auto hit=matches(page).first().toObject();CTEST_COMPARE(hit["session_id"].toString(),own);
        CTEST_COMPARE(hit["line"].toInt(),2);CTEST_COMPARE(hit["role"].toString(),"user");CTEST_COMPARE(hit["field"].toString(),"text");
        CTEST_CHECK(hit["byte_offset"].toInteger()>0);CTEST_CHECK(hit["snippet"].toString().contains("한글"));
        CTEST_COMPARE(matches(history.search(f.viewer,f.work,{{"query","qT 6.8 ["}})).size(),1);
        CTEST_COMPARE(matches(history.search(f.viewer,f.work,{{"query","Qt.*"}})).size(),0);
        CTEST_COMPARE(matches(history.search(f.viewer,f.work,{{"query","private-system-needle"}})).size(),0);
        CTEST_COMPARE(matches(history.search(f.viewer,f.work,{{"query","private-metadata-needle"}})).size(),0);
        CTEST_THROWS(Error,history.search(f.viewer,f.other,{{"query","한글"}}));
        } catch (const std::exception& error) { agent_ctest::fail(__FILE__, __LINE__, error.what()); }
    }
    void repeatedFreshSearchesShareThePreviewReadBoundary() {
        try {
            Fixture f;const auto id=f.session("initial");a::SessionHistory history(f.directory);
            for(int index=0;index<16;++index) {
                {auto lease=f.store.acquire(id);lease->append({{},a::MessageRole::User,QString("needle %1").arg(index)});}
                const auto page=history.search(f.viewer,f.work,{{"query","needle"},{"session_ids",QJsonArray{id}},{"limit",50}});
                CTEST_COMPARE(matches(page).size(),index+1);
            }
        } catch(const std::exception& error) {agent_ctest::fail(__FILE__, __LINE__, error.what());}
    }
    void paginatesLargeHistoryWithoutWriterLeaseOrDuplicateResults() {
        Fixture f;const auto id=f.session("begin needle");auto lease=f.store.acquire(id);
        for(int i=0;i<32;++i)lease->append({{},a::MessageRole::User,QString(50000,'x')+QString(" needle %1").arg(i)});
        a::SessionHistoryOptions options;options.maxScanBytes=131072;options.maxRecordBytes=65536;
        a::SessionHistory history(f.directory,options);QJsonObject args{{"query","needle"},{"limit",2}};QSet<QString> ids;int pages=0;
        do {
            auto page=history.search(f.viewer,f.work,args);CTEST_CHECK(page["scanned_bytes"].toInteger()<=options.maxScanBytes);
            for(const auto& value:matches(page)){const auto hit=value.toObject();CTEST_CHECK(!ids.contains(hit["message_id"].toString()));ids.insert(hit["message_id"].toString());}
            args={{"cursor",page["next_cursor"]},{"limit",2}};CTEST_CHECK(++pages<100);
            // A model appends its own search result between calls. This must
            // not invalidate a search of older, unchanged conversations.
            {auto viewer=f.store.acquire(f.viewer);viewer->append({{},a::MessageRole::User,"Current query progressed"});}
        }while(!args["cursor"].toString().isEmpty());
        CTEST_COMPARE(ids.size(),33);CTEST_CHECK(pages>16);CTEST_COMPARE(lease->session().messages.size(),33);
    }
    void cursorCannotCrossOwnerQueryOrSnapshot() {
        Fixture f;const auto id=f.session("needle");{auto lease=f.store.acquire(id);lease->append({{},a::MessageRole::User,"needle second"});}
        const auto other=f.session("another session");a::SessionHistory history(f.directory);
        const auto page=history.search(f.viewer,f.work,{{"query","needle"},{"limit",1}});const auto cursor=page["next_cursor"].toString();CTEST_CHECK(!cursor.isEmpty());
        CTEST_THROWS(Error,history.search(other,f.work,{{"cursor",cursor}}));
        CTEST_THROWS(Error,history.search(f.viewer,f.work,{{"cursor",cursor},{"query","changed"}}));
        a::SessionHistory foreign(f.directory);CTEST_THROWS(Error,foreign.search(f.viewer,f.work,{{"cursor",cursor}}));
        {auto lease=f.store.acquire(id);lease->append({{},a::MessageRole::User,"needle appended"});}
        CTEST_THROWS(Error,history.search(f.viewer,f.work,{{"cursor",cursor}}));
    }
    void selectedSessionBoundsAndSymlinks() {
        Fixture f;const auto id=f.session("needle"),other=f.session("foreign needle",f.other);a::SessionHistory history(f.directory);
        CTEST_THROWS(Error,history.search(f.viewer,f.work,{{"query","needle"},{"session_ids",QJsonArray{other}}}));
        CTEST_THROWS(Error,history.search(f.viewer,f.work,{{"query","needle"},{"session_ids",QJsonArray{"../other"}}}));
        CTEST_THROWS(Error,history.search(f.viewer,f.work,{{"query","needle"},{"path",f.path(other)}}));
        const auto linked=QUuid::createUuid().toString(QUuid::WithoutBraces);CTEST_CHECK(createNativeTestLink(QDir(f.directory).filePath(other),QDir(f.directory).filePath(linked)));
        CTEST_COMPARE(matches(history.search(f.viewer,f.work,{{"query","needle"}})).size(),1);
        CTEST_THROWS(Error,history.search(f.viewer,f.work,{{"query","needle"},{"session_ids",QJsonArray{linked}}}));
        const auto ownOther=f.session("linked needle");CTEST_CHECK(QFile::remove(f.path(ownOther)));CTEST_CHECK(createNativeTestLink(f.path(other),f.path(ownOther)));
        CTEST_COMPARE(matches(history.search(f.viewer,f.work,{{"query","needle"}})).size(),1);
    }
    void partialTailRemainsUntouchedAndMalformedRecordsFail() {
        try {
        Fixture f;const auto id=f.session("needle");QFile file(f.path(id));CTEST_CHECK(file.open(QIODevice::Append));file.write("{\"type\":");file.close();
        const auto before=QFileInfo(file).size();a::SessionHistory history(f.directory);
        auto page=history.search(f.viewer,f.work,{{"query","needle"}});CTEST_COMPARE(matches(page).size(),1);
        CTEST_COMPARE(page["incomplete_tails"].toInt(),1);CTEST_COMPARE(QFileInfo(file).size(),before);
        CTEST_CHECK(file.open(QIODevice::Append));file.write("INVALID}\n");file.close();
        CTEST_THROWS(Error,history.search(f.viewer,f.work,{{"query","needle"}}));
        } catch (const std::exception& error) { agent_ctest::fail(__FILE__, __LINE__, error.what()); }
    }
    void cancellationLimitsAndNativeToolValidation() {
        Fixture f;const auto id=f.session("needle");a::SessionHistory history(f.directory);CancellationToken token;token.cancel();
        CTEST_THROWS(Error,history.search(f.viewer,f.work,{{"query","needle"}},token));
        CTEST_THROWS(Error,history.search(f.viewer,f.work,{{"query"," "}}));
        CTEST_THROWS(Error,history.search(f.viewer,f.work,{{"query","needle"},{"limit",1.5}}));
        CTEST_THROWS(Error,history.search(f.viewer,f.work,{{"query","needle"},{"session_ids",QJsonArray{id,id}}}));
        a::ToolRegistry registry;registry.add(history.tool());registry.validateInput("SessionSearch",{{"query","needle"}});
        CTEST_THROWS(Error,registry.validateInput("SessionSearch",{{"query","needle"},{"workspace",f.other}}));
        const auto tool=registry.get("SessionSearch");CTEST_CHECK(tool.definition.readOnly);CTEST_CHECK(!tool.isMcp);
        CTEST_COMPARE(matches(tool.execute({{"query","needle"}},{f.viewer,"run",f.work}).data).size(),1);
        a::SessionHistoryOptions tiny;tiny.maxSessions=2;a::SessionHistory limited(f.directory,tiny);f.session("another");
        CTEST_THROWS(Error,limited.search(f.viewer,f.work,{{"query","needle"}}));
        CTEST_COMPARE(matches(limited.search(f.viewer,f.work,{{"query","needle"},{"session_ids",QJsonArray{id}}})).size(),1);
    }
    void structuredFieldsAndUnicodeExcerpts() {
        try {
        Fixture f;const auto id=f.session("Begin");
        {auto lease=f.store.acquire(id);
            lease->append({{},a::MessageRole::Assistant,{},{{"call","Lookup",{{"query","ARGUMENT_NEEDLE"}}}}});
            a::Message result{{},a::MessageRole::Tool,"Result"};
            result.toolCallId="call";result.data={{"value","DATA_NEEDLE"}};lease->append(result);
            lease->append({{},a::MessageRole::User,QString::fromUtf8("😀").repeated(300)+"UNICODE_NEEDLE"+QString::fromUtf8("😀").repeated(300)});
        }
        a::SessionHistoryOptions options;options.maxSnippetCharacters=512;a::SessionHistory history(f.directory,options);
        auto hit=matches(history.search(f.viewer,f.work,{{"query","ARGUMENT_NEEDLE"}})).first().toObject();CTEST_COMPARE(hit["field"].toString(),"tool_calls");
        hit=matches(history.search(f.viewer,f.work,{{"query","DATA_NEEDLE"}})).first().toObject();CTEST_COMPARE(hit["field"].toString(),"data");
        hit=matches(history.search(f.viewer,f.work,{{"query","UNICODE_NEEDLE"}})).first().toObject();const auto snippet=hit["snippet"].toString();
        CTEST_CHECK(snippet.size()<=512);CTEST_CHECK(snippet.contains("UNICODE_NEEDLE"));CTEST_COMPARE(QString::fromUtf8(snippet.toUtf8()),snippet);CTEST_CHECK(hit["truncated"].toBool());
        } catch (const std::exception& error) { agent_ctest::fail(__FILE__, __LINE__, error.what()); }
    }
    void cursorsAreSingleUseBoundedAndExpiring() {
        Fixture f;const auto id=f.session("needle first");{auto lease=f.store.acquire(id);lease->append({{},a::MessageRole::User,"needle second"});}
        CTEST_CHECK(iiFileProvider::native::Presentation::waitForIdle(std::chrono::seconds(15)));
        a::SessionHistoryOptions options;options.maxCursors=1;options.cursorLifetimeMs=60000;
        a::SessionHistory history(f.directory,options);const QJsonObject args{{"query","needle"},{"limit",1}};
        auto first=history.search(f.viewer,f.work,args);auto cursor=first["next_cursor"].toString();CTEST_CHECK(!cursor.isEmpty());
        CTEST_THROWS(Error,history.search(f.viewer,f.work,args));
        const auto next=history.search(f.viewer,f.work,{{"cursor",cursor}});CTEST_CHECK(next["complete"].toBool());CTEST_COMPARE(matches(next).size(),1);
        CTEST_THROWS(Error,history.search(f.viewer,f.work,{{"cursor",cursor}}));
        // Capacity/single-use checks must not depend on disk I/O finishing in a
        // very short TTL. Exercise expiration on its own search instance.
        options.cursorLifetimeMs=1000;a::SessionHistory expiring(f.directory,options);
        cursor=expiring.search(f.viewer,f.work,args)["next_cursor"].toString();CTEST_CHECK(!cursor.isEmpty());agent_ctest::processFor(1100);
        CTEST_THROWS(Error,expiring.search(f.viewer,f.work,{{"cursor",cursor}}));
        CTEST_CHECK(!expiring.search(f.viewer,f.work,args)["next_cursor"].toString().isEmpty());
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner;
    SessionHistoryTests tests;
    runner.run("previewPublicationKeepsCursorButContentEditsInvalidateIt", [&] { tests.previewPublicationKeepsCursorButContentEditsInvalidateIt(); });
    runner.run("literalUnicodeSearchAndOwnedWorkspace", [&] { tests.literalUnicodeSearchAndOwnedWorkspace(); });
    runner.run("repeatedFreshSearchesShareThePreviewReadBoundary", [&] { tests.repeatedFreshSearchesShareThePreviewReadBoundary(); });
    runner.run("paginatesLargeHistoryWithoutWriterLeaseOrDuplicateResults", [&] { tests.paginatesLargeHistoryWithoutWriterLeaseOrDuplicateResults(); });
    runner.run("cursorCannotCrossOwnerQueryOrSnapshot", [&] { tests.cursorCannotCrossOwnerQueryOrSnapshot(); });
    runner.run("selectedSessionBoundsAndSymlinks", [&] { tests.selectedSessionBoundsAndSymlinks(); });
    runner.run("partialTailRemainsUntouchedAndMalformedRecordsFail", [&] { tests.partialTailRemainsUntouchedAndMalformedRecordsFail(); });
    runner.run("cancellationLimitsAndNativeToolValidation", [&] { tests.cancellationLimitsAndNativeToolValidation(); });
    runner.run("structuredFieldsAndUnicodeExcerpts", [&] { tests.structuredFieldsAndUnicodeExcerpts(); });
    runner.run("cursorsAreSingleUseBoundedAndExpiring", [&] { tests.cursorsAreSingleUseBoundedAndExpiring(); });
    return runner.result();
}

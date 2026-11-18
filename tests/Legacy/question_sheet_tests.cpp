#include <iiMultimodalAgent/agent/QuestionInbox.h>
#include <iiMultimodalAgent/agent/UserQuestions.h>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include "../CTestSupport.h"
#include "../QtEventWait.h"
#include "QtGuiInput.h"
#include <QDir>
#include <QJsonArray>
#include <future>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
namespace {
QJsonObject question(QString text="Which renderer?",bool multi=false) {
    return {{"questions",QJsonArray{QJsonObject{{"question",text},{"header","Renderer"},{"multiSelect",multi},
        {"options",QJsonArray{QJsonObject{{"label","Qt"},{"description","Native"},{"preview","<b>Native preview</b>"}},
            QJsonObject{{"label","Web"},{"description","Browser"},{"preview","**Browser preview**"}}}}}}}};
}
struct Fixture {
    QTemporaryDir directory{QDir::current().filePath("question-ui-XXXXXX")};
    a::QuestionInbox inbox;
    QQmlApplicationEngine engine;
    QStringList warnings;
    QQuickWindow* window=nullptr;
    std::vector<std::future<a::ToolResult>> pending;
    Fixture() {
        QObject::connect(&engine,&QQmlApplicationEngine::warnings,&engine,[this](const QList<QQmlError>& values){for(const auto& value:values)warnings.append(value.toString());});
        engine.addImportPath(QUESTION_LVRS_IMPORT_PATH);
        engine.setInitialProperties({{"testInbox",QVariant::fromValue(&inbox)},
            {"sheetUrl",QUrl::fromLocalFile(QString(QUESTION_QML_DIR)+"/UserQuestionsSheet.qml")}});
        engine.loadData(R"(
            import QtQuick
            import LVRS 1.0 as LV
            LV.ApplicationWindow {
                required property var testInbox
                required property url sheetUrl
                visible: true; width: 960; height: 900
                Loader { id: sheet; objectName: "questionLoader"; Component.onCompleted: setSource(sheetUrl, {inbox: testInbox}) }
            }
        )");
        if(!engine.rootObjects().isEmpty())window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    }
    ~Fixture(){inbox.close();}
    void ask(QJsonObject input=question(),CancellationToken token={}) {
        const auto hook=inbox.hook();const auto cwd=directory.path();const auto id=QString::number(pending.size());
        pending.push_back(std::async(std::launch::async,[=] {
            auto registry=std::make_shared<a::ToolRegistry>();registry->add(a::userQuestionTool({false,"markdown"}));
            a::ToolRunnerOptions options;options.hooks={hook};
            try {return a::ToolRunner(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),options)
                .run({id,"AskUserQuestion",input},{id,"run",cwd,{},token});}
            catch(const Error& error){if(error.code()!=ErrorCode::Cancelled)throw;return a::ToolResult{QString::fromUtf8(error.what()),{},true};}
        }));
    }
    QObject* visual(QQuickItem* item,const char* name) {
        if(item->objectName()==name)return item;
        if(auto* found=item->findChild<QObject*>(name))return found;
        for(auto* child:item->childItems())if(auto* found=visual(child,name))return found;
        return nullptr;
    }
    QObject* object(const char* name) {
        if(!window)return nullptr;
        if(auto* found=window->findChild<QObject*>(name))return found;
        // Popup content is visually reparented into the window overlay.
        return visual(window->contentItem(),name);
    }
    QQuickItem* item(const char* name){return qobject_cast<QQuickItem*>(object(name));}
    bool click(const char* name) {
        auto* target=item(name);if(!target||!target->isVisible())return false;
        const auto point=target->mapToScene(QPointF(target->width()/2,target->height()/2)).toPoint();
        if(!QRect(QPoint(0,0),window->size()).contains(point)){qWarning()<<name<<"outside window"<<point;return false;}
        agent_ctest::clickWindow(window,point);return true;
    }
    bool type(const char* name,const QString& text) {
        auto* wrapper=object(name);if(!wrapper)return false;
        auto* editor=qvariant_cast<QQuickItem*>(wrapper->property("editorItem"));if(!editor)return false;
        editor->forceActiveFocus();QInputMethodEvent input;input.setCommitString(text);QCoreApplication::sendEvent(editor,&input);return true;
    }
};
}
class QuestionSheetTests {
public:
    void singleSelectionAndPlainPreview() {
        Fixture f;CTEST_CHECK_MESSAGE(f.window,qPrintable(f.warnings.join('\n')));f.ask();
        CTEST_WAIT_DEFAULT(f.object("agentQuestionChoice_0_0"));
        CTEST_WAIT_DEFAULT(f.object("agentQuestionsSheet")->property("opened").toBool());
        CTEST_CHECK(f.click("agentQuestionChoice_0_0"));
        CTEST_CHECK(f.click("agentQuestionChoice_0_0"));CTEST_CHECK(f.object("agentQuestionChoice_0_0")->property("checked").toBool());
        CTEST_WAIT_COMPARE(f.object("agentQuestionPreview_0")->property("text").toString(),"<b>Native preview</b>");
        CTEST_COMPARE(f.object("agentQuestionPreview_0")->property("textFormat").toInt(),int(Qt::PlainText));
        f.window->grabWindow().save(QDir::current().filePath("question-before-submit.png"));
        CTEST_CHECK(f.click("agentQuestionSubmit"));CTEST_CHECK_MESSAGE(agent_ctest::waitFor([&] { return f.inbox.requests().isEmpty(); },3000),qPrintable(f.inbox.errorString()));
        const auto result=f.pending.front().get();CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
        CTEST_COMPARE(result.data["answers"].toObject()["Which renderer?"],"Qt");
        CTEST_COMPARE(result.data["annotations"].toObject()["Which renderer?"].toObject()["preview"],"<b>Native preview</b>");
        CTEST_CHECK_MESSAGE(f.warnings.isEmpty(),qPrintable(f.warnings.join('\n')));
    }
    void multilineFreeAnswerAndNotesPreservePrototypeKeys() {
        Fixture f;CTEST_CHECK(f.window);f.ask(question("__proto__",true));
        CTEST_WAIT_DEFAULT(f.object("agentQuestionOption_0_0"));CTEST_WAIT_DEFAULT(f.object("agentQuestionsSheet")->property("opened").toBool());
        CTEST_CHECK(f.click("agentQuestionOption_0_1"));CTEST_CHECK(f.click("agentQuestionOption_0_0"));
        CTEST_CHECK(f.type("agentQuestionOther_0","직접 입력\n두 번째 줄"));
        CTEST_CHECK(f.type("agentQuestionNotes_0","검토 메모\n두 번째 줄"));
        CTEST_CHECK(f.click("agentQuestionSubmit"));CTEST_CHECK_MESSAGE(agent_ctest::waitFor([&] { return f.inbox.requests().isEmpty(); },3000),qPrintable(f.inbox.errorString()));
        const auto result=f.pending.front().get();CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
        CTEST_COMPARE(result.data["answers"].toObject()["__proto__"],"Qt, Web, 직접 입력\n두 번째 줄");
        CTEST_COMPARE(result.data["annotations"].toObject()["__proto__"].toObject()["notes"],"검토 메모\n두 번째 줄");
        CTEST_CHECK_MESSAGE(f.warnings.isEmpty(),qPrintable(f.warnings.join('\n')));
    }
    void newRequestDoesNotResetFocusOrDraftAndSkipAdvances() {
        Fixture f;CTEST_CHECK(f.window);f.ask();
        CTEST_WAIT_DEFAULT(f.object("agentQuestionOther_0"));CTEST_WAIT_DEFAULT(f.object("agentQuestionsSheet")->property("opened").toBool());
        CTEST_CHECK(f.type("agentQuestionOther_0","Draft"));auto* editor=f.window->activeFocusItem();CTEST_CHECK(editor);
        f.ask(question("Second?"));CTEST_WAIT_COMPARE(f.inbox.requests().size(),2);
        CTEST_COMPARE(f.window->activeFocusItem(),editor);CTEST_COMPARE(f.object("agentQuestionOther_0")->property("text").toString(),"Draft");
        CTEST_CHECK(f.click("agentQuestionSkip"));CTEST_WAIT_COMPARE(f.inbox.requests().size(),1);
        CTEST_COMPARE(f.pending.front().get().data["answers"].toObject(),QJsonObject{});
        CTEST_WAIT_COMPARE(f.object("agentQuestionText_0")->property("text").toString(),"Second?");
        CTEST_COMPARE(f.object("agentQuestionOther_0")->property("text").toString(),"");
        CTEST_CHECK(f.click("agentQuestionDecline"));CTEST_WAIT_DEFAULT(f.inbox.requests().isEmpty());CTEST_CHECK(f.pending.back().get().isError);
        CTEST_CHECK_MESSAGE(f.warnings.isEmpty(),qPrintable(f.warnings.join('\n')));
    }
    void cancellationClosesAndEscapeDeclines() {
        Fixture f;CTEST_CHECK(f.window);CancellationToken token;f.ask(question(),token);
        CTEST_WAIT_DEFAULT(f.object("agentQuestionOther_0"));token.cancel();CTEST_WAIT_DEFAULT(f.inbox.requests().isEmpty());
        CTEST_WAIT_DEFAULT(!f.object("agentQuestionsSheet")->property("visible").toBool());CTEST_CHECK(f.pending.front().get().isError);
        f.ask();CTEST_WAIT_DEFAULT(f.object("agentQuestionOther_0"));CTEST_WAIT_DEFAULT(f.object("agentQuestionsSheet")->property("opened").toBool());
        agent_ctest::pressEscape(f.window);CTEST_WAIT_DEFAULT(f.inbox.requests().isEmpty());CTEST_CHECK(f.pending.back().get().isError);
        CTEST_CHECK_MESSAGE(f.warnings.isEmpty(),qPrintable(f.warnings.join('\n')));
    }
    void fourQuestionsAllowPartialFreeAnswers() {
        Fixture f;CTEST_CHECK(f.window);auto input=question("First?");auto rows=input["questions"].toArray();
        for(const auto& text:{"Second?","Third?","Fourth?"})rows.append(question(text)["questions"].toArray().first());
        input["questions"]=rows;f.ask(input);CTEST_WAIT_DEFAULT(f.object("agentQuestionOther_3"));
        CTEST_WAIT_DEFAULT(f.object("agentQuestionsSheet")->property("opened").toBool());
        CTEST_CHECK(f.click("agentQuestionChoice_0_0"));CTEST_CHECK(f.type("agentQuestionOther_0","My own renderer"));
        CTEST_CHECK(!f.object("agentQuestionChoice_0_0")->property("checked").toBool());
        auto* viewport=f.object("sheet_viewport");CTEST_CHECK(viewport);
        CTEST_CHECK(viewport->property("contentHeight").toReal()>viewport->property("height").toReal());
        viewport->setProperty("contentY",viewport->property("contentHeight").toReal()-viewport->property("height").toReal());
        CTEST_CHECK(f.click("agentQuestionSubmit"));CTEST_WAIT_DEFAULT(f.inbox.requests().isEmpty());
        const auto result=f.pending.front().get();CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
        CTEST_COMPARE(result.data["answers"].toObject(),(QJsonObject{{"First?","My own renderer"}}));
        CTEST_CHECK(result.data["annotations"].toObject().isEmpty());
        CTEST_CHECK_MESSAGE(f.warnings.isEmpty(),qPrintable(f.warnings.join('\n')));
    }
    void multipleSelectionsDoNotUseAnUnselectedPreview() {
        Fixture f;CTEST_CHECK(f.window);auto input=question("Renderer?",true);auto row=input["questions"].toArray().first().toObject();
        auto choices=row["options"].toArray();choices.append(QJsonObject{{"label","Qt, Web"},{"description","Another choice"},{"preview","Must not be attached"}});
        row["options"]=choices;input["questions"]=QJsonArray{row};f.ask(input);
        CTEST_WAIT_DEFAULT(f.object("agentQuestionOption_0_2"));CTEST_WAIT_DEFAULT(f.object("agentQuestionsSheet")->property("opened").toBool());
        CTEST_CHECK(f.click("agentQuestionOption_0_0"));CTEST_CHECK(f.click("agentQuestionOption_0_1"));
        CTEST_CHECK(f.click("agentQuestionSubmit"));CTEST_WAIT_DEFAULT(f.inbox.requests().isEmpty());
        const auto result=f.pending.front().get();CTEST_CHECK(!result.isError);
        CTEST_COMPARE(result.data["answers"].toObject()["Renderer?"],"Qt, Web");CTEST_CHECK(result.data["annotations"].toObject().isEmpty());
        CTEST_CHECK_MESSAGE(f.warnings.isEmpty(),qPrintable(f.warnings.join('\n')));
    }
};
int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    QuestionSheetTests tests;
    runner.run("singleSelectionAndPlainPreview", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.singleSelectionAndPlainPreview(); });
    runner.run("multilineFreeAnswerAndNotesPreservePrototypeKeys", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.multilineFreeAnswerAndNotesPreservePrototypeKeys(); });
    runner.run("newRequestDoesNotResetFocusOrDraftAndSkipAdvances", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.newRequestDoesNotResetFocusOrDraftAndSkipAdvances(); });
    runner.run("cancellationClosesAndEscapeDeclines", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.cancellationClosesAndEscapeDeclines(); });
    runner.run("fourQuestionsAllowPartialFreeAnswers", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.fourQuestionsAllowPartialFreeAnswers(); });
    runner.run("multipleSelectionsDoNotUseAnUnselectedPreview", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.multipleSelectionsDoNotUseAnUnselectedPreview(); });
    return runner.result();
}

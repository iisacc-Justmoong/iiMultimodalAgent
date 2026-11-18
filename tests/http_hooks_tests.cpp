#include <iiMultimodalAgent/agent/CommandHooks.h>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include "CTestSupport.h"
#include "QtEventWait.h"
#include <QCoreApplication>
#include "../third_party/cpp-httplib/httplib.h"
#include <future>
#include <mutex>
#include <thread>
using namespace iiLocalLLM; using namespace iiMultimodalAgent;
namespace a=iiMultimodalAgent::agent;
using namespace std::chrono_literals;
namespace {
struct Endpoint {
    httplib::Server server;
    std::thread thread;
    int port=0;
    std::atomic_int calls=0;
    std::mutex mutex;
    QJsonObject received;
    QByteArray auth,filtered,host;
    std::function<void(const httplib::Request&,httplib::Response&)> handler;
    void setHandler(std::function<void(const httplib::Request&,httplib::Response&)> value){std::lock_guard lock(mutex);handler=std::move(value);}
    Endpoint() {
        server.Post(".*",[this](const auto& request,auto& response) {
            ++calls;
            std::function<void(const httplib::Request&,httplib::Response&)> callback;
            {std::lock_guard lock(mutex);
                received=QJsonDocument::fromJson(QByteArray::fromStdString(request.body)).object();
                auth=QByteArray::fromStdString(request.get_header_value("Authorization"));
                filtered=QByteArray::fromStdString(request.get_header_value("X-Filtered"));
                host=QByteArray::fromStdString(request.get_header_value("Host"));
                callback=handler;
            }
            if(callback)callback(request,response);else response.set_content("{}","application/json");
        });
        port=server.bind_to_any_port("127.0.0.1");
        if(port<1)throw std::runtime_error("Cannot bind hook fixture");
        thread=std::thread([this]{server.listen_after_bind();});server.wait_until_ready();
    }
    ~Endpoint(){server.stop();if(thread.joinable())thread.join();}
    QString url(const QString& path="/hook") const{return QString("http://127.0.0.1:%1").arg(port)+path;}
};
QJsonObject settings(const QString& event,QJsonObject hook) {
    return {{"hooks",QJsonObject{{event,QJsonArray{QJsonObject{{"matcher","Write|Edit"},{"hooks",QJsonArray{hook}}}}}}}};
}
QJsonObject hook(const QString& url){return {{"type","http"},{"url",url}};}
a::HookInput input(){return {a::HookKind::BeforeTool,"session","run",{"call","Write",{{"path","original.txt"},{"content","$(touch INJECTED) 한글"}}},{},{}};}
QJsonObject pre(const QString& decision,const QJsonObject& update={}) {
    QJsonObject value{{"hookEventName","PreToolUse"},{"permissionDecision",decision}};
    if(!update.isEmpty())value["updatedInput"]=update;
    return {{"hookSpecificOutput",value}};
}
QJsonObject diagnostic(const a::HookResult& result){return result.diagnostics.last().toObject();}
}
class HttpHooksTests final:public QObject {
public:
    void httpPostsOriginalJsonAndOnlyInterpolatesAllowedHeaders() {
        QTemporaryDir work;Endpoint endpoint;a::CommandHookOptions options;options.workingDirectory=work.path();
        options.environment=QProcessEnvironment{};
        options.environment.insert("HOOK_TOKEN","fixture\r\n token");options.environment.insert("FORBIDDEN","SHOULD_NOT_LEAK");
        auto value=hook(endpoint.url());value["allowedEnvVars"]=QJsonArray{"HOOK_TOKEN"};
        value["headers"]=QJsonObject{{"Authorization","Bearer ${HOOK_TOKEN}"},{"X-Filtered","$FORBIDDEN/${HOOK_TOKEN}"}};
        try {
            a::CommandHooks hooks(settings("PreToolUse",value),options);
            const auto result=hooks.callback()(input(),{});CTEST_COMPARE(diagnostic(result)["outcome"],"success");
            CTEST_COMPARE(endpoint.calls.load(),1);
            {std::lock_guard lock(endpoint.mutex);
                CTEST_COMPARE(endpoint.received["tool_input"],input().call.arguments);
                CTEST_COMPARE(endpoint.received["hook_event_name"],"PreToolUse");CTEST_COMPARE(endpoint.received["cwd"],work.path());
                CTEST_COMPARE(endpoint.auth,"Bearer fixture token");CTEST_COMPARE(endpoint.filtered,"/fixture token");
            }
            const auto described=QJsonDocument(hooks.describe()).toJson();
            CTEST_CHECK(!described.contains("HOOK_TOKEN")&&!described.contains("127.0.0.1")&&!described.contains("fixture token"));
        } catch(const Error& error){agent_ctest::fail(__FILE__, __LINE__, error.what());}
    }
    void successfulHttpDecisionsReachTheRealToolAndRecheckHostDeny() {
        QTemporaryDir work;Endpoint endpoint;a::CommandHookOptions options;options.workingDirectory=work.path();options.environment={};
        endpoint.setHandler([](const auto&,auto& reply){reply.set_content(QJsonDocument(pre("allow",{{"path","approved.txt"},{"content","APPROVED"}})).toJson().toStdString(),"application/json");});
        try {
            a::CommandHooks hooks(settings("PreToolUse",hook(endpoint.url())),options);
            auto registry=std::make_shared<a::ToolRegistry>();a::registerWorkspaceTools(*registry,work.path());
            a::ToolRunner runner(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::DontAsk),{{hooks.callback()}});
            const auto result=runner.run(input().call,{"session","run",work.path()});CTEST_CHECK_MESSAGE(!result.isError,qPrintable(result.text));
            QFile file(work.filePath("approved.txt"));CTEST_CHECK(file.open(QIODevice::ReadOnly));CTEST_COMPARE(file.readAll(),"APPROVED");
            CTEST_CHECK(!QFileInfo::exists(work.filePath("original.txt")));
            a::ToolRunner denied(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::DontAsk,QList<a::PermissionRule>{{"Write",a::PermissionBehavior::Deny}}),{{hooks.callback()}});
            CTEST_CHECK(denied.run(input().call,{"session","run",work.path()}).isError);
        } catch(const Error& error){agent_ctest::fail(__FILE__, __LINE__, error.what());}
    }
    void httpStatusAndMalformedBodiesCannotGrantOrInjectPlainText() {
        QTemporaryDir work;Endpoint endpoint;a::CommandHookOptions options;options.workingDirectory=work.path();options.environment={};options.maxOutputBytes=1024;
        struct Case{int status;QByteArray body;bool success;};
        const QList<Case> cases{{200,"",true},{204,"",true},{200,"{}",true},{200,"{\"async\":true,\"asyncTimeout\":1000}",true},{200,"{\"async\":false}",false},{200,"NOT_JSON",false},{200,"[]",false},
            {200,"{broken",false},{200,QByteArray("{\"value\":\"\xff\"}"),false},{200,QByteArray(2048,'x'),false},
            {500,QJsonDocument(pre("allow")).toJson(),false},{302,QJsonDocument(pre("allow")).toJson(),false}};
        for(const auto& test:cases) {
            endpoint.setHandler([&](const auto&,auto& reply){reply.status=test.status;reply.set_header("Location",endpoint.url("/redirected").toStdString());reply.set_content(test.body.toStdString(),"application/json");});
            try {
                const auto before=endpoint.calls.load();
                const auto result=a::CommandHooks(settings("PreToolUse",hook(endpoint.url())),options).callback()(input(),{});
                CTEST_COMPARE(endpoint.calls.load(),before+1);CTEST_CHECK(!result.permission&&!result.permissionResponse&&!result.block&&!result.stop);CTEST_CHECK(result.feedback.isEmpty());
                CTEST_COMPARE(diagnostic(result)["outcome"],test.success?"success":"non_blocking_error");
            } catch(const Error& error){agent_ctest::fail(__FILE__, __LINE__, error.what());}
        }
    }
    void urlAndEnvironmentPoliciesAreAppliedBeforeAnyRequest() {
        QTemporaryDir work;Endpoint endpoint;a::CommandHookOptions options;options.workingDirectory=work.path();options.environment={};
        options.environment.insert("A","KEPT");options.environment.insert("B","OMITTED");
        auto value=hook(endpoint.url());value["allowedEnvVars"]=QJsonArray{"A","B"};value["headers"]=QJsonObject{{"Authorization","$A/$B"}};
        auto config=settings("PreToolUse",value);config["allowedHttpHookUrls"]=QJsonArray{};
        try {
            const auto rejected=a::CommandHooks(config,options).callback()(input(),{});CTEST_COMPARE(endpoint.calls.load(),0);
            CTEST_COMPARE(diagnostic(rejected)["outcome"],"non_blocking_error");
            config["allowedHttpHookUrls"]=QJsonArray{endpoint.url("/*")};config["httpHookAllowedEnvVars"]=QJsonArray{"A"};
            const auto result=a::CommandHooks(config,options).callback()(input(),{});CTEST_COMPARE(diagnostic(result)["outcome"],"success");
            CTEST_COMPARE(endpoint.calls.load(),1);
            {std::lock_guard lock(endpoint.mutex);CTEST_COMPARE(endpoint.auth,"KEPT/");}
            options.environment.insert("A",QString("INVALID")+QChar(0x7f));
            const auto invalid=a::CommandHooks(config,options).callback()(input(),{});
            CTEST_COMPARE(diagnostic(invalid)["outcome"],"non_blocking_error");CTEST_COMPARE(endpoint.calls.load(),1);
        } catch(const Error& error){agent_ctest::fail(__FILE__, __LINE__, error.what());}
    }
    void privateAndMappedAddressesAreRejectedAndLoopbackDnsWorks() {
        QTemporaryDir work;Endpoint endpoint;a::CommandHookOptions options;options.workingDirectory=work.path();options.environment={};options.timeoutMs=200;
        for(const QString& address:{"0.0.0.1","10.0.0.1","100.64.0.1","100.100.100.200","169.254.169.254","172.16.0.1","172.31.1.1","192.168.1.1",
                "[::]","[fc00::1]","[fd00::1]","[fe80::1]","[febf::1]","[::ffff:192.168.1.1]","[::ffff:a9fe:a9fe]"}) {
            const auto result=a::CommandHooks(settings("PreToolUse",hook("http://"+address+"/hook")),options).callback()(input(),{});
            CTEST_COMPARE(diagnostic(result)["error_code"],"unauthorized");CTEST_COMPARE(endpoint.calls.load(),0);
        }
        auto url=endpoint.url();url.replace("127.0.0.1","localhost");
        const auto result=a::CommandHooks(settings("PreToolUse",hook(url)),options).callback()(input(),{});
        CTEST_COMPARE(diagnostic(result)["outcome"],"success");CTEST_COMPARE(endpoint.calls.load(),1);
        std::lock_guard lock(endpoint.mutex);CTEST_COMPARE(endpoint.host,QString("localhost:%1").arg(endpoint.port).toUtf8());
    }
    void timeoutAndCancellationReleaseCapacityAndOnceIsShared() {
        QTemporaryDir work;Endpoint endpoint;a::CommandHookOptions options;options.workingDirectory=work.path();options.environment={};options.maxConcurrentProcesses=1;
        endpoint.setHandler([](const auto&,auto& reply){std::this_thread::sleep_for(250ms);reply.set_content("{}","application/json");});
        auto value=hook(endpoint.url());value["timeout"]=0.04;
        auto timed=a::CommandHooks(settings("PreToolUse",value),options).callback()(input(),{});
        CTEST_COMPARE(diagnostic(timed)["error_code"],"timeout");
        value["timeout"]=2;value["once"]=true;a::CommandHooks hooks(settings("PreToolUse",value),options);CancellationToken token;
        const auto before=endpoint.calls.load();auto pending=std::async(std::launch::async,[&]{return hooks.callback()(input(),token);});
        CTEST_WAIT_DEFAULT(endpoint.calls.load()>before);token.cancel();
        CTEST_CHECK(pending.wait_for(500ms)==std::future_status::ready);
        try{(void)pending.get();agent_ctest::fail(__FILE__, __LINE__, "Cancelled HTTP hook completed");}catch(const Error& error){CTEST_COMPARE(error.code(),ErrorCode::Cancelled);}
        CTEST_CHECK(hooks.callback()(input(),{}).diagnostics.isEmpty());
        endpoint.setHandler({});
        auto other=input();other.sessionId="second";const auto completed=hooks.callback()(other,{});CTEST_COMPARE(diagnostic(completed)["outcome"],"success");
        const auto count=endpoint.calls.load();std::vector<std::future<a::HookResult>> calls;
        other.sessionId="race";for(int n=0;n<8;++n)calls.push_back(std::async(std::launch::async,[&]{return hooks.callback()(other,{});}));
        for(auto& call:calls)(void)call.get();CTEST_COMPARE(endpoint.calls.load(),count+1);
    }
    void httpPermissionRequestKeepsItsDecisionWhenACommandFinishesLater() {
        QTemporaryDir work;Endpoint endpoint;a::CommandHookOptions options;options.workingDirectory=work.path();options.environment={};
        const QJsonObject decision{{"behavior","allow"},{"updatedInput",QJsonObject{{"path","approved.txt"},{"content","APPROVED"}}}};
        endpoint.setHandler([&](const auto&,auto& reply){reply.set_content(QJsonDocument(QJsonObject{{"hookSpecificOutput",QJsonObject{{"hookEventName","PermissionRequest"},{"decision",decision}}}}).toJson().toStdString(),"application/json");});
        auto config=settings("PermissionRequest",hook(endpoint.url()));auto groups=config["hooks"].toObject();auto group=groups["PermissionRequest"].toArray()[0].toObject();
#if defined(Q_OS_UNIX) && !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
        auto entries=group["hooks"].toArray();entries.append(QJsonObject{{"type","command"},{"command","sleep .15; printf '{\"hookSpecificOutput\":{\"hookEventName\":\"PermissionRequest\",\"decision\":{\"behavior\":\"deny\",\"interrupt\":true}}}'"}});
        group["hooks"]=entries;groups["PermissionRequest"]=QJsonArray{group};config["hooks"]=groups;
#endif
        auto request=input();request.kind=a::HookKind::PermissionRequest;
        const auto result=a::CommandHooks(config,options).callback()(request,{});CTEST_CHECK(result.permissionResponse);
        CTEST_COMPARE(result.permissionResponse->behavior,a::PermissionBehavior::Allow);CTEST_CHECK(!result.stop&&!result.permissionResponse->interrupt);
        CTEST_COMPARE(result.permissionResponse->updatedArguments->value("content"),"APPROVED");
    }
    void unsupportedAndInvalidConfigurationIsRejected() {
        QTemporaryDir work;a::CommandHookOptions options;options.workingDirectory=work.path();options.environment={};
        for(const QJsonObject& value:{hook("file:///tmp/value"),hook("http://user:secret@127.0.0.1/"),hook("http://127.0.0.1/#fragment"),
                QJsonObject{{"type","http"},{"url","http://127.0.0.1/"},{"headers",QJsonObject{{"Host","spoof"}}}},
                QJsonObject{{"type","http"},{"url","http://127.0.0.1/"},{"allowedEnvVars",true}},
                QJsonObject{{"type","http"},{"url","http://127.0.0.1/"},{"async",true}}}) {
            try{a::CommandHooks ignored(settings("PreToolUse",value),options);agent_ctest::fail(__FILE__, __LINE__, "Invalid HTTP hook accepted");}catch(const Error&){}
        }
    }
    void environmentProxyAndNoProxyUseHostOwnedRouting() {
        QTemporaryDir work;Endpoint proxy;a::CommandHookOptions options;options.workingDirectory=work.path();options.environment={};
        options.environment.insert("HTTPS_PROXY",proxy.url("/"));
        auto config=settings("PreToolUse",hook("http://10.0.0.2/hook"));
        const auto routed=a::CommandHooks(config,options).callback()(input(),{});
        CTEST_COMPARE(diagnostic(routed)["outcome"],"success");CTEST_COMPARE(proxy.calls.load(),1);
        {std::lock_guard lock(proxy.mutex);CTEST_COMPARE(proxy.host,"10.0.0.2");}
        options.environment.insert("NO_PROXY","10.0.0.2");
        const auto direct=a::CommandHooks(config,options).callback()(input(),{});
        CTEST_COMPARE(diagnostic(direct)["error_code"],"unauthorized");CTEST_COMPARE(proxy.calls.load(),1);
        options.environment.remove("NO_PROXY");options.httpProxy=QNetworkProxy(QNetworkProxy::NoProxy);
        const auto disabled=a::CommandHooks(config,options).callback()(input(),{});
        CTEST_COMPARE(diagnostic(disabled)["error_code"],"unauthorized");CTEST_COMPARE(proxy.calls.load(),1);
    }
    void duplicateHttpUrlsUseLastMatchedConfigurationButKeepDistinctConditions() {
        QTemporaryDir work;Endpoint endpoint;a::CommandHookOptions options;options.workingDirectory=work.path();options.environment={};
        auto first=hook(endpoint.url()),last=first;
        first["headers"]=QJsonObject{{"Authorization","FIRST"}};last["headers"]=QJsonObject{{"Authorization","LAST"}};
        const auto config=[&](QJsonArray values){return QJsonObject{{"hooks",QJsonObject{{"PreToolUse",QJsonArray{
            QJsonObject{{"matcher","Write"},{"hooks",values}},
            QJsonObject{{"matcher","Read"},{"hooks",QJsonArray{first}}}}}}}};};
        const auto deduplicated=a::CommandHooks(config({first,last}),options).callback()(input(),{});
        CTEST_COMPARE(endpoint.calls.load(),1);CTEST_COMPARE(deduplicated.diagnostics.size(),1);
        {std::lock_guard lock(endpoint.mutex);CTEST_COMPARE(endpoint.auth,"LAST");}
        first["if"]="Write";last["if"]="Write(original.txt)";
        const auto distinct=a::CommandHooks(config({first,last}),options).callback()(input(),{});
        CTEST_COMPARE(endpoint.calls.load(),3);CTEST_COMPARE(distinct.diagnostics.size(),2);
    }
    void invalidHeaderNamesAreRejectedBeforeNetworkIo() {
        QTemporaryDir work;Endpoint endpoint;a::CommandHookOptions options;options.workingDirectory=work.path();options.environment={};
        for(const QString& name:{QString("X-Policy\n"),QString("X-Policy\r\n"),QString("X Policy"),QString("X-Policy:")}) {
            auto value=hook(endpoint.url());value["headers"]=QJsonObject{{name,"VALUE"}};
            bool rejected=false;try{a::CommandHooks ignored(settings("PreToolUse",value),options);}catch(const Error&){rejected=true;}
            CTEST_CHECK_MESSAGE(rejected,qPrintable("Accepted header name: "+name));
        }
        CTEST_COMPARE(endpoint.calls.load(),0);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    HttpHooksTests tests;
    runner.run("httpPostsOriginalJsonAndOnlyInterpolatesAllowedHeaders", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.httpPostsOriginalJsonAndOnlyInterpolatesAllowedHeaders(); });
    runner.run("successfulHttpDecisionsReachTheRealToolAndRecheckHostDeny", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.successfulHttpDecisionsReachTheRealToolAndRecheckHostDeny(); });
    runner.run("httpStatusAndMalformedBodiesCannotGrantOrInjectPlainText", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.httpStatusAndMalformedBodiesCannotGrantOrInjectPlainText(); });
    runner.run("urlAndEnvironmentPoliciesAreAppliedBeforeAnyRequest", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.urlAndEnvironmentPoliciesAreAppliedBeforeAnyRequest(); });
    runner.run("privateAndMappedAddressesAreRejectedAndLoopbackDnsWorks", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.privateAndMappedAddressesAreRejectedAndLoopbackDnsWorks(); });
    runner.run("timeoutAndCancellationReleaseCapacityAndOnceIsShared", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.timeoutAndCancellationReleaseCapacityAndOnceIsShared(); });
    runner.run("httpPermissionRequestKeepsItsDecisionWhenACommandFinishesLater", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.httpPermissionRequestKeepsItsDecisionWhenACommandFinishesLater(); });
    runner.run("unsupportedAndInvalidConfigurationIsRejected", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.unsupportedAndInvalidConfigurationIsRejected(); });
    runner.run("environmentProxyAndNoProxyUseHostOwnedRouting", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.environmentProxyAndNoProxyUseHostOwnedRouting(); });
    runner.run("duplicateHttpUrlsUseLastMatchedConfigurationButKeepDistinctConditions", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.duplicateHttpUrlsUseLastMatchedConfigurationButKeepDistinctConditions(); });
    runner.run("invalidHeaderNamesAreRejectedBeforeNetworkIo", [&] { agent_ctest::ScopedDeferredDeletes cleanup; tests.invalidHeaderNamesAreRejectedBeforeNetworkIo(); });
    return runner.result();
}

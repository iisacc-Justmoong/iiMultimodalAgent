#include "GraphQL.h"
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslSocket>

namespace iiMultimodalAgent::agent {
namespace {
void require(bool value,const char* message,ErrorCode code=ErrorCode::InvalidArgument) {
    if(!value) throw Error(code,message);
}
QJsonObject nativeExecute(const GraphQLEndpoint& endpoint,const QJsonObject& payload,
    const CancellationToken& cancellation,int timeout,int maximum) {
    cancellation.throwIfCancelled();
    QNetworkAccessManager manager;manager.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    QNetworkRequest request(endpoint.url);
    auto ssl=QSslConfiguration::defaultConfiguration();ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);request.setSslConfiguration(ssl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute,false);
    request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");
    request.setRawHeader("Accept","application/graphql-response+json, application/json");
    if(!endpoint.authorization.isEmpty())request.setRawHeader("Authorization",endpoint.authorization);
    auto reply=std::unique_ptr<QNetworkReply>(manager.post(request,QJsonDocument(payload).toJson(QJsonDocument::Compact)));
    reply->setReadBufferSize(65536);
    QEventLoop loop;QTimer timer;QElapsedTimer elapsed;elapsed.start();timer.setInterval(10);
    QByteArray body;bool overflow=false,timedOut=false;
    auto drain=[&] {
        if(!reply->isOpen())return;
        body+=reply->read(maximum+1-body.size());
        if(body.size()>maximum){overflow=true;reply->abort();loop.quit();}
    };
    QObject::connect(reply.get(),&QNetworkReply::readyRead,&loop,drain);
    QObject::connect(reply.get(),&QNetworkReply::finished,&loop,&QEventLoop::quit);
    QObject::connect(&timer,&QTimer::timeout,&loop,[&] {
        if(cancellation.isCancelled() || elapsed.elapsed()>=timeout) {
            timedOut=!cancellation.isCancelled();reply->abort();loop.quit();
        }
    });
    timer.start();if(!reply->isFinished())loop.exec();drain();cancellation.throwIfCancelled();
    require(!timedOut,"GraphQL request timed out",ErrorCode::Timeout);
    require(!overflow,"GraphQL response exceeds byte limit",ErrorCode::ResourceLimit);
    const auto status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    require((status>=200 && status<300 && reply->error()==QNetworkReply::NoError) || status==400 || status==422,
        "GraphQL HTTP request failed",ErrorCode::RuntimeFailure);
    const auto mime=reply->rawHeader("Content-Type").split(';').first().trimmed().toLower();
    require(mime=="application/json" || mime=="application/graphql-response+json",
        "GraphQL response must use a JSON media type",ErrorCode::ProtocolError);
    QJsonParseError error;const auto document=QJsonDocument::fromJson(body,&error);
    require(error.error==QJsonParseError::NoError && document.isObject(),"Invalid GraphQL response JSON",ErrorCode::ProtocolError);
    const auto result=document.object();
    if(status>=400) require(result.value("errors").isArray() && !result.value("errors").toArray().isEmpty(),
        "GraphQL HTTP error response has no errors",ErrorCode::ProtocolError);
    return result;
}
}
Tool graphQLTool(GraphQLOptions options,GraphQLExecutor executor) {
    require(!options.endpoints.isEmpty() && options.endpoints.size()<=128 && options.timeoutMs>=1 && options.timeoutMs<=300000
        && options.maxResponseBytes>=1 && options.maxResponseBytes<=8*1024*1024,"Invalid GraphQL host configuration");
    QJsonArray ids;const QRegularExpression validId("^[A-Za-z0-9_.-]{1,64}$");
    for(auto it=options.endpoints.cbegin();it!=options.endpoints.cend();++it) {
        const auto& url=it.value().url;const auto& authorization=it.value().authorization;
        require(validId.match(it.key()).hasMatch() && url.isValid() && (url.scheme()=="https" || url.scheme()=="http")
            && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasFragment()
            && authorization.size()<=8192 && !authorization.contains('\r') && !authorization.contains('\n')
            && !authorization.contains('\0'),"Invalid GraphQL endpoint configuration");
        ids.append(it.key());
    }
    if(!executor)executor=nativeExecute;
    Tool tool;
    tool.definition={"GraphQL","Execute a GraphQL operation on a host-configured endpoint. Mutations can change remote data.",
        {{"type","object"},{"properties",QJsonObject{
            {"endpoint",QJsonObject{{"type","string"},{"enum",ids}}},
            {"query",QJsonObject{{"type","string"},{"minLength",1},{"maxLength",65536}}},
            {"variables",QJsonObject{{"type","object"}}},
            {"operation_name",QJsonObject{{"type","string"},{"maxLength",256}}}}},
            {"required",QJsonArray{"endpoint","query"}},{"additionalProperties",false}}};
    tool.definition.metadata={{"source","builtin.graphql"}};
    tool.execute=[options=std::move(options),executor=std::move(executor)](const QJsonObject& args,const ToolContext& context) {
        require(args["endpoint"].isString() && args["query"].isString(),"GraphQL endpoint and query are required");
        const auto found=options.endpoints.constFind(args["endpoint"].toString());
        require(found!=options.endpoints.cend(),"Unknown GraphQL endpoint ID");
        const auto query=args["query"].toString();
        require(!query.trimmed().isEmpty() && !query.contains(QChar(0)) && query.toUtf8().size()<=65536,"Invalid GraphQL query");
        require(!args.contains("variables") || args["variables"].isObject(),"GraphQL variables must be an object");
        require(!args.contains("operation_name") || (args["operation_name"].isString() && args["operation_name"].toString().size()<=256),"Invalid GraphQL operation name");
        for(auto it=args.begin();it!=args.end();++it)
            require(it.key()=="endpoint" || it.key()=="query" || it.key()=="variables" || it.key()=="operation_name","Unknown GraphQL option");
        QJsonObject payload{{"query",query}};
        if(args.contains("variables"))payload.insert("variables",args["variables"]);
        if(args.contains("operation_name"))payload.insert("operationName",args["operation_name"]);
        require(QJsonDocument(payload).toJson(QJsonDocument::Compact).size()<=1024*1024,"GraphQL request exceeds byte limit",ErrorCode::ResourceLimit);
        context.cancellation.throwIfCancelled();const auto response=executor(*found,payload,context.cancellation,options.timeoutMs,options.maxResponseBytes);
        context.cancellation.throwIfCancelled();
        require(response.contains("data") || response.contains("errors"),"GraphQL response lacks data and errors",ErrorCode::ProtocolError);
        require(!response.contains("data") || response["data"].isObject() || response["data"].isNull(),"Invalid GraphQL data",ErrorCode::ProtocolError);
        require(!response.contains("errors") || (response["errors"].isArray() && !response["errors"].toArray().isEmpty()),"Invalid GraphQL errors",ErrorCode::ProtocolError);
        if(response.contains("errors"))for(const auto& entry:response["errors"].toArray())
            require(entry.isObject() && entry.toObject()["message"].isString(),"Invalid GraphQL error",ErrorCode::ProtocolError);
        const auto bytes=QJsonDocument(response).toJson(QJsonDocument::Compact);
        require(bytes.size()<=options.maxResponseBytes,"GraphQL response exceeds byte limit",ErrorCode::ResourceLimit);
        return ToolResult{QString::fromUtf8(bytes),response,response.contains("errors")};
    };
    return tool;
}
}

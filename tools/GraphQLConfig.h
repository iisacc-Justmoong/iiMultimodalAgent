#pragma once
#include "PrivateFile.h"
#include <iiMultimodalAgent/agent/GraphQL.h>
#include <QtCore/QJsonDocument>

namespace iiLocalLLMClient {
inline iiMultimodalAgent::agent::GraphQLOptions graphQLConfig(const QString& path,const QString& workspace) {
    using namespace iiMultimodalAgent::agent;
    const auto canonical=QFileInfo(path).canonicalFilePath();
    const auto prefix=workspace.endsWith('/')?workspace:workspace+'/';
#ifdef Q_OS_WIN
    constexpr auto comparison=Qt::CaseInsensitive;
#else
    constexpr auto comparison=Qt::CaseSensitive;
#endif
    if(canonical.isEmpty() || canonical.compare(workspace,comparison)==0 || canonical.startsWith(prefix,comparison))
        throw std::runtime_error("GraphQL configuration must be private and outside the workspace");
    QJsonParseError error;const auto doc=QJsonDocument::fromJson(readPrivateFile(path,1024*1024),&error);
    if(error.error!=QJsonParseError::NoError || !doc.isObject())throw std::runtime_error("Invalid GraphQL configuration");
    const auto root=doc.object();
    for(auto it=root.begin();it!=root.end();++it)
        if(it.key()!="endpoints" && it.key()!="timeout_ms" && it.key()!="max_response_bytes")throw std::runtime_error("Unknown GraphQL configuration field");
    if(!root["endpoints"].isObject())throw std::runtime_error("GraphQL endpoints must be an object");
    GraphQLOptions options;
    auto integer=[&](const char* key,int fallback,int max) {
        const auto value=root.value(QLatin1String(key));if(value.isUndefined())return fallback;
        if(!value.isDouble() || value.toDouble()!=value.toInt() || value.toInt()<1 || value.toInt()>max)
            throw std::runtime_error("Invalid GraphQL limit");return value.toInt();
    };
    options.timeoutMs=integer("timeout_ms",options.timeoutMs,300000);
    options.maxResponseBytes=integer("max_response_bytes",options.maxResponseBytes,8*1024*1024);
    const auto endpoints=root["endpoints"].toObject();
    for(auto it=endpoints.begin();it!=endpoints.end();++it) {
        if(!it.value().isObject())throw std::runtime_error("GraphQL endpoint must be an object");
        const auto entry=it.value().toObject();
        for(auto field=entry.begin();field!=entry.end();++field)
            if(field.key()!="url" && field.key()!="authorization")throw std::runtime_error("Unknown GraphQL endpoint field");
        if(!entry["url"].isString() || (entry.contains("authorization")&&!entry["authorization"].isString()))
            throw std::runtime_error("Invalid GraphQL endpoint fields");
        options.endpoints.insert(it.key(),{QUrl(entry["url"].toString(),QUrl::StrictMode),entry["authorization"].toString().toUtf8()});
    }
    return options;
}
}

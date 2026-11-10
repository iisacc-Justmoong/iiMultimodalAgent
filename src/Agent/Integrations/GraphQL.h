#pragma once
#include <iiMultimodalAgent/agent/Tools.h>
#include <QtCore/QUrl>

namespace iiMultimodalAgent::agent {
// Host-owned destinations and credentials. The model only selects an ID.
struct GraphQLEndpoint {
    QUrl url;
    QByteArray authorization;
};
struct GraphQLOptions {
    QMap<QString,GraphQLEndpoint> endpoints;
    int timeoutMs = 30000;
    int maxResponseBytes = 1024 * 1024;
};
// A host may replace the native transport without coupling the agent to a
// network SDK. The callable is invoked only after normal tool admission.
using GraphQLExecutor = std::function<QJsonObject(const GraphQLEndpoint&,const QJsonObject&,
    const CancellationToken&,int,int)>;
[[nodiscard]] IIMULTIMODALAGENT_EXPORT Tool graphQLTool(GraphQLOptions options, GraphQLExecutor executor = {});
}

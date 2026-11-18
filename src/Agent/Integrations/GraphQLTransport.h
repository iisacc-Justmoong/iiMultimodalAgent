#pragma once

#include "GraphQL.h"
#include <iiNetwork/Application.h>

namespace iiMultimodalAgent::agent::detail {

// These operations keep GraphQL policy independent of the event-loop driver.
[[nodiscard]] ii::network::HttpRequest graphQLRequest(
    const GraphQLEndpoint&, const QJsonObject&, int timeoutMs, int maximumBytes);
[[nodiscard]] ii::network::HttpResponse graphQLExchange(
    ii::network::HttpRequest, const iiLocalLLM::CancellationToken&);
// The injected transport delivers completion on the supplied loop's thread.
[[nodiscard]] ii::network::HttpResponse graphQLExchange(
    ii::network::HttpRequest, const iiLocalLLM::CancellationToken&,
    ii::runtime::EventLoop&, ii::network::HttpTransport&);
[[nodiscard]] QJsonObject graphQLResponse(
    const ii::network::HttpResponse&, const iiLocalLLM::CancellationToken&, int maximumBytes);

} // namespace iiMultimodalAgent::agent::detail

#include "GraphQLTransport.h"

#include <iiSDKCore/Runtime/EventLoop.h>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <chrono>
#include <optional>
#include <utility>

namespace iiMultimodalAgent::agent::detail {
namespace {
void require(bool condition, const char* message, iiLocalLLM::ErrorCode code)
{
    if (!condition) throw iiLocalLLM::Error(code, message);
}
}

ii::network::HttpRequest graphQLRequest(const GraphQLEndpoint& endpoint,
    const QJsonObject& payload, int timeoutMs, int maximumBytes)
{
    ii::network::HttpRequest request;
    request.url = endpoint.url.toEncoded().toStdString();
    request.set_method(ii::network::HttpMethod::post);
    request.body = QJsonDocument(payload).toJson(QJsonDocument::Compact).toStdString();
    request.headers.emplace("Content-Type", "application/json");
    request.headers.emplace("Accept", "application/graphql-response+json, application/json");
    if (!endpoint.authorization.isEmpty())
        request.headers.emplace("Authorization", endpoint.authorization.toStdString());
    request.timeout = std::chrono::milliseconds(timeoutMs);
    request.maximum_bytes = static_cast<std::size_t>(maximumBytes);
    request.proxy.type = ii::network::ProxyType::disabled;
    request.follow_redirects = false;
    request.enable_cookies = false;
    return request;
}

ii::network::HttpResponse graphQLExchange(ii::network::HttpRequest request,
    const iiLocalLLM::CancellationToken& cancellation,
    ii::runtime::EventLoop& loop, ii::network::HttpTransport& transport)
{
    cancellation.throwIfCancelled();
    std::optional<ii::network::HttpResponse> response;
    struct Pending final {
        ii::network::HttpTransport& transport;
        bool admitted = false;
        bool completed = false;
        ~Pending() noexcept {
            if (admitted && !completed) {
                try { transport.cancel(); } catch (...) {}
            }
        }
    } pending{transport};
    const auto poll = loop.every(std::chrono::milliseconds(10), [&] {
        if (cancellation.isCancelled()) loop.request_stop();
    });
    require(bool(poll), "GraphQL cancellation monitor could not start", iiLocalLLM::ErrorCode::RuntimeFailure);
    struct Poll final {
        ii::runtime::TaskToken token;
        ~Poll() { (void)token.cancel(); }
    } pollGuard{poll.token};
    const auto admitted = transport.start(std::move(request), [&](auto value) {
        pending.completed = true;
        response.emplace(std::move(value));
        loop.request_stop();
    });
    require(admitted, "GraphQL HTTP request could not start", iiLocalLLM::ErrorCode::RuntimeFailure);
    pending.admitted = true;
    loop.run();
    cancellation.throwIfCancelled();
    require(response.has_value(), "GraphQL HTTP request did not complete", iiLocalLLM::ErrorCode::RuntimeFailure);
    return std::move(*response);
}

ii::network::HttpResponse graphQLExchange(ii::network::HttpRequest request,
    const iiLocalLLM::CancellationToken& cancellation)
{
    cancellation.throwIfCancelled();
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    throw iiLocalLLM::Error(iiLocalLLM::ErrorCode::RuntimeFailure,
        "Built-in GraphQL transport requires a worker; provide a host GraphQLExecutor");
#else
    ii::runtime::EventLoop loop;
    ii::network::NativeHttpTransport transport(loop);
    return graphQLExchange(std::move(request), cancellation, loop, transport);
#endif
}

QJsonObject graphQLResponse(const ii::network::HttpResponse& response,
    const iiLocalLLM::CancellationToken& cancellation, int maximumBytes)
{
    using iiLocalLLM::ErrorCode;
    cancellation.throwIfCancelled();
    require(!response.cancelled, "GraphQL request was cancelled", ErrorCode::Cancelled);
    require(!response.timed_out, "GraphQL request timed out", ErrorCode::Timeout);
    require(!response.too_large && response.body.size() <= static_cast<std::size_t>(maximumBytes),
        "GraphQL response exceeds byte limit", ErrorCode::ResourceLimit);
    require(response.error.empty() && ((response.status >= 200 && response.status < 300)
            || response.status == 400 || response.status == 422),
        "GraphQL HTTP request failed", ErrorCode::RuntimeFailure);
    const auto mime = QByteArray::fromStdString(response.content_type).split(';').first().trimmed().toLower();
    require(mime == "application/json" || mime == "application/graphql-response+json",
        "GraphQL response must use a JSON media type", ErrorCode::ProtocolError);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(response.body), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(),
        "Invalid GraphQL response JSON", ErrorCode::ProtocolError);
    const auto result = document.object();
    if (response.status >= 400)
        require(result.value("errors").isArray() && !result.value("errors").toArray().isEmpty(),
            "GraphQL HTTP error response has no errors", ErrorCode::ProtocolError);
    return result;
}

} // namespace iiMultimodalAgent::agent::detail

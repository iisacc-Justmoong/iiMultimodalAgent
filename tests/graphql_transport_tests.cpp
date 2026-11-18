#include "../src/Agent/Integrations/GraphQLTransport.h"
#include "CTestSupport.h"
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <functional>

using namespace iiMultimodalAgent::agent;
using namespace iiLocalLLM;
namespace net = ii::network;
namespace transport = iiMultimodalAgent::agent::detail;

class FakeTransport final : public net::HttpTransport {
public:
    explicit FakeTransport(ii::runtime::EventLoop& eventLoop) : loop(eventLoop) {}
    bool start(net::HttpRequest value, std::function<void(net::HttpResponse)> completion) override
    {
        ++starts; request = std::move(value);
        if (!admit) return false;
        active = true;
        if (!complete) return true;
        const auto queued = loop.after(std::chrono::milliseconds(1), [this, completion = std::move(completion)]() mutable {
            if (!active) return;
            active = false;
            net::HttpResponse value; value.status = 200;
            completion(std::move(value));
        });
        if (!queued) active = false;
        return bool(queued);
    }
    void cancel() override { cancelled = true; active = false; }
    [[nodiscard]] bool busy() const override { return active; }
    ii::runtime::EventLoop& loop;
    net::HttpRequest request;
    bool admit = true, complete = true, cancelled = false, active = false;
    int starts = 0;
};

static void expects(ErrorCode expected, const std::function<void()>& action)
{
    try { action(); }
    catch (const Error& error) { CTEST_COMPARE(error.code(), expected); return; }
    agent_ctest::fail(__FILE__, __LINE__, "Expected a structured GraphQL transport error");
}

int main()
{
    agent_ctest::CTestRunner runner;
    runner.run("request_preserves_host_policy", [] {
        GraphQLEndpoint endpoint{QUrl("https://example.test/graphql?tag=a%20b"), "Bearer fixture"};
        QJsonObject payload{{"query", "query { ping }"}, {"variables", QJsonObject{{"id", 7}}}};
        const auto request = transport::graphQLRequest(endpoint, payload, 1234, 3210);
        CTEST_COMPARE(request.url, endpoint.url.toEncoded().toStdString());
        CTEST_COMPARE(request.method, "POST");
        CTEST_COMPARE(request.headers.at("Content-Type"), "application/json");
        CTEST_COMPARE(request.headers.at("Accept"), "application/graphql-response+json, application/json");
        CTEST_COMPARE(request.headers.at("Authorization"), "Bearer fixture");
        CTEST_COMPARE(request.timeout.count(), 1234);
        CTEST_COMPARE(request.maximum_bytes, std::size_t(3210));
        CTEST_CHECK(!request.follow_redirects && !request.enable_cookies);
        CTEST_COMPARE(request.proxy.type, net::ProxyType::disabled);
        CTEST_COMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(request.body)).object(), payload);
        endpoint.authorization.clear();
        CTEST_CHECK(!transport::graphQLRequest(endpoint, payload, 1, 1).headers.contains("Authorization"));
    });
    runner.run("json_success_and_graphql_error", [] {
        CancellationToken token;
        net::HttpResponse response; response.status = 200;
        response.content_type = "Application/JSON; charset=utf-8";
        response.body = R"({"data":{"value":7}})";
        CTEST_COMPARE(transport::graphQLResponse(response, token, 1024)["data"].toObject()["value"], 7);
        response.status = 422; response.content_type = "application/graphql-response+json";
        response.body = R"({"errors":[{"message":"invalid"}]})";
        CTEST_COMPARE(transport::graphQLResponse(response, token, 1024)["errors"].toArray().size(), 1);
    });
    runner.run("injected_transport_completes_without_a_server", [] {
        ii::runtime::EventLoop loop; FakeTransport fake(loop); CancellationToken token;
        net::HttpRequest request; request.url = "https://example.test/graphql";
        const auto response = transport::graphQLExchange(request, token, loop, fake);
        CTEST_COMPARE(response.status, 200);
        CTEST_COMPARE(fake.starts, 1);
        CTEST_COMPARE(fake.request.url, request.url);
        CTEST_CHECK(!fake.cancelled);
    });
    runner.run("injected_transport_rejection_and_cancellation", [] {
        {
            ii::runtime::EventLoop loop; FakeTransport fake(loop); fake.admit = false; CancellationToken token;
            expects(ErrorCode::RuntimeFailure, [&] { (void)transport::graphQLExchange({}, token, loop, fake); });
            CTEST_COMPARE(fake.starts, 1);
            token.cancel();
            CTEST_CHECK(loop.after(std::chrono::milliseconds(20), [&] { loop.request_stop(); }));
            loop.run();
            CTEST_CHECK(!fake.cancelled);
        }
        {
            ii::runtime::EventLoop loop; FakeTransport fake(loop); fake.complete = false; CancellationToken token;
            CTEST_CHECK(loop.after(std::chrono::milliseconds(1), [&] { token.cancel(); }));
            expects(ErrorCode::Cancelled, [&] { (void)transport::graphQLExchange({}, token, loop, fake); });
            CTEST_CHECK(fake.cancelled);
        }
        {
            ii::runtime::EventLoop loop; FakeTransport fake(loop); fake.complete = false; CancellationToken token;
            CTEST_CHECK(loop.after(std::chrono::milliseconds(1), [&] { loop.request_stop(); }));
            expects(ErrorCode::RuntimeFailure, [&] { (void)transport::graphQLExchange({}, token, loop, fake); });
            CTEST_CHECK(fake.cancelled);
        }
    });
    runner.run("network_timeout_size_and_cancel", [] {
        CancellationToken token; net::HttpResponse response;
        response.timed_out = true;
        expects(ErrorCode::Timeout, [&] { (void)transport::graphQLResponse(response, token, 8); });
        response.timed_out = false; response.too_large = true;
        expects(ErrorCode::ResourceLimit, [&] { (void)transport::graphQLResponse(response, token, 8); });
        response.too_large = false; response.status = 200; response.body = "123456789";
        expects(ErrorCode::ResourceLimit, [&] { (void)transport::graphQLResponse(response, token, 8); });
        response.body.clear(); response.error = "connection failed";
        expects(ErrorCode::RuntimeFailure, [&] { (void)transport::graphQLResponse(response, token, 8); });
        token.cancel();
        expects(ErrorCode::Cancelled, [&] { (void)transport::graphQLResponse(response, token, 8); });
        expects(ErrorCode::Cancelled, [&] { (void)transport::graphQLExchange({}, token); });
    });
    runner.run("partial_http_error_body_is_never_accepted", [] {
        CancellationToken token; net::HttpResponse response;
        response.content_type = "application/json";
        response.body = R"({"errors":[{"message":"partial"}]})";
        response.error = "connection terminated before response completed";
        for (const int status : {400, 422}) {
            response.status = status;
            expects(ErrorCode::RuntimeFailure, [&] { (void)transport::graphQLResponse(response, token, 1024); });
        }
        response.error.clear(); response.status = 200; response.cancelled = true;
        expects(ErrorCode::Cancelled, [&] { (void)transport::graphQLResponse(response, token, 1024); });
    });
    runner.run("redirect_media_and_json_are_validated", [] {
        CancellationToken token; net::HttpResponse response;
        response.status = 302; response.content_type = "application/json"; response.body = R"({"data":{}})";
        expects(ErrorCode::RuntimeFailure, [&] { (void)transport::graphQLResponse(response, token, 1024); });
        response.status = 200; response.content_type = "text/html";
        expects(ErrorCode::ProtocolError, [&] { (void)transport::graphQLResponse(response, token, 1024); });
        response.content_type = "application/json"; response.body = "[1,2]";
        expects(ErrorCode::ProtocolError, [&] { (void)transport::graphQLResponse(response, token, 1024); });
        response.status = 400; response.body = R"({"data":{}})";
        expects(ErrorCode::ProtocolError, [&] { (void)transport::graphQLResponse(response, token, 1024); });
    });
    return runner.result();
}

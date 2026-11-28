#include "drogon_adapter.hpp"

#include <string>

namespace faulttwin::server {

std::optional<api::HttpRequest> to_api_request(const drogon::HttpRequestPtr& request) {
    api::HttpRequest out;
    out.method = request->getMethodString();
    out.path = request->path();
    auto query = api::parse_query_string(request->query());
    if (!query) {
        return std::nullopt;
    }
    out.query = std::move(*query);
    for (const auto& [name, value] : request->headers()) {
        out.headers.emplace_back(name, value);
    }
    out.body = std::string(request->body());
    return out;
}

drogon::HttpResponsePtr to_drogon_response(const api::HttpResponse& response) {
    auto r = drogon::HttpResponse::newHttpResponse();
    r->setStatusCode(static_cast<drogon::HttpStatusCode>(response.status));
    r->setContentTypeString(response.content_type);
    r->setBody(response.body);
    for (const auto& [name, value] : response.headers) {
        r->addHeader(name, value);
    }
    return r;
}

}  // namespace faulttwin::server

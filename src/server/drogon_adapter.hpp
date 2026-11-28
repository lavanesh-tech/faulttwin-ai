#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include "faulttwin/api/http.hpp"

namespace faulttwin::server {

/// Drogon request -> transport-independent request. Header names arrive lower-cased from
/// Drogon; the raw query string is parsed here (not Drogon's parameter map, which merges
/// repeated keys and form bodies). nullopt when the query string is malformed.
[[nodiscard]] std::optional<api::HttpRequest> to_api_request(const drogon::HttpRequestPtr& request);

/// Transport-independent response -> Drogon response.
[[nodiscard]] drogon::HttpResponsePtr to_drogon_response(const api::HttpResponse& response);

}  // namespace faulttwin::server

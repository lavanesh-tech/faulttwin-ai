#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace faulttwin::api {

/// A transport-independent HTTP request. The Drogon adapter fills it in; unit tests build it
/// directly, so every route, status code and validation rule is tested without a socket.
struct HttpRequest {
    std::string method;  ///< upper case, e.g. "GET"
    std::string path;    ///< decoded path without the query string
    std::vector<std::pair<std::string, std::string>> query;    ///< decoded, in order
    std::vector<std::pair<std::string, std::string>> headers;  ///< names in lower case
    std::string body;

    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;
    /// The single value of a query parameter; nullopt if absent. Repeated parameters are
    /// reported by `query_repeated`.
    [[nodiscard]] std::optional<std::string_view> param(std::string_view name) const;
    [[nodiscard]] bool query_repeated(std::string_view name) const;
};

struct HttpResponse {
    int status{200};
    std::string content_type{"application/json"};
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

/// Splits and percent-decodes an application/x-www-form-urlencoded query string ("a=1&b=x%20y",
/// '+' is a space). A key without '=' has an empty value. nullopt for a malformed escape or a
/// decoded NUL byte. Order and repeats are kept so that the API can reject repeated filters.
[[nodiscard]] std::optional<std::vector<std::pair<std::string, std::string>>> parse_query_string(
    std::string_view raw);

/// Request ids: client-supplied X-Request-Id is kept when it is 1-64 characters of
/// [A-Za-z0-9._-]; anything else is replaced by a generated id, so a log line can never be
/// forged through this header.
[[nodiscard]] bool valid_request_id(std::string_view id) noexcept;

/// Constant-time comparison for secrets: the time taken does not depend on where the first
/// differing byte is (it still depends on the lengths).
[[nodiscard]] bool constant_time_equals(std::string_view a, std::string_view b) noexcept;

}  // namespace faulttwin::api

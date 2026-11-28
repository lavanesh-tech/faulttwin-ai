#include "faulttwin/api/api_service.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <system_error>
#include <utility>
#include <variant>

#include "faulttwin/core/json.hpp"

namespace faulttwin::api {

namespace {

using core::json_number;
using core::json_quote;
using storage::StoreErrorKind;

std::string time_or_null(const std::optional<core::WallTime>& t) {
    return t ? json_quote(core::iso8601(*t)) : "null";
}

/// Parses a positive decimal id without sign, spaces or leading zeros.
std::optional<std::uint64_t> parse_id(std::string_view text) {
    if (text.empty() || text.size() > 19 || text.front() == '0') {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0) {
        return std::nullopt;
    }
    return value;
}

std::string_view method_label(std::string_view method) {
    static constexpr std::array<std::string_view, 7> kKnown{"GET",    "POST", "PUT",    "PATCH",
                                                            "DELETE", "HEAD", "OPTIONS"};
    for (const auto m : kKnown) {
        if (m == method) {
            return m;
        }
    }
    return "OTHER";
}

HttpResponse json_ok(std::string body, int status = 200) {
    HttpResponse r;
    r.status = status;
    r.body = std::move(body);
    return r;
}

HttpResponse method_not_allowed(std::string_view allow, std::string_view request_id) {
    auto r = error_response(405, "method_not_allowed", "use " + std::string(allow), request_id);
    r.headers.emplace_back("Allow", std::string(allow));
    return r;
}

std::string evidence_object(const std::vector<storage::Evidence>& evidence) {
    return storage::evidence_json(evidence);
}

/// Unknown or repeated parameters are errors, so a typo never silently widens a query.
std::string check_incident_query_keys(const HttpRequest& request) {
    static constexpr std::array<std::string_view, 8> kAllowed{"state", "node",  "rule",  "severity",
                                                              "since", "until", "limit", "cursor"};
    for (const auto& [key, value] : request.query) {
        if (std::ranges::find(kAllowed, key) == kAllowed.end()) {
            return "unknown query parameter '" + key + "'";
        }
        if (request.query_repeated(key)) {
            return "query parameter '" + key + "' is repeated";
        }
    }
    return {};
}

/// Parses and validates the query string of GET /api/v1/incidents.
std::variant<storage::IncidentQuery, std::string> parse_incident_query(const HttpRequest& request) {
    if (auto problem = check_incident_query_keys(request); !problem.empty()) {
        return problem;
    }
    storage::IncidentQuery query;
    if (const auto v = request.param("state")) {
        query.state = storage::incident_state_from_string(*v);
        if (!query.state) {
            return std::string("state must be open or resolved");
        }
    }
    if (const auto v = request.param("severity")) {
        query.severity = storage::severity_from_string(*v);
        if (!query.severity) {
            return std::string("severity must be warning or critical");
        }
    }
    if (const auto v = request.param("node")) {
        query.node = std::string(*v);
    }
    if (const auto v = request.param("rule")) {
        query.rule_id = std::string(*v);
    }
    for (const auto& [name, target] :
         std::array<std::pair<std::string_view, std::optional<storage::Timestamp>*>, 2>{
             {{"since", &query.opened_since}, {"until", &query.opened_until}}}) {
        if (const auto v = request.param(name)) {
            *target = core::parse_iso8601(*v);
            if (!*target) {
                return std::string(std::string(name) +
                                   " must be a UTC time like 2026-01-01T00:00:00Z");
            }
        }
    }
    if (const auto v = request.param("limit")) {
        const auto limit = parse_id(*v);
        if (!limit || *limit > storage::kMaxPageSize) {
            return std::string("limit must be an integer from 1 to " +
                               std::to_string(storage::kMaxPageSize));
        }
        query.limit = static_cast<std::size_t>(*limit);
    }
    if (const auto v = request.param("cursor")) {
        query.before_id = parse_id(*v);
        if (!query.before_id) {
            return std::string("cursor must be the next_cursor value of a previous page");
        }
    }
    if (auto problem = storage::validate_query(query); !problem.empty()) {
        return problem;
    }
    return query;
}

}  // namespace

HttpResponse error_response(int status, std::string_view code, std::string_view message,
                            std::string_view request_id) {
    HttpResponse r;
    r.status = status;
    r.body = R"({"error":{"code":)" + json_quote(code) + ",\"message\":" + json_quote(message) +
             ",\"request_id\":" + json_quote(request_id) + "}}";
    return r;
}

std::string incident_json(const storage::Incident& incident) {
    std::string out = "{\"id\":" + std::to_string(incident.id);
    out += ",\"rule_set_version\":" + json_quote(incident.rule_set_version);
    out += ",\"feature_schema_version\":" + json_quote(incident.feature_schema_version);
    out += ",\"rule_id\":" + json_quote(incident.rule_id);
    out += ",\"node\":" + json_quote(incident.node);
    out += ",\"severity\":" + json_quote(detection::to_string(incident.severity));
    out += ",\"state\":" + json_quote(storage::to_string(incident.state));
    out += ",\"resolution\":";
    out += incident.resolution ? json_quote(storage::to_string(*incident.resolution)) : "null";
    out += ",\"violation_since\":" + json_quote(core::iso8601(incident.violation_since));
    out += ",\"opened_at\":" + json_quote(core::iso8601(incident.opened_at));
    out += ",\"resolved_at\":" + time_or_null(incident.resolved_at);
    out += ",\"updated_at\":" + json_quote(core::iso8601(incident.updated_at));
    out += ",\"events\":[";
    for (std::size_t i = 0; i < incident.events.size(); ++i) {
        const auto& e = incident.events[i];
        out += i == 0 ? "" : ",";
        out += "{\"kind\":" + json_quote(detection::to_string(e.kind));
        out += ",\"severity\":" + json_quote(detection::to_string(e.severity));
        out += ",\"at\":" + json_quote(core::iso8601(e.at));
        out += ",\"evidence\":" + evidence_object(e.evidence) + "}";
    }
    out += "],\"acknowledgement\":";
    if (incident.acknowledgement) {
        const auto& a = *incident.acknowledgement;
        out += "{\"by\":" + json_quote(a.by) + ",\"note\":" + json_quote(a.note) +
               ",\"at\":" + json_quote(core::iso8601(a.at)) + "}";
    } else {
        out += "null";
    }
    out += '}';
    return out;
}

ApiService::ApiService(storage::IncidentStore& store, StatusBoard& board, core::Logger& logger,
                       observability::Registry& metrics, ApiConfig config, IdGenerator ids,
                       ClockFn clock)
    : store_(store),
      board_(board),
      logger_(logger),
      metrics_(metrics),
      config_(std::move(config)),
      ids_(std::move(ids)),
      clock_(std::move(clock)) {
    if (!ids_) {
        const auto start = core::to_unix_micros(core::WallClock::now());
        ids_ = [this, start] {
            return "req-" + std::to_string(start) + "-" +
                   std::to_string(sequence_.fetch_add(1, std::memory_order_relaxed) + 1);
        };
    }
}

HttpResponse ApiService::handle(const HttpRequest& request) {
    const auto started = std::chrono::steady_clock::now();
    const auto supplied = request.header("x-request-id");
    const std::string request_id =
        supplied && valid_request_id(*supplied) ? std::string(*supplied) : ids_();

    Routed routed;
    try {
        routed = route(request, request_id);
    } catch (const std::exception& e) {
        // A handler bug must not take the server down or leak internals to the client.
        logger_.error("api", "unhandled exception in handler",
                      {{"request_id", request_id}, {"detail", e.what()}});
        routed.route = "error";
        routed.response = error_response(500, "internal", "internal error", request_id);
    }
    HttpResponse& response = routed.response;
    response.headers.emplace_back("X-Request-Id", request_id);
    response.headers.emplace_back("Cache-Control", "no-store");
    response.headers.emplace_back("X-Content-Type-Options", "nosniff");

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const std::string method(method_label(request.method));
    metrics_
        .counter(
            "faulttwin_http_requests_total", "HTTP requests by route template, method and status",
            {"route", "method", "status"}, {routed.route, method, std::to_string(response.status)})
        .inc();
    metrics_
        .histogram("faulttwin_http_request_duration_seconds",
                   "Time spent handling a request inside the API service (excludes the network)",
                   observability::default_latency_buckets(), {"route"}, {routed.route})
        .observe(seconds);
    const bool probe =
        routed.route == "/healthz" || routed.route == "/readyz" || routed.route == "/metrics";
    logger_.log(probe ? core::LogLevel::kDebug : core::LogLevel::kInfo, "api", "request",
                {{"request_id", request_id},
                 {"method", method},
                 {"route", routed.route},
                 {"status", response.status},
                 {"duration_ms", seconds * 1000.0}});
    return response;
}

ApiService::Routed ApiService::route(const HttpRequest& request, const std::string& request_id) {
    const std::string_view path = request.path;
    const bool get = request.method == "GET";
    const auto only_get = [&](std::string route, auto&& fn) -> Routed {
        if (!get) {
            return {std::move(route), method_not_allowed("GET", request_id)};
        }
        return {std::move(route), fn()};
    };

    if (path == "/healthz") {
        return only_get("/healthz", [] { return json_ok(R"({"status":"ok"})"); });
    }
    if (path == "/readyz") {
        return only_get("/readyz", [this] { return readyz(); });
    }
    if (path == "/metrics") {
        return only_get("/metrics", [this] {
            HttpResponse r;
            r.content_type = "text/plain; version=0.0.4; charset=utf-8";
            r.body = metrics_.render();
            return r;
        });
    }
    if (path == "/api/v1/version") {
        return only_get("/api/v1/version", [this] { return version(); });
    }
    if (path == "/api/v1/nodes") {
        return only_get("/api/v1/nodes", [this] { return nodes(); });
    }
    if (path == "/api/v1/incidents") {
        return only_get("/api/v1/incidents", [&] { return list_incidents(request, request_id); });
    }
    constexpr std::string_view kIncidentPrefix = "/api/v1/incidents/";
    if (path.starts_with(kIncidentPrefix)) {
        std::string_view rest = path.substr(kIncidentPrefix.size());
        constexpr std::string_view kAck = "/acknowledge";
        const bool ack = rest.ends_with(kAck);
        if (ack) {
            rest.remove_suffix(kAck.size());
        }
        const std::string route_name =
            ack ? "/api/v1/incidents/{id}/acknowledge" : "/api/v1/incidents/{id}";
        const auto id = parse_id(rest);
        if (!id) {
            return {route_name, error_response(404, "not_found",
                                               "incident ids are positive integers", request_id)};
        }
        if (ack) {
            if (request.method != "POST") {
                return {route_name, method_not_allowed("POST", request_id)};
            }
            return {route_name, acknowledge(request, *id, request_id)};
        }
        return only_get(route_name, [&] { return get_incident(*id, request_id); });
    }
    if (path.starts_with("/api/") && !path.starts_with("/api/v1/")) {
        return {"unmatched", error_response(404, "unsupported_api_version",
                                            "this server implements /api/v1 only", request_id)};
    }
    return {"unmatched", error_response(404, "not_found", "no such route", request_id)};
}

HttpResponse ApiService::readyz() {
    const bool draining_now = draining();
    std::string store_detail = "reachable";
    const auto ping = store_.ping();
    const bool store_ok = std::holds_alternative<storage::Done>(ping);
    if (!store_ok) {
        store_detail = std::get<storage::StoreError>(ping).message;
    }
    const auto age = board_.age();
    bool monitor_ok = false;
    std::string monitor_detail = "no monitor cycle completed yet";
    if (age) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(*age);
        monitor_ok = ms <= config_.max_cycle_age;
        monitor_detail = "last cycle " + std::to_string(ms.count()) + " ms ago (limit " +
                         std::to_string(config_.max_cycle_age.count()) + " ms)";
    }
    const bool ready = store_ok && monitor_ok && !draining_now;
    std::string body = "{\"status\":";
    body += ready ? "\"ready\"" : "\"not_ready\"";
    body += R"(,"checks":{"store":{"ok":)";
    body += store_ok ? "true" : "false";
    body += ",\"backend\":" + json_quote(store_.backend());
    body += ",\"detail\":" + json_quote(store_detail) + R"(},"monitor":{"ok":)";
    body += monitor_ok ? "true" : "false";
    body += ",\"detail\":" + json_quote(monitor_detail) + "},\"draining\":";
    body += draining_now ? "true" : "false";
    body += "}}";
    return json_ok(std::move(body), ready ? 200 : 503);
}

HttpResponse ApiService::version() const {
    return json_ok("{\"api\":" + json_quote(kApiVersion) +
                   ",\"build\":" + json_quote(config_.build_version) +
                   ",\"rule_set_version\":" + json_quote(config_.rule_set_version) +
                   ",\"feature_schema_version\":" + json_quote(config_.feature_schema_version) +
                   ",\"store\":" + json_quote(store_.backend()) + "}");
}

HttpResponse ApiService::nodes() const {
    const auto status = board_.snapshot();
    std::string body = "{\"source\":" + json_quote(status.source);
    body += ",\"cycles\":" + std::to_string(status.cycles);
    body += ",\"last_cycle_at\":" + time_or_null(status.last_cycle_at);
    body += ",\"nodes\":[";
    for (std::size_t i = 0; i < status.nodes.size(); ++i) {
        const auto& n = status.nodes[i];
        body += i == 0 ? "" : ",";
        body += "{\"node\":" + json_quote(n.node) + ",\"role\":" + json_quote(n.role) +
                ",\"availability\":" + json_quote(n.availability) +
                ",\"observed_at\":" + json_quote(core::iso8601(n.observed_at)) +
                ",\"replication_lag_seconds\":" + json_number(n.replication_lag_seconds) +
                ",\"probe_latency_ms\":" + json_number(n.probe_latency_ms) + "}";
    }
    body += "],\"firing\":[";
    for (std::size_t i = 0; i < status.firing.size(); ++i) {
        body += i == 0 ? "" : ",";
        body += "{\"rule_id\":" + json_quote(status.firing[i].rule_id) +
                ",\"node\":" + json_quote(status.firing[i].node) + "}";
    }
    body += "]}";
    return json_ok(std::move(body));
}

HttpResponse ApiService::list_incidents(const HttpRequest& request, const std::string& request_id) {
    auto parsed = parse_incident_query(request);
    if (auto* problem = std::get_if<std::string>(&parsed)) {
        return error_response(400, "invalid_parameter", *problem, request_id);
    }
    const auto& query = std::get<storage::IncidentQuery>(parsed);
    auto result = store_.list(query);
    if (auto* error = std::get_if<storage::StoreError>(&result)) {
        return store_error(*error, request_id);
    }
    const auto& page = std::get<storage::IncidentPage>(result);
    std::string body = "{\"items\":[";
    for (std::size_t i = 0; i < page.items.size(); ++i) {
        body += i == 0 ? "" : ",";
        body += incident_json(page.items[i]);
    }
    body += "],\"count\":" + std::to_string(page.items.size()) + ",\"next_cursor\":";
    body += page.next_before_id ? json_quote(std::to_string(*page.next_before_id)) : "null";
    body += '}';
    return json_ok(std::move(body));
}

HttpResponse ApiService::get_incident(std::uint64_t id, const std::string& request_id) {
    auto result = store_.get(id);
    if (auto* error = std::get_if<storage::StoreError>(&result)) {
        return store_error(*error, request_id);
    }
    return json_ok(incident_json(std::get<storage::Incident>(result)));
}

HttpResponse ApiService::acknowledge(const HttpRequest& request, std::uint64_t id,
                                     const std::string& request_id) {
    if (config_.api_key.empty()) {
        return error_response(403, "writes_disabled",
                              "no API key is configured, so state-changing endpoints are off",
                              request_id);
    }
    const auto auth = request.header("authorization");
    constexpr std::string_view kBearer = "Bearer ";
    if (!auth || !auth->starts_with(kBearer) ||
        !constant_time_equals(auth->substr(kBearer.size()), config_.api_key)) {
        auto r =
            error_response(401, "unauthorized", "a valid Bearer token is required", request_id);
        r.headers.emplace_back("WWW-Authenticate", "Bearer");
        logger_.warn("api", "rejected unauthenticated write",
                     {{"request_id", request_id}, {"route", "/api/v1/incidents/{id}/acknowledge"}});
        return r;
    }
    const auto content_type = request.header("content-type");
    if (!content_type || !content_type->starts_with("application/json")) {
        return error_response(415, "unsupported_media_type", "send application/json", request_id);
    }
    if (request.body.size() > config_.max_body_bytes) {
        return error_response(413, "payload_too_large",
                              "body exceeds " + std::to_string(config_.max_body_bytes) + " bytes",
                              request_id);
    }
    core::JsonLimits limits;
    limits.max_bytes = config_.max_body_bytes;
    limits.max_depth = 4;
    auto parsed = core::parse_json(request.body, limits);
    if (const auto* error = std::get_if<core::JsonError>(&parsed)) {
        return error_response(400, "invalid_json",
                              "byte " + std::to_string(error->offset) + ": " + error->message,
                              request_id);
    }
    const auto& root = std::get<core::JsonValue>(parsed);
    const auto* object = root.as_object();
    if (object == nullptr) {
        return error_response(400, "invalid_body", "body must be a JSON object", request_id);
    }
    storage::Acknowledgement ack;
    bool have_by = false;
    for (const auto& [key, value] : *object) {
        const auto* text = value.as_string();
        if (key != "by" && key != "note") {
            return error_response(400, "invalid_body", "unknown field '" + key + "'", request_id);
        }
        if (text == nullptr) {
            return error_response(400, "invalid_body", "'" + key + "' must be a string",
                                  request_id);
        }
        if (key == "by") {
            ack.by = *text;
            have_by = true;
        } else {
            ack.note = *text;
        }
    }
    if (!have_by) {
        return error_response(400, "invalid_body", "'by' is required", request_id);
    }
    if (auto problem = storage::validate_acknowledgement(ack); !problem.empty()) {
        return error_response(400, "invalid_body", problem, request_id);
    }
    ack.at = clock_();
    auto result = store_.acknowledge(id, ack);
    if (auto* error = std::get_if<storage::StoreError>(&result)) {
        return store_error(*error, request_id);
    }
    logger_.info("api", "incident acknowledged",
                 {{"request_id", request_id}, {"incident_id", id}, {"by", ack.by}});
    return json_ok(incident_json(std::get<storage::Incident>(result)));
}

HttpResponse ApiService::store_error(const storage::StoreError& error,
                                     const std::string& request_id) {
    switch (error.kind) {
        case StoreErrorKind::kNotFound:
            return error_response(404, "not_found", error.message, request_id);
        case StoreErrorKind::kConflict:
            return error_response(409, "conflict", error.message, request_id);
        case StoreErrorKind::kInvalid:
            return error_response(400, "invalid_parameter", error.message, request_id);
        case StoreErrorKind::kUnavailable: {
            logger_.warn("api", "incident store unavailable",
                         {{"request_id", request_id}, {"detail", error.message}});
            auto r = error_response(503, "store_unavailable", "the incident store is unavailable",
                                    request_id);
            r.headers.emplace_back("Retry-After", "5");
            return r;
        }
        case StoreErrorKind::kCapacity:
        case StoreErrorKind::kInternal:
            break;
    }
    // Internal details go to the log, not to the client.
    logger_.error("api", "incident store error",
                  {{"request_id", request_id},
                   {"error", std::string(storage::to_string(error.kind))},
                   {"detail", error.message}});
    return error_response(500, "internal", "internal error", request_id);
}

}  // namespace faulttwin::api

#include "faulttwin/api/api_service.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../storage/flaky_store.hpp"
#include "faulttwin/core/json.hpp"

namespace faulttwin::api {
namespace {

using namespace std::chrono_literals;
using storage::Severity;

const core::WallTime kT0{std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}};

// JSON accessors that fail the test instead of dereferencing null (plain `*p` after an EXPECT
// also trips GCC's -Wnull-dereference at -O3).
const core::JsonValue& at(const core::JsonValue& v, std::string_view key) {
    static const core::JsonValue kMissing{};
    const auto* member = v.find(key);
    if (member == nullptr) {
        ADD_FAILURE() << "missing member " << key;
        return kMissing;
    }
    return *member;
}

const core::JsonValue& at(const core::JsonValue& v, std::size_t index) {
    static const core::JsonValue kMissing{};
    const auto* items = v.as_array();
    if (items == nullptr || index >= items->size()) {
        ADD_FAILURE() << "missing item " << index;
        return kMissing;
    }
    return (*items)[index];
}

std::string str(const core::JsonValue& v) {
    const auto* s = v.as_string();
    if (s == nullptr) {
        ADD_FAILURE() << "not a string";
        return {};
    }
    return *s;
}

double num(const core::JsonValue& v) {
    const auto* n = v.as_number();
    if (n == nullptr) {
        ADD_FAILURE() << "not a number";
        return 0.0;
    }
    return *n;
}

class ApiTest : public ::testing::Test {
  protected:
    ApiTest() { reset_service("secret-key"); }

    void reset_service(std::string key) {
        ApiConfig config;
        config.api_key = std::move(key);
        config.build_version = "0.5.0";
        config.rule_set_version = "faulttwin.rules.v1";
        config.feature_schema_version = "faulttwin.features.v1";
        config.max_cycle_age = 10s;
        service = std::make_unique<ApiService>(
            store, board, logger, metrics, config, [] { return std::string("generated-id"); },
            [] { return kT0 + 1h; });
    }

    HttpResponse get(std::string path,
                     std::vector<std::pair<std::string, std::string>> query = {}) {
        HttpRequest r;
        r.method = "GET";
        r.path = std::move(path);
        r.query = std::move(query);
        return service->handle(r);
    }

    HttpResponse ack(std::uint64_t id, std::string body, std::string auth = "Bearer secret-key",
                     std::string content_type = "application/json") {
        HttpRequest r;
        r.method = "POST";
        r.path = "/api/v1/incidents/" + std::to_string(id) + "/acknowledge";
        r.body = std::move(body);
        if (!auth.empty()) {
            r.headers.emplace_back("authorization", auth);
        }
        if (!content_type.empty()) {
            r.headers.emplace_back("content-type", content_type);
        }
        return service->handle(r);
    }

    std::uint64_t seed_incident(std::string node) {
        return std::get<std::uint64_t>(store.open({"faulttwin.rules.v1",
                                                   "faulttwin.features.v1",
                                                   "replication_lag_high",
                                                   std::move(node),
                                                   Severity::kWarning,
                                                   kT0 - 10s,
                                                   kT0,
                                                   {{"lag.last", 7.5}}}));
    }

    static core::JsonValue body_json(const HttpResponse& r) {
        auto parsed = core::parse_json(r.body);
        EXPECT_TRUE(std::holds_alternative<core::JsonValue>(parsed)) << r.body;
        return std::holds_alternative<core::JsonValue>(parsed) ? std::get<core::JsonValue>(parsed)
                                                               : core::JsonValue{};
    }
    static std::string error_code(const HttpResponse& r) {
        const auto v = body_json(r);
        const auto* e = v.find("error");
        return e != nullptr && e->find("code") != nullptr ? str(at(*e, "code")) : "";
    }
    static std::string header(const HttpResponse& r, std::string_view name) {
        for (const auto& [k, v] : r.headers) {
            if (k == name) {
                return v;
            }
        }
        return {};
    }

    std::vector<std::string> lines;
    core::Logger logger{[this](std::string_view l) { lines.emplace_back(l); },
                        core::LogLevel::kDebug};
    storage::testing::FlakyStore store;
    StatusBoard board;
    observability::Registry metrics;
    std::unique_ptr<ApiService> service;
};

TEST_F(ApiTest, HealthzIsAlwaysOk) {
    const auto r = get("/healthz");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, R"({"status":"ok"})");
    EXPECT_EQ(header(r, "X-Request-Id"), "generated-id");
    EXPECT_EQ(header(r, "Cache-Control"), "no-store");
}

TEST_F(ApiTest, ReadyzNeedsAMonitorCycleAndAStore) {
    auto r = get("/readyz");
    EXPECT_EQ(r.status, 503);
    EXPECT_NE(r.body.find("no monitor cycle completed yet"), std::string::npos);
    board.publish({});
    r = get("/readyz");
    EXPECT_EQ(r.status, 200) << r.body;
    store.available = false;
    r = get("/readyz");
    EXPECT_EQ(r.status, 503);
    EXPECT_NE(r.body.find("test: store is down"), std::string::npos);
    store.available = true;
    service->begin_shutdown();
    r = get("/readyz");
    EXPECT_EQ(r.status, 503);
    EXPECT_NE(r.body.find(R"("draining":true)"), std::string::npos);
}

TEST_F(ApiTest, MetricsUseTheTextFormatAndRouteTemplates) {
    (void)get("/api/v1/incidents/5");
    (void)get("/api/v1/incidents/6");
    const auto r = get("/metrics");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.content_type, "text/plain; version=0.0.4; charset=utf-8");
    EXPECT_NE(
        r.body.find(
            R"(faulttwin_http_requests_total{route="/api/v1/incidents/{id}",method="GET",status="404"} 2)"),
        std::string::npos)
        << r.body;
    EXPECT_EQ(r.body.find("incidents/5"), std::string::npos);
}

TEST_F(ApiTest, VersionReportsTheContracts) {
    const auto v = body_json(get("/api/v1/version"));
    EXPECT_EQ(str(at(v, "api")), "v1");
    EXPECT_EQ(str(at(v, "rule_set_version")), "faulttwin.rules.v1");
    EXPECT_EQ(str(at(v, "store")), "flaky");
}

TEST_F(ApiTest, NodesShowTheLatestPublishedStatus) {
    ClusterStatus s;
    s.cycles = 3;
    s.source = "synthetic";
    s.last_cycle_at = kT0;
    s.nodes.push_back({"pg-replica-1", "replica", "up", kT0, std::nullopt, 1.25});
    s.firing.push_back({"replication_lag_high", "pg-replica-1"});
    board.publish(s);
    const auto r = get("/api/v1/nodes");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body,
              R"({"source":"synthetic","cycles":3,"last_cycle_at":"2026-01-01T00:00:00.000Z",)"
              R"("nodes":[{"node":"pg-replica-1","role":"replica","availability":"up",)"
              R"("observed_at":"2026-01-01T00:00:00.000Z","replication_lag_seconds":null,)"
              R"("probe_latency_ms":1.25}],"firing":[{"rule_id":"replication_lag_high",)"
              R"("node":"pg-replica-1"}]})");
}

TEST_F(ApiTest, GetsOneIncident) {
    const auto id = seed_incident("pg-replica-1");
    const auto r = get("/api/v1/incidents/" + std::to_string(id));
    ASSERT_EQ(r.status, 200) << r.body;
    const auto v = body_json(r);
    EXPECT_EQ(str(at(v, "node")), "pg-replica-1");
    EXPECT_EQ(str(at(v, "state")), "open");
    EXPECT_TRUE(at(v, "resolution").is_null());
    EXPECT_EQ(str(at(v, "opened_at")), "2026-01-01T00:00:00.000Z");
    EXPECT_DOUBLE_EQ(num(at(at(at(at(v, "events"), 0), "evidence"), "lag.last")), 7.5);
}

TEST_F(ApiTest, IncidentIdsMustBeCanonicalPositiveIntegers) {
    for (const std::string bad : {"0", "-1", "01", "1x", "abc", "", "99999999999999999999"}) {
        const auto r = get("/api/v1/incidents/" + bad);
        EXPECT_EQ(r.status, 404) << bad;
    }
    EXPECT_EQ(get("/api/v1/incidents/77").status, 404);
}

TEST_F(ApiTest, ListsWithFiltersAndCursor) {
    for (int i = 0; i < 3; ++i) {
        (void)seed_incident("n" + std::to_string(i));
    }
    auto r = get("/api/v1/incidents", {{"limit", "2"}});
    ASSERT_EQ(r.status, 200) << r.body;
    auto v = body_json(r);
    EXPECT_EQ(num(at(v, "count")), 2.0);
    ASSERT_NE(at(v, "next_cursor").as_string(), nullptr);
    const std::string cursor = str(at(v, "next_cursor"));
    r = get("/api/v1/incidents", {{"limit", "2"}, {"cursor", cursor}});
    v = body_json(r);
    EXPECT_EQ(num(at(v, "count")), 1.0);
    EXPECT_TRUE(at(v, "next_cursor").is_null());
    r = get("/api/v1/incidents", {{"node", "n1"},
                                  {"state", "open"},
                                  {"severity", "warning"},
                                  {"since", "2025-12-31T00:00:00Z"},
                                  {"until", "2026-01-02T00:00:00Z"}});
    EXPECT_EQ(num(at(body_json(r), "count")), 1.0);
}

TEST_F(ApiTest, RejectsBadListParameters) {
    const std::vector<std::vector<std::pair<std::string, std::string>>> cases{
        {{"state", "closed"}},
        {{"severity", "high"}},
        {{"limit", "0"}},
        {{"limit", "501"}},
        {{"limit", "ten"}},
        {{"since", "2026-01-01"}},
        {{"since", "2026-01-02T00:00:00Z"}, {"until", "2026-01-01T00:00:00Z"}},
        {{"cursor", "-5"}},
        {{"limt", "5"}},
        {{"node", "a"}, {"node", "b"}},
        {{"node", std::string(200, 'x')}},
    };
    for (const auto& query : cases) {
        const auto r = get("/api/v1/incidents", query);
        EXPECT_EQ(r.status, 400) << query.front().first << "=" << query.front().second;
        EXPECT_EQ(error_code(r), "invalid_parameter");
    }
}

TEST_F(ApiTest, StoreOutageIs503WithRetryAfter) {
    store.available = false;
    const auto r = get("/api/v1/incidents");
    EXPECT_EQ(r.status, 503);
    EXPECT_EQ(error_code(r), "store_unavailable");
    EXPECT_EQ(header(r, "Retry-After"), "5");
    EXPECT_EQ(r.body.find("test: store is down"), std::string::npos);  // detail only in logs
}

TEST_F(ApiTest, AcknowledgeHappyPath) {
    const auto id = seed_incident("n");
    const auto r = ack(id, R"({"by":"alice","note":"on it"})");
    ASSERT_EQ(r.status, 200) << r.body;
    const auto v = body_json(r);
    EXPECT_EQ(str(at(at(v, "acknowledgement"), "by")), "alice");
    EXPECT_EQ(str(at(at(v, "acknowledgement"), "at")), "2026-01-01T01:00:00.000Z");
    EXPECT_EQ(ack(id, R"({"by":"bob"})").status, 409);
}

TEST_F(ApiTest, AcknowledgeRequiresTheBearerToken) {
    const auto id = seed_incident("n");
    auto r = ack(id, R"({"by":"alice"})", "");
    EXPECT_EQ(r.status, 401);
    EXPECT_EQ(header(r, "WWW-Authenticate"), "Bearer");
    EXPECT_EQ(ack(id, R"({"by":"alice"})", "Bearer wrong").status, 401);
    EXPECT_EQ(ack(id, R"({"by":"alice"})", "secret-key").status, 401);
    EXPECT_EQ(ack(id, R"({"by":"alice"})", "Bearer secret-key-and-more").status, 401);
    EXPECT_FALSE(std::get<storage::Incident>(store.get(id)).acknowledgement.has_value());
    for (const auto& l : lines) {
        EXPECT_EQ(l.find("secret-key"), std::string::npos) << l;
    }
}

TEST_F(ApiTest, WritesAreDisabledWithoutAConfiguredKey) {
    reset_service("");
    const auto id = seed_incident("n");
    const auto r = ack(id, R"({"by":"alice"})", "Bearer ");
    EXPECT_EQ(r.status, 403);
    EXPECT_EQ(error_code(r), "writes_disabled");
}

TEST_F(ApiTest, AcknowledgeValidatesTheBody) {
    const auto id = seed_incident("n");
    EXPECT_EQ(ack(id, R"({"by":"alice"})", "Bearer secret-key", "text/plain").status, 415);
    EXPECT_EQ(ack(id, std::string(20000, ' ')).status, 413);
    for (const std::string body :
         {"not json", "[]", R"({"note":"x"})", R"({"by":5})", R"({"by":"a","extra":1})",
          R"({"by":""})", R"({"by":"a","by":"b"})", R"({"by":"a\u0001"})"}) {
        const auto r = ack(id, body);
        EXPECT_EQ(r.status, 400) << body;
    }
    EXPECT_EQ(ack(424242, R"({"by":"a"})").status, 404);
}

TEST_F(ApiTest, MethodsAndUnknownRoutes) {
    HttpRequest post_health;
    post_health.method = "POST";
    post_health.path = "/healthz";
    auto r = service->handle(post_health);
    EXPECT_EQ(r.status, 405);
    EXPECT_EQ(header(r, "Allow"), "GET");
    const auto id = seed_incident("n");
    HttpRequest get_ack;
    get_ack.method = "GET";
    get_ack.path = "/api/v1/incidents/" + std::to_string(id) + "/acknowledge";
    EXPECT_EQ(service->handle(get_ack).status, 405);
    EXPECT_EQ(get("/nope").status, 404);
    r = get("/api/v2/incidents");
    EXPECT_EQ(r.status, 404);
    EXPECT_EQ(error_code(r), "unsupported_api_version");
}

TEST_F(ApiTest, KeepsAValidClientRequestIdAndReplacesAnInvalidOne) {
    HttpRequest r;
    r.method = "GET";
    r.path = "/healthz";
    r.headers.emplace_back("x-request-id", "abc-123");
    EXPECT_EQ(header(service->handle(r), "X-Request-Id"), "abc-123");
    r.headers.back().second = "bad id\n{\"level\":\"error\"}";
    EXPECT_EQ(header(service->handle(r), "X-Request-Id"), "generated-id");
}

TEST_F(ApiTest, AccessLogHasRequestIdRouteAndStatus) {
    (void)get("/api/v1/incidents");
    ASSERT_FALSE(lines.empty());
    const auto& last = lines.back();
    EXPECT_NE(last.find(R"("request_id":"generated-id")"), std::string::npos) << last;
    EXPECT_NE(last.find(R"("route":"/api/v1/incidents")"), std::string::npos) << last;
    EXPECT_NE(last.find(R"("status":200)"), std::string::npos) << last;
}

TEST(Http, ConstantTimeEquals) {
    EXPECT_TRUE(constant_time_equals("abc", "abc"));
    EXPECT_FALSE(constant_time_equals("abc", "abd"));
    EXPECT_FALSE(constant_time_equals("abc", "abcd"));
    EXPECT_FALSE(constant_time_equals("", "a"));
    EXPECT_TRUE(constant_time_equals("", ""));
}

}  // namespace
}  // namespace faulttwin::api

namespace faulttwin::api {
namespace {

TEST(Http, ParsesQueryStrings) {
    const auto q = parse_query_string("a=1&b=x%20y+z&&flag&a=2&e=%C3%A9");
    ASSERT_TRUE(q.has_value());
    ASSERT_EQ(q->size(), 5U);
    EXPECT_EQ((*q)[0], (std::pair<std::string, std::string>{"a", "1"}));
    EXPECT_EQ((*q)[1].second, "x y z");
    EXPECT_EQ((*q)[2], (std::pair<std::string, std::string>{"flag", ""}));
    EXPECT_EQ((*q)[3].second, "2");
    EXPECT_EQ((*q)[4].second, "\xc3\xa9");
    EXPECT_FALSE(parse_query_string("a=%2").has_value());
    EXPECT_FALSE(parse_query_string("a=%zz").has_value());
    EXPECT_FALSE(parse_query_string("a=%00").has_value());
    EXPECT_TRUE(parse_query_string("")->empty());
}

}  // namespace
}  // namespace faulttwin::api

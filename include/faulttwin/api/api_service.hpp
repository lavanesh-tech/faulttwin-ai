#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "faulttwin/api/http.hpp"
#include "faulttwin/api/status_board.hpp"
#include "faulttwin/core/logger.hpp"
#include "faulttwin/observability/metrics.hpp"
#include "faulttwin/storage/incident_store.hpp"

namespace faulttwin::api {

inline constexpr std::string_view kApiVersion = "v1";

struct ApiConfig {
    /// Bearer token for state-changing endpoints. Empty: those endpoints answer 403 (writes
    /// disabled), so a forgotten key never means "open to anyone".
    std::string api_key;
    std::size_t max_body_bytes{16UL * 1024UL};
    /// /readyz fails when no monitor cycle has completed for this long.
    std::chrono::milliseconds max_cycle_age{std::chrono::seconds{15}};
    std::string build_version{"0.0.0"};
    std::string rule_set_version;
    std::string feature_schema_version;
};

/// The HTTP API as a pure function of (request, state): routing, validation, auth, JSON
/// rendering, error format, request ids, access logs and request metrics. It owns no sockets
/// and no threads, and is safe to call from many threads at once (its own state is the
/// registry's atomics and an atomic counter; the store and board are thread-safe).
///
/// Routes (all JSON unless noted):
///   GET  /healthz                              liveness: the process answers
///   GET  /readyz                               readiness: store reachable, monitor cycling
///   GET  /metrics                              Prometheus text format 0.0.4
///   GET  /api/v1/version
///   GET  /api/v1/nodes                         latest node status and firing alerts
///   GET  /api/v1/incidents                     filters: state, node, rule, severity, since,
///                                              until, limit, cursor
///   GET  /api/v1/incidents/{id}
///   POST /api/v1/incidents/{id}/acknowledge    Bearer token; body {"by": "...", "note": "..."}
class ApiService {
  public:
    using IdGenerator = std::function<std::string()>;
    using ClockFn = std::function<core::WallTime()>;

    ApiService(
        storage::IncidentStore& store, StatusBoard& board, core::Logger& logger,
        observability::Registry& metrics, ApiConfig config, IdGenerator ids = {},
        ClockFn clock = [] { return core::WallClock::now(); });

    [[nodiscard]] HttpResponse handle(const HttpRequest& request);

    /// After this, /readyz answers 503 "draining" so a load balancer stops sending traffic
    /// while in-flight requests finish.
    void begin_shutdown() noexcept { draining_.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool draining() const noexcept {
        return draining_.load(std::memory_order_relaxed);
    }

  private:
    struct Routed {
        std::string route;  ///< template used as the metrics label, e.g. /api/v1/incidents/{id}
        HttpResponse response;
    };
    Routed route(const HttpRequest& request, const std::string& request_id);
    HttpResponse readyz();
    [[nodiscard]] HttpResponse version() const;
    [[nodiscard]] HttpResponse nodes() const;
    HttpResponse list_incidents(const HttpRequest& request, const std::string& request_id);
    HttpResponse get_incident(std::uint64_t id, const std::string& request_id);
    HttpResponse acknowledge(const HttpRequest& request, std::uint64_t id,
                             const std::string& request_id);
    HttpResponse store_error(const storage::StoreError& error, const std::string& request_id);

    storage::IncidentStore& store_;
    StatusBoard& board_;
    core::Logger& logger_;
    observability::Registry& metrics_;
    ApiConfig config_;
    IdGenerator ids_;
    ClockFn clock_;
    std::atomic<bool> draining_{false};
    std::atomic<std::uint64_t> sequence_{0};
};

/// A JSON error body: {"error":{"code":"...","message":"...","request_id":"..."}}.
[[nodiscard]] HttpResponse error_response(int status, std::string_view code,
                                          std::string_view message, std::string_view request_id);
/// An incident as JSON, the representation used by every incident endpoint.
[[nodiscard]] std::string incident_json(const storage::Incident& incident);

}  // namespace faulttwin::api

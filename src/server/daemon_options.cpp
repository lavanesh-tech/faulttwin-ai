#include "daemon_options.hpp"

#include <charconv>
#include <system_error>
#include <utility>

namespace faulttwin::server {

namespace {

std::optional<std::int64_t> parse_bounded(std::string_view text, std::int64_t min,
                                          std::int64_t max) {
    std::int64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < min ||
        value > max) {
        return std::nullopt;
    }
    return value;
}

bool is_loopback(std::string_view host) {
    return host == "127.0.0.1" || host == "::1" || host == "localhost";
}

std::optional<std::string> apply_monitor_env(DaemonOptions& o, const EnvLookup& env) {
    const auto nodes = env("FT_MONITOR_NODES");
    if (!nodes || nodes->empty()) {
        return "--source postgres needs FT_MONITOR_NODES (see .env.example)";
    }
    auto parsed = collectors::parse_endpoint_list(*nodes);
    if (const auto* error = std::get_if<collectors::EndpointParseError>(&parsed)) {
        return "FT_MONITOR_NODES: " + error->message;
    }
    o.nodes = std::get<std::vector<collectors::PgEndpoint>>(std::move(parsed));
    o.monitor_credentials.user = env("FT_MONITOR_USER").value_or("ft_monitor");
    o.monitor_credentials.dbname = env("FT_MONITOR_DBNAME").value_or("postgres");
    const auto password = env("FT_MONITOR_PASSWORD");
    if (!password || password->empty()) {
        return "--source postgres needs FT_MONITOR_PASSWORD (see .env.example)";
    }
    o.monitor_credentials.password = *password;
    return std::nullopt;
}

std::optional<std::string> apply_metadb_env(DaemonOptions& o, const EnvLookup& env) {
    o.metadb.endpoint.host = env("FT_METADB_HOST").value_or("127.0.0.1");
    if (const auto port = env("FT_METADB_PORT")) {
        const auto p = parse_bounded(*port, 1, 65535);
        if (!p) {
            return "FT_METADB_PORT must be 1-65535";
        }
        o.metadb.endpoint.port = static_cast<std::uint16_t>(*p);
    }
    o.metadb.credentials.user = env("FT_METADB_USER").value_or("ft_app");
    o.metadb.credentials.dbname = env("FT_METADB_DBNAME").value_or("faulttwin");
    const auto password = env("FT_METADB_PASSWORD");
    if (!password || password->empty()) {
        return "--store postgres needs FT_METADB_PASSWORD (see .env.example)";
    }
    o.metadb.credentials.password = *password;
    return std::nullopt;
}

std::optional<std::string> apply_listen(DaemonOptions& o, std::string_view value) {
    const auto colon = value.rfind(':');
    if (colon == std::string_view::npos || colon == 0) {
        return "--listen must be host:port, e.g. 127.0.0.1:8080";
    }
    const auto port = parse_bounded(value.substr(colon + 1), 0, 65535);
    if (!port) {
        return "--listen port must be 0-65535 (0 picks a free port)";
    }
    o.listen_host = std::string(value.substr(0, colon));
    o.listen_port = static_cast<std::uint16_t>(*port);
    return std::nullopt;
}

/// Applies one "--option value" pair; the error message if it is invalid or unknown.
std::optional<std::string> apply_option(DaemonOptions& o, std::string_view arg,
                                        std::string_view value) {
    if (arg == "--listen") {
        return apply_listen(o, value);
    }
    if (arg == "--source") {
        if (value != "postgres" && value != "synthetic") {
            return std::string{"--source must be postgres or synthetic"};
        }
        o.source = value == "postgres" ? SourceKind::kPostgres : SourceKind::kSynthetic;
        return std::nullopt;
    }
    if (arg == "--store") {
        if (value != "memory" && value != "postgres") {
            return std::string{"--store must be memory or postgres"};
        }
        o.store = value == "postgres" ? StoreKind::kPostgres : StoreKind::kMemory;
        return std::nullopt;
    }
    if (arg == "--interval-ms") {
        const auto n = parse_bounded(value, 1, 3'600'000);
        if (!n) {
            return std::string{"--interval-ms must be 1-3600000"};
        }
        o.interval = std::chrono::milliseconds{*n};
        return std::nullopt;
    }
    if (arg == "--http-threads") {
        const auto n = parse_bounded(value, 1, 64);
        if (!n) {
            return std::string{"--http-threads must be 1-64"};
        }
        o.http_threads = static_cast<std::size_t>(*n);
        return std::nullopt;
    }
    if (arg == "--synthetic-seed") {
        const auto n = parse_bounded(value, 1, 1'000'000'000);
        if (!n) {
            return std::string{"--synthetic-seed must be 1-1000000000"};
        }
        o.synthetic_seed = static_cast<std::uint64_t>(*n);
        return std::nullopt;
    }
    if (arg == "--log-level") {
        const auto level = core::log_level_from_string(value);
        if (!level) {
            return std::string{"--log-level must be debug, info, warn or error"};
        }
        o.log_level = *level;
        return std::nullopt;
    }
    return std::string{"unknown option: " + std::string(arg)};
}

}  // namespace

std::variant<DaemonOptions, HelpRequested, UsageError> parse_daemon_options(
    const std::vector<std::string_view>& args, const EnvLookup& env) {
    DaemonOptions o;
    bool allow_remote = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            return HelpRequested{};
        }
        if (arg == "--allow-remote") {
            allow_remote = true;
            continue;
        }
        if (i + 1 >= args.size()) {
            return UsageError{"unknown option or missing value: " + std::string(arg)};
        }
        const std::string_view value = args[++i];
        if (auto error = apply_option(o, arg, value)) {
            return UsageError{std::move(*error)};
        }
    }
    if (!is_loopback(o.listen_host) && !allow_remote) {
        return UsageError{"refusing to listen on " + o.listen_host +
                          " without --allow-remote: read endpoints are unauthenticated"};
    }
    o.api_key = env("FT_API_KEY").value_or("");
    if (!o.api_key.empty() && o.api_key.size() < 16) {
        return UsageError{"FT_API_KEY must be at least 16 characters (or unset to disable writes)"};
    }
    if (o.source == SourceKind::kPostgres) {
        if (auto error = apply_monitor_env(o, env)) {
            return UsageError{std::move(*error)};
        }
    }
    if (o.store == StoreKind::kPostgres) {
        if (auto error = apply_metadb_env(o, env)) {
            return UsageError{std::move(*error)};
        }
    }
    return o;
}

std::string daemon_usage(std::string_view program) {
    std::string out = "Usage: ";
    out += program;
    out += R"( [options]

FaultTwin daemon: monitors a cluster (or a synthetic scenario), runs the Phase 4 rules,
records incidents and serves the HTTP API.

Options:
  --listen HOST:PORT    address to serve on (default 127.0.0.1:8080; port 0 = any free port)
  --allow-remote        allow a non-loopback listen address
  --source KIND         postgres | synthetic (default synthetic)
  --store KIND          memory | postgres (default memory)
  --interval-ms N       monitor cycle interval (default 1000)
  --synthetic-seed N    scenario seed for --source synthetic (default 1)
  --http-threads N      HTTP worker threads (default 2)
  --log-level LEVEL     debug | info | warn | error (default info)
  -h, --help            show this help

Environment (secrets are read only from here):
  FT_API_KEY            Bearer token for POST endpoints; unset disables them (>= 16 chars)
  FT_MONITOR_NODES, FT_MONITOR_USER, FT_MONITOR_PASSWORD, FT_MONITOR_DBNAME
                        monitored cluster, for --source postgres
  FT_METADB_HOST, FT_METADB_PORT, FT_METADB_USER, FT_METADB_PASSWORD, FT_METADB_DBNAME
                        FaultTwin's own database, for --store postgres
)";
    return out;
}

}  // namespace faulttwin::server

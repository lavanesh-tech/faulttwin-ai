#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "faulttwin/collectors/endpoint.hpp"
#include "faulttwin/collectors/libpq_session.hpp"
#include "faulttwin/core/logger.hpp"

namespace faulttwin::server {

enum class SourceKind : std::uint8_t { kPostgres, kSynthetic };
enum class StoreKind : std::uint8_t { kMemory, kPostgres };

struct MetaDbOptions {
    collectors::PgEndpoint endpoint{"meta-db", "127.0.0.1", 55435};
    collectors::PgCredentials credentials{"ft_app", "", "faulttwin"};
};

struct DaemonOptions {
    std::string listen_host{"127.0.0.1"};
    std::uint16_t listen_port{8080};
    std::size_t http_threads{2};
    SourceKind source{SourceKind::kSynthetic};
    StoreKind store{StoreKind::kMemory};
    std::chrono::milliseconds interval{1000};
    core::LogLevel log_level{core::LogLevel::kInfo};
    std::string api_key;  ///< from FT_API_KEY only

    // --source postgres (same environment as faulttwin-monitor)
    std::vector<collectors::PgEndpoint> nodes;
    collectors::PgCredentials monitor_credentials;
    collectors::ConnectOptions connect;
    std::chrono::milliseconds max_age{5000};

    // --source synthetic (Phase 4 scenario generator, replayed one tick per interval)
    std::uint64_t synthetic_seed{1};

    // --store postgres
    MetaDbOptions metadb;
};

struct HelpRequested {};
struct UsageError {
    std::string message;
};

using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

/// Options come from the command line; every secret (FT_API_KEY, FT_MONITOR_PASSWORD,
/// FT_METADB_PASSWORD) comes only from the environment, so it never shows in the process list.
/// Listening on anything other than a loopback address requires --allow-remote, because the
/// read endpoints have no authentication in Phase 5.
[[nodiscard]] std::variant<DaemonOptions, HelpRequested, UsageError> parse_daemon_options(
    const std::vector<std::string_view>& args, const EnvLookup& env);

[[nodiscard]] std::string daemon_usage(std::string_view program);

}  // namespace faulttwin::server

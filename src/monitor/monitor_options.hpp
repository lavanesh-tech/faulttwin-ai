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

namespace faulttwin::monitor {

enum class OutputFormat : std::uint8_t { kText, kJson };

struct MonitorOptions {
    std::vector<collectors::PgEndpoint> nodes;
    collectors::PgCredentials credentials;
    collectors::ConnectOptions connect;
    collectors::CollectionPolicy policy;
    std::uint32_t cycles{1};
    std::chrono::milliseconds interval{1000};
    std::chrono::milliseconds max_age{5000};
    OutputFormat format{OutputFormat::kText};
    bool resources{true};
};

struct HelpRequested {};
struct UsageError {
    std::string message;
};

/// Looks up an environment variable; std::nullopt when unset.
using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

/// Parses command-line options; nodes and credentials come from the environment
/// (FT_MONITOR_NODES, FT_MONITOR_USER, FT_MONITOR_PASSWORD, FT_MONITOR_DBNAME) so that the
/// password never appears in the process list.
[[nodiscard]] std::variant<MonitorOptions, HelpRequested, UsageError> parse_monitor_options(
    const std::vector<std::string_view>& args, const EnvLookup& env);

[[nodiscard]] std::string monitor_usage(std::string_view program);

}  // namespace faulttwin::monitor

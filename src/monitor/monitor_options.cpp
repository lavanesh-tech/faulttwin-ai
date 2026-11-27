#include "monitor_options.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <system_error>

namespace faulttwin::monitor {
namespace {

template <typename Int>
std::optional<Int> parse_int(std::string_view text, Int min, Int max) {
    Int value{};
    const char* const last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), last, value);
    if (text.empty() || ec != std::errc{} || ptr != last || value < min || value > max) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::string monitor_usage(std::string_view program) {
    std::string text = "Usage: ";
    text += program;
    text += R"( [options]

Collects PostgreSQL and host telemetry from the nodes in FT_MONITOR_NODES using a read-only
monitoring role, and prints one report per cycle.

Environment:
  FT_MONITOR_NODES     name=host:port[,name=host:port...]   (required)
  FT_MONITOR_USER      monitoring role                       (default: ft_monitor)
  FT_MONITOR_PASSWORD  its password                          (required; never pass it as an option)
  FT_MONITOR_DBNAME    database to connect to                (default: postgres)

Options:
  --cycles N             collection cycles, 1-1000000             (default 1)
  --interval-ms MS       time between cycle starts, 0-3600000     (default 1000)
  --format text|json     text summary or one JSON object per line (default text)
  --max-age-ms MS        readings older than this are stale       (default 5000)
  --connect-timeout-s S  libpq connect timeout, 2-60              (default 2)
  --query-timeout-ms MS  client-side deadline per query, 100-60000 (default 2000)
  --no-resources         do not collect host CPU/memory/disk
  --detect               evaluate the anomaly rules (faulttwin.rules.v1) every cycle and
                         print detections (fired / escalated / resolved)
  -h, --help             show this help
)";
    return text;
}

namespace {

/// Applies one "--name value" option; returns an error message or nullopt.
std::optional<std::string> apply_option(MonitorOptions& options, std::string_view name,
                                        std::string_view value) {
    const auto bounded = [&](std::int64_t min, std::int64_t max) {
        return parse_int<std::int64_t>(value, min, max);
    };
    const auto range_error = [&](std::int64_t min, std::int64_t max) {
        return std::string(name) + " must be " + std::to_string(min) + "-" + std::to_string(max);
    };
    if (name == "--format") {
        if (value != "text" && value != "json") {
            return "--format must be text or json";
        }
        options.format = value == "json" ? OutputFormat::kJson : OutputFormat::kText;
        return std::nullopt;
    }
    struct IntOption {
        std::string_view name;
        std::int64_t min;
        std::int64_t max;
    };
    static constexpr std::array<IntOption, 5> kIntOptions{{{"--cycles", 1, 1'000'000},
                                                           {"--interval-ms", 0, 3'600'000},
                                                           {"--max-age-ms", 1, 3'600'000},
                                                           {"--connect-timeout-s", 2, 60},
                                                           {"--query-timeout-ms", 100, 60'000}}};
    const auto* spec = std::ranges::find(kIntOptions, name, &IntOption::name);
    if (spec == kIntOptions.end()) {
        return "unknown option: " + std::string(name);
    }
    const auto n = bounded(spec->min, spec->max);
    if (!n) {
        return range_error(spec->min, spec->max);
    }
    if (name == "--cycles") {
        options.cycles = static_cast<std::uint32_t>(*n);
    } else if (name == "--interval-ms") {
        options.interval = std::chrono::milliseconds{*n};
    } else if (name == "--max-age-ms") {
        options.max_age = std::chrono::milliseconds{*n};
    } else if (name == "--connect-timeout-s") {
        options.connect.connect_timeout = std::chrono::seconds{*n};
    } else {
        options.connect.query_timeout = std::chrono::milliseconds{*n};
        // Keep the server-side statement_timeout just inside the client deadline.
        options.connect.statement_timeout = std::chrono::milliseconds{*n * 3 / 4};
    }
    return std::nullopt;
}

std::optional<std::string> apply_environment(MonitorOptions& options, const EnvLookup& env) {
    const auto nodes = env("FT_MONITOR_NODES");
    if (!nodes || nodes->empty()) {
        return "FT_MONITOR_NODES is not set (see .env.example)";
    }
    auto parsed = collectors::parse_endpoint_list(*nodes);
    if (const auto* error = std::get_if<collectors::EndpointParseError>(&parsed)) {
        return "FT_MONITOR_NODES: " + error->message;
    }
    options.nodes = std::get<std::vector<collectors::PgEndpoint>>(std::move(parsed));
    options.credentials.user = env("FT_MONITOR_USER").value_or("ft_monitor");
    const auto password = env("FT_MONITOR_PASSWORD");
    if (!password || password->empty()) {
        return "FT_MONITOR_PASSWORD is not set (see .env.example)";
    }
    options.credentials.password = *password;
    options.credentials.dbname = env("FT_MONITOR_DBNAME").value_or("postgres");
    return std::nullopt;
}

}  // namespace

std::variant<MonitorOptions, HelpRequested, UsageError> parse_monitor_options(
    const std::vector<std::string_view>& args, const EnvLookup& env) {
    MonitorOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            return HelpRequested{};
        }
        if (arg == "--no-resources") {
            options.resources = false;
            continue;
        }
        if (arg == "--detect") {
            options.detect = true;
            continue;
        }
        if (i + 1 >= args.size()) {
            return UsageError{"unknown option or missing value: " + std::string(arg)};
        }
        if (auto error = apply_option(options, arg, args[++i])) {
            return UsageError{std::move(*error)};
        }
    }
    if (auto error = apply_environment(options, env)) {
        return UsageError{std::move(*error)};
    }
    return options;
}

}  // namespace faulttwin::monitor

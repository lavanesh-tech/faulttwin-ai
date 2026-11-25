#include "options.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace faulttwin::cli {
namespace {

template <typename Int>
std::optional<Int> parse_integer(std::string_view text) {
    Int value{};
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || text.empty()) {
        return std::nullopt;
    }
    return value;
}

// std::from_chars for double is not available in every standard library we
// target (older libc++), so strtod is used with strict end-pointer checks.
// The program never calls setlocale, so the "C" locale decimal point applies.
std::optional<double> parse_double(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const std::string buffer{text};
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(buffer.c_str(), &end);
    if (errno != 0 || end != buffer.c_str() + buffer.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

UsageError error(std::string message) {
    return UsageError{std::move(message)};
}

std::optional<std::uint32_t> parse_u32_in(std::string_view text, std::uint32_t lo,
                                          std::uint32_t hi) {
    const auto n = parse_integer<std::uint32_t>(text);
    return (n && *n >= lo && *n <= hi) ? n : std::nullopt;
}

std::optional<double> parse_fraction(std::string_view text) {
    const auto f = parse_double(text);
    return (f && *f >= 0.0 && *f <= 1.0) ? f : std::nullopt;
}

/// Applies one `--option value` pair. Returns false if the value is invalid.
/// Precondition: `name` is one of kValueOptions.
bool apply_value_option(std::string_view name, std::string_view value, Options& options) {
    auto& config = options.config;
    if (name == "--nodes") {
        const auto n = parse_u32_in(value, 1, kMaxNodes);
        config.node_count = n.value_or(0);
        return n.has_value();
    }
    if (name == "--samples-per-node") {
        const auto n = parse_u32_in(value, 1, kMaxSamplesPerNode);
        config.samples_per_node = n.value_or(0);
        return n.has_value();
    }
    if (name == "--seed") {
        const auto n = parse_integer<std::uint64_t>(value);
        config.seed = n.value_or(0);
        return n.has_value();
    }
    if (name == "--invalid-fraction") {
        const auto f = parse_fraction(value);
        config.invalid_fraction = f.value_or(0.0);
        return f.has_value();
    }
    if (name == "--lag-missing-fraction") {
        const auto f = parse_fraction(value);
        config.replica_lag_missing_fraction = f.value_or(0.0);
        return f.has_value();
    }
    if (name == "--start-unix-ms") {
        const auto ms = parse_integer<std::int64_t>(value);
        if (!ms || *ms <= 0) {
            return false;
        }
        config.start_time = telemetry::Timestamp{std::chrono::milliseconds{*ms}};
        return true;
    }
    if (name == "--interval-ms") {
        const auto ms = parse_integer<std::int64_t>(value);
        if (!ms || *ms < 1 || *ms > kMaxIntervalMs) {
            return false;
        }
        config.interval = std::chrono::milliseconds{*ms};
        return true;
    }
    // --format
    if (value == "text" || value == "jsonl") {
        options.format = value == "text" ? OutputFormat::kText : OutputFormat::kJsonLines;
        return true;
    }
    return false;
}

constexpr std::array<std::string_view, 8> kValueOptions{"--nodes",
                                                        "--samples-per-node",
                                                        "--seed",
                                                        "--invalid-fraction",
                                                        "--lag-missing-fraction",
                                                        "--start-unix-ms",
                                                        "--interval-ms",
                                                        "--format"};

bool is_value_option(std::string_view name) {
    return std::find(kValueOptions.begin(), kValueOptions.end(), name) != kValueOptions.end();
}

}  // namespace

ParseResult parse_options(std::span<const std::string_view> args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--help" || arg == "-h") {
            return HelpRequested{};
        }
        if (arg == "--quiet") {
            options.quiet = true;
            continue;
        }
        if (!is_value_option(arg)) {
            return error("unknown option '" + std::string{arg} + "'");
        }
        if (i + 1 >= args.size()) {
            return error("missing value for option '" + std::string{arg} + "'");
        }
        const std::string_view value = args[++i];
        if (!apply_value_option(arg, value, options)) {
            return error("invalid value '" + std::string{value} + "' for option '" +
                         std::string{arg} + "'");
        }
    }
    return options;
}

std::string usage_text(std::string_view program_name) {
    std::string text = "Usage: ";
    text += program_name;
    text += R"( [options]

Generates deterministic synthetic telemetry for a primary/replica cluster,
optionally injects known defects, validates every sample and checks that the
validator's verdict matches the injected ground truth.

Options:
  --nodes N                 Nodes in the cluster, 1..64 (default 3; node 0 is the primary)
  --samples-per-node N      Samples per node, 1..100000 (default 5)
  --seed N                  PRNG seed, unsigned 64-bit (default 42)
  --invalid-fraction F      Probability of injecting a defect, 0..1 (default 0)
  --lag-missing-fraction F  Probability a replica has no lag measurement, 0..1 (default 0.1)
  --start-unix-ms N         Timestamp of the first tick in Unix ms (default 2026-01-01T00:00:00Z)
  --interval-ms N           Tick interval in ms, 1..3600000 (default 1000)
  --format text|jsonl       Per-sample output format (default text)
  --quiet                   Print the summary only
  -h, --help                Show this help

Exit status: 0 = validator agreed with ground truth for every sample,
             1 = at least one mismatch, 2 = usage error.
)";
    return text;
}

}  // namespace faulttwin::cli

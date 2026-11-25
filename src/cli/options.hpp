#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "faulttwin/telemetry/synthetic.hpp"

namespace faulttwin::cli {

enum class OutputFormat : std::uint8_t { kText, kJsonLines };

// Upper bounds keep a mistyped argument from allocating unbounded memory.
inline constexpr std::uint32_t kMaxNodes = 64;
inline constexpr std::uint32_t kMaxSamplesPerNode = 100'000;
inline constexpr std::int64_t kMaxIntervalMs = 3'600'000;

struct Options {
    telemetry::SyntheticConfig config;
    OutputFormat format{OutputFormat::kText};
    bool quiet{false};  ///< summary only
};

struct HelpRequested {};

struct UsageError {
    std::string message;
};

using ParseResult = std::variant<Options, HelpRequested, UsageError>;

/// Parses argv[1..]. Never throws for bad input; reports a UsageError instead.
[[nodiscard]] ParseResult parse_options(std::span<const std::string_view> args);

[[nodiscard]] std::string usage_text(std::string_view program_name);

}  // namespace faulttwin::cli

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace faulttwin::detect {

struct SeedRange {
    std::uint64_t first{0};
    std::uint64_t last{0};  ///< inclusive
    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<std::size_t>(last - first + 1);
    }
};

/// `evaluate`: tune on validation seeds, evaluate once on the test seeds (and the shifted
/// test seeds, generated with lower fault intensity).
struct EvaluateOptions {
    SeedRange validation{1, 40};
    SeedRange test{1001, 1040};
    std::optional<SeedRange> shifted{SeedRange{2001, 2040}};
    std::uint32_t grace_seconds{30};
    std::string out_dir;  ///< empty: print the summary only
};

/// `scenario`: run the default rules on one scenario and print ground truth and detections.
struct ScenarioOptions {
    std::uint64_t seed{1};
    bool jsonl{false};
};

struct HelpRequested {};
struct UsageError {
    std::string message;
};

using DetectCommand = std::variant<EvaluateOptions, ScenarioOptions, HelpRequested, UsageError>;

[[nodiscard]] DetectCommand parse_detect_options(const std::vector<std::string_view>& args);
[[nodiscard]] std::optional<SeedRange> parse_seed_range(std::string_view text);
[[nodiscard]] std::string detect_usage(std::string_view program);

}  // namespace faulttwin::detect

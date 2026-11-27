#include "detect_options.hpp"

#include <charconv>
#include <system_error>
#include <utility>

namespace faulttwin::detect {

namespace {

template <typename T>
std::optional<T> parse_unsigned(std::string_view text) {
    T value{};
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end || text.empty()) {
        return std::nullopt;
    }
    return value;
}

UsageError usage_error(std::string message) {
    return UsageError{std::move(message)};
}

}  // namespace

std::optional<SeedRange> parse_seed_range(std::string_view text) {
    const auto dash = text.find('-');
    if (dash == std::string_view::npos) {
        const auto single = parse_unsigned<std::uint64_t>(text);
        if (!single) {
            return std::nullopt;
        }
        return SeedRange{*single, *single};
    }
    const auto first = parse_unsigned<std::uint64_t>(text.substr(0, dash));
    const auto last = parse_unsigned<std::uint64_t>(text.substr(dash + 1));
    if (!first || !last || *last < *first || *last - *first >= 100000) {
        return std::nullopt;
    }
    return SeedRange{*first, *last};
}

namespace {

bool overlaps(const SeedRange& a, const SeedRange& b) {
    return a.first <= b.last && b.first <= a.last;
}

/// Applies one "--name value" option of `evaluate`; returns an error message or nullopt.
std::optional<std::string> apply_evaluate_option(EvaluateOptions& options, std::string_view arg,
                                                 std::string_view value) {
    const auto bad_range = [&] { return "invalid seed range '" + std::string{value} + "'"; };
    if (arg == "--validation-seeds" || arg == "--test-seeds") {
        const auto range = parse_seed_range(value);
        if (!range) {
            return bad_range();
        }
        (arg == "--validation-seeds" ? options.validation : options.test) = *range;
        return std::nullopt;
    }
    if (arg == "--shifted-seeds") {
        if (value == "none") {
            options.shifted.reset();
            return std::nullopt;
        }
        const auto range = parse_seed_range(value);
        if (!range) {
            return bad_range();
        }
        options.shifted = *range;
        return std::nullopt;
    }
    if (arg == "--grace") {
        const auto grace = parse_unsigned<std::uint32_t>(value);
        if (!grace || *grace > 3600) {
            return std::string{"--grace must be 0..3600 seconds"};
        }
        options.grace_seconds = *grace;
        return std::nullopt;
    }
    if (arg == "--out") {
        options.out_dir = std::string{value};
        return std::nullopt;
    }
    return "unknown option " + std::string{arg};
}

DetectCommand parse_evaluate(const std::vector<std::string_view>& args) {
    EvaluateOptions options;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--help" || arg == "-h") {
            return HelpRequested{};
        }
        if (i + 1 >= args.size()) {
            return usage_error("missing value for " + std::string{arg});
        }
        if (auto error = apply_evaluate_option(options, arg, args[++i])) {
            return usage_error(std::move(*error));
        }
    }
    // Leakage guard: thresholds are chosen on validation scenarios only, so no seed may be
    // shared with a split that is evaluated.
    const bool shifted_overlaps =
        options.shifted && (overlaps(options.validation, *options.shifted) ||
                            overlaps(options.test, *options.shifted));
    if (overlaps(options.validation, options.test) || shifted_overlaps) {
        return usage_error("validation, test and shifted seed ranges must not overlap");
    }
    // The learned detectors train on the first half of the validation seeds and choose their
    // thresholds on the second half, so both halves must be non-empty.
    if (options.validation.size() < 2) {
        return usage_error("--validation-seeds needs at least 2 seeds");
    }
    return options;
}

DetectCommand parse_scenario(const std::vector<std::string_view>& args) {
    ScenarioOptions options;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--help" || arg == "-h") {
            return HelpRequested{};
        }
        if (i + 1 >= args.size()) {
            return usage_error("missing value for " + std::string{arg});
        }
        const std::string_view value = args[++i];
        if (arg == "--seed") {
            const auto seed = parse_unsigned<std::uint64_t>(value);
            if (!seed) {
                return usage_error("invalid seed '" + std::string{value} + "'");
            }
            options.seed = *seed;
        } else if (arg == "--format") {
            if (value != "text" && value != "jsonl") {
                return usage_error("--format must be text or jsonl");
            }
            options.jsonl = value == "jsonl";
        } else {
            return usage_error("unknown option " + std::string{arg});
        }
    }
    return options;
}

}  // namespace

DetectCommand parse_detect_options(const std::vector<std::string_view>& args) {
    if (args.size() < 2) {
        return usage_error("missing command (evaluate or scenario)");
    }
    const std::string_view command = args[1];
    if (command == "--help" || command == "-h" || command == "help") {
        return HelpRequested{};
    }
    if (command == "evaluate") {
        return parse_evaluate(args);
    }
    if (command == "scenario") {
        return parse_scenario(args);
    }
    return usage_error("unknown command '" + std::string{command} + "'");
}

std::string detect_usage(std::string_view program) {
    std::string out = "Usage: ";
    out += program;
    out += R"( evaluate [options]
       )";
    out += program;
    out += R"( scenario --seed N [--format text|jsonl]

Rule-based anomaly detection on deterministic synthetic scenarios (Phase 4).

evaluate: tunes every default rule's trigger and sustain on the validation scenarios only,
then evaluates the tuned rules and a naive-threshold baseline once on the held-out test
scenarios (and on shifted test scenarios with lower fault intensity). Two learned comparison
detectors, k-NN distance and an averaged perceptron, train on the first half of the
validation seeds, choose their alert thresholds on the second half, and are evaluated once on
the same test splits.
  --validation-seeds A-B   default 1-40 (at least 2 seeds)
  --test-seeds C-D         default 1001-1040
  --shifted-seeds E-F|none default 2001-2040 (intensity 0.5-0.8 instead of 0.8-1.2)
  --grace SECONDS          alert matching grace after an episode ends, default 30
  --out DIR                also write summary.json, summary.md, tuning-*.csv, alerts-*.csv

scenario: prints one scenario's ground truth and the detections of the default rules.

Values are synthetic and illustrative; they are not measurements of a real database.
)";
    return out;
}

}  // namespace faulttwin::detect

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>

#include "faulttwin/telemetry/synthetic.hpp"
#include "faulttwin/telemetry/validation.hpp"
#include "options.hpp"

namespace faulttwin::cli {

/// Counters for one CLI run. Ordered maps keep the printed summary stable.
struct RunSummary {
    std::uint64_t attempted{0};
    std::uint64_t valid{0};
    std::uint64_t invalid{0};
    std::uint64_t injected_defects{0};
    std::uint64_t ground_truth_mismatches{0};
    std::uint64_t replica_lag_measured{0};
    std::uint64_t replica_lag_not_measured{0};
    std::map<std::string, std::uint64_t> issues_by_type;  ///< "field:code" -> count
};

/// True when the validator's verdict for `sample` is exactly what the injected
/// ground truth predicts: valid with no defect, or a single expected issue.
[[nodiscard]] bool matches_ground_truth(const telemetry::SyntheticSample& sample,
                                        const telemetry::ValidationResult& result);

void accumulate(RunSummary& summary, const telemetry::SyntheticSample& sample,
                const telemetry::ValidationResult& result);

[[nodiscard]] std::string format_timestamp_utc(telemetry::Timestamp timestamp);

/// JSON string literal with escaping for quotes, backslashes and control bytes.
[[nodiscard]] std::string json_quote(std::string_view text);

void write_sample_line(std::ostream& out, OutputFormat format,
                       const telemetry::SyntheticSample& sample,
                       const telemetry::ValidationResult& result);

void write_summary(std::ostream& out, OutputFormat format, const Options& options,
                   const RunSummary& summary);

}  // namespace faulttwin::cli

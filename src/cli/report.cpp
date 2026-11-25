#include "report.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>

namespace faulttwin::cli {
namespace {

using telemetry::SyntheticSample;
using telemetry::ValidationResult;

std::string issue_key(const telemetry::ValidationIssue& issue) {
    std::string key{telemetry::to_string(issue.field)};
    key += ':';
    key += telemetry::to_string(issue.code);
    return key;
}

/// Fixed three-decimal rendering; non-finite values are rendered as quoted
/// strings so that JSON output stays valid.
std::string format_number(double value, bool json) {
    if (std::isnan(value)) {
        return json ? "\"nan\"" : "nan";
    }
    if (std::isinf(value)) {
        const char* text = value > 0 ? "inf" : "-inf";
        return json ? std::string{"\""} + text + "\"" : std::string{text};
    }
    std::array<char, 64> buffer{};
    const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                         std::chars_format::fixed, 3);
    if (ec != std::errc{}) {
        return json ? "null" : "?";
    }
    return std::string{buffer.data(), end};
}

/// Appends `value` as a zero-padded decimal of at least `width` digits.
void append_padded(std::string& out, long long value, int width) {
    std::array<char, 24> digits{};
    const auto [end, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    const auto length = ec == std::errc{} ? static_cast<int>(end - digits.data()) : 0;
    for (int i = length; i < width; ++i) {
        out += '0';
    }
    out.append(digits.data(), static_cast<std::size_t>(length));
}

std::string format_lag(const SyntheticSample& sample, const ValidationResult& result, bool json) {
    if (result.sample.has_value()) {
        const auto state = result.sample->replication_lag_state();
        if (state != telemetry::ReplicationLagState::kMeasured) {
            return json ? json_quote(telemetry::to_string(state))
                        : std::string{telemetry::to_string(state)};
        }
    }
    if (!sample.raw.replication_lag_ms.has_value()) {
        return json ? "null" : "absent";
    }
    return format_number(*sample.raw.replication_lag_ms, json);
}

}  // namespace

bool matches_ground_truth(const SyntheticSample& sample, const ValidationResult& result) {
    if (!sample.defect.has_value()) {
        return result.ok();
    }
    return !result.ok() && result.issues.size() == 1 &&
           result.issues.front() == telemetry::expected_issue(*sample.defect);
}

void accumulate(RunSummary& summary, const SyntheticSample& sample,
                const ValidationResult& result) {
    ++summary.attempted;
    if (result.sample.has_value()) {
        ++summary.valid;
        switch (result.sample->replication_lag_state()) {
            case telemetry::ReplicationLagState::kMeasured:
                ++summary.replica_lag_measured;
                break;
            case telemetry::ReplicationLagState::kNotMeasured:
                ++summary.replica_lag_not_measured;
                break;
            case telemetry::ReplicationLagState::kNotApplicable:
                break;
        }
    } else {
        ++summary.invalid;
    }
    if (sample.defect.has_value()) {
        ++summary.injected_defects;
    }
    if (!matches_ground_truth(sample, result)) {
        ++summary.ground_truth_mismatches;
    }
    for (const auto& issue : result.issues) {
        ++summary.issues_by_type[issue_key(issue)];
    }
}

std::string format_timestamp_utc(telemetry::Timestamp timestamp) {
    using namespace std::chrono;
    const auto ms = floor<milliseconds>(timestamp);
    const auto day = floor<days>(ms);
    const year_month_day ymd{day};
    const hh_mm_ss<milliseconds> tod{ms - day};

    std::string out;
    out.reserve(24);
    append_padded(out, static_cast<int>(ymd.year()), 4);
    out += '-';
    append_padded(out, static_cast<unsigned>(ymd.month()), 2);
    out += '-';
    append_padded(out, static_cast<unsigned>(ymd.day()), 2);
    out += 'T';
    append_padded(out, tod.hours().count(), 2);
    out += ':';
    append_padded(out, tod.minutes().count(), 2);
    out += ':';
    append_padded(out, tod.seconds().count(), 2);
    out += '.';
    append_padded(out, tod.subseconds().count(), 3);
    out += 'Z';
    return out;
}

std::string json_quote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20U) {
                    constexpr std::string_view kHex = "0123456789abcdef";
                    const auto byte = static_cast<unsigned char>(c);
                    out += "\\u00";
                    out += kHex[byte >> 4U];
                    out += kHex[byte & 0x0FU];
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

void write_sample_line(std::ostream& out, OutputFormat format, const SyntheticSample& sample,
                       const ValidationResult& result) {
    const auto& raw = sample.raw;
    const bool json = format == OutputFormat::kJsonLines;
    const std::string defect =
        sample.defect.has_value() ? std::string{telemetry::to_string(*sample.defect)} : "";

    if (json) {
        out << "{\"node_id\":" << json_quote(raw.node_id)
            << ",\"role\":" << json_quote(telemetry::to_string(raw.role))
            << ",\"collected_at\":" << json_quote(format_timestamp_utc(raw.collected_at))
            << ",\"cpu_usage_percent\":" << format_number(raw.cpu_usage_percent, true)
            << ",\"memory_usage_percent\":" << format_number(raw.memory_usage_percent, true)
            << ",\"disk_usage_percent\":" << format_number(raw.disk_usage_percent, true)
            << ",\"replication_lag_ms\":" << format_lag(sample, result, true)
            << ",\"valid\":" << (result.ok() ? "true" : "false") << ",\"issues\":[";
        for (std::size_t i = 0; i < result.issues.size(); ++i) {
            out << (i == 0 ? "" : ",") << json_quote(issue_key(result.issues[i]));
        }
        out << "],\"injected_defect\":" << (defect.empty() ? "null" : json_quote(defect))
            << ",\"matches_ground_truth\":"
            << (matches_ground_truth(sample, result) ? "true" : "false") << "}\n";
        return;
    }

    // Text output is for humans; node ids are printed through json_quote so that
    // control characters in untrusted input cannot corrupt the terminal.
    out << (result.ok() ? "VALID   " : "INVALID ") << json_quote(raw.node_id)
        << " role=" << telemetry::to_string(raw.role)
        << " at=" << format_timestamp_utc(raw.collected_at)
        << " cpu=" << format_number(raw.cpu_usage_percent, false)
        << " mem=" << format_number(raw.memory_usage_percent, false)
        << " disk=" << format_number(raw.disk_usage_percent, false)
        << " lag_ms=" << format_lag(sample, result, false);
    for (const auto& issue : result.issues) {
        out << " issue=" << issue_key(issue);
    }
    if (!defect.empty()) {
        out << " injected=" << defect;
    }
    if (!matches_ground_truth(sample, result)) {
        out << " GROUND_TRUTH_MISMATCH";
    }
    out << '\n';
}

void write_summary(std::ostream& out, OutputFormat format, const Options& options,
                   const RunSummary& summary) {
    const auto& c = options.config;
    if (format == OutputFormat::kJsonLines) {
        out << R"({"summary":{"seed":)" << c.seed << ",\"nodes\":" << c.node_count
            << ",\"samples_per_node\":" << c.samples_per_node
            << ",\"attempted\":" << summary.attempted << ",\"valid\":" << summary.valid
            << ",\"invalid\":" << summary.invalid
            << ",\"injected_defects\":" << summary.injected_defects
            << ",\"ground_truth_mismatches\":" << summary.ground_truth_mismatches
            << ",\"replica_lag_measured\":" << summary.replica_lag_measured
            << ",\"replica_lag_not_measured\":" << summary.replica_lag_not_measured
            << ",\"issues_by_type\":{";
        bool first = true;
        for (const auto& [key, count] : summary.issues_by_type) {
            out << (first ? "" : ",") << json_quote(key) << ':' << count;
            first = false;
        }
        out << "}}}\n";
        return;
    }

    out << "---- summary ----\n"
        << "seed                      " << c.seed << '\n'
        << "nodes x samples           " << c.node_count << " x " << c.samples_per_node << '\n'
        << "attempted                 " << summary.attempted << '\n'
        << "valid                     " << summary.valid << '\n'
        << "invalid                   " << summary.invalid << '\n'
        << "injected defects          " << summary.injected_defects << '\n'
        << "ground-truth mismatches   " << summary.ground_truth_mismatches << '\n'
        << "replica lag measured      " << summary.replica_lag_measured << '\n'
        << "replica lag not measured  " << summary.replica_lag_not_measured << '\n';
    for (const auto& [key, count] : summary.issues_by_type) {
        out << "issue " << key << "  " << count << '\n';
    }
}

}  // namespace faulttwin::cli

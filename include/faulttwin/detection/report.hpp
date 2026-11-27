#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "faulttwin/detection/rule_engine.hpp"

namespace faulttwin::detection {

/// Shortest round-trip-safe text with 15 significant digits; "null" for nullopt and for
/// non-finite values (JSON has no NaN).
[[nodiscard]] std::string json_number(std::optional<double> value);
/// JSON string literal with escaping.
[[nodiscard]] std::string json_quote(std::string_view text);
/// UTC, millisecond precision: 2026-01-01T00:00:05.000Z.
[[nodiscard]] std::string iso8601(Timestamp t);
/// One detection as a single-line JSON object with "type":"detection".
[[nodiscard]] std::string detection_json(const Detection& detection);
/// One detection as one line of text, for terminals.
[[nodiscard]] std::string detection_text(const Detection& detection);

}  // namespace faulttwin::detection

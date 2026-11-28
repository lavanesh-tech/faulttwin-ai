#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace faulttwin::core {

using WallClock = std::chrono::system_clock;
using WallTime = WallClock::time_point;

/// UTC with millisecond precision: 2026-01-01T00:00:05.000Z. Sub-millisecond digits are
/// truncated towards negative infinity.
[[nodiscard]] std::string iso8601(WallTime t);

/// Parses the form iso8601() writes, and also accepts it without fractional seconds or with
/// 1-9 fractional digits: YYYY-MM-DDTHH:MM:SS[.f]Z. Only "Z" (UTC) is accepted, so a time
/// zone is never guessed. Returns nullopt for anything else, including impossible dates.
[[nodiscard]] std::optional<WallTime> parse_iso8601(std::string_view text);

/// Microseconds since the Unix epoch; the precision PostgreSQL timestamptz keeps.
[[nodiscard]] std::int64_t to_unix_micros(WallTime t);
[[nodiscard]] WallTime from_unix_micros(std::int64_t micros);

}  // namespace faulttwin::core

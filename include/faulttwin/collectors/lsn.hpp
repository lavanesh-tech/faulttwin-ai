#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace faulttwin::collectors {

/// PostgreSQL write-ahead log position (pg_lsn): a 64-bit byte offset printed as two
/// hexadecimal halves, "XXXXXXXX/XXXXXXXX".
struct Lsn {
    std::uint64_t value{0};
    friend auto operator<=>(const Lsn&, const Lsn&) = default;
};

/// Parses the server's text form. Each half is 1-8 hex digits (either case); anything else,
/// including surrounding whitespace, is rejected.
[[nodiscard]] std::optional<Lsn> parse_lsn(std::string_view text) noexcept;

/// Formats like the server does (upper-case hex, no leading zeros).
[[nodiscard]] std::string to_string(Lsn lsn);

/// Bytes from `from` to `to`; nullopt when `to` is behind `from` (which, for lag, means the two
/// positions were not read consistently and the difference must not be reported as a value).
[[nodiscard]] std::optional<std::uint64_t> bytes_between(Lsn from, Lsn to) noexcept;

}  // namespace faulttwin::collectors

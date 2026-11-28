#include "faulttwin/core/time.hpp"

#include <charconv>
#include <cstdint>
#include <system_error>

namespace faulttwin::core {

namespace {

void append_padded(std::string& out, long long value, int width) {
    const std::string text = std::to_string(value);
    if (static_cast<int>(text.size()) < width) {
        out.append(static_cast<std::size_t>(width) - text.size(), '0');
    }
    out += text;
}

std::optional<int> fixed_digits(std::string_view text, std::size_t at, std::size_t count) {
    if (at + count > text.size()) {
        return std::nullopt;
    }
    int value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = text[at + i];
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

}  // namespace

std::string iso8601(WallTime t) {
    using std::chrono::milliseconds;
    const auto ms = std::chrono::floor<milliseconds>(t);
    const auto day = std::chrono::floor<std::chrono::days>(ms);
    const std::chrono::year_month_day ymd{day};
    const std::chrono::hh_mm_ss<milliseconds> time{ms - day};
    std::string out;
    out.reserve(24);
    append_padded(out, static_cast<int>(ymd.year()), 4);
    out += '-';
    append_padded(out, static_cast<unsigned>(ymd.month()), 2);
    out += '-';
    append_padded(out, static_cast<unsigned>(ymd.day()), 2);
    out += 'T';
    append_padded(out, time.hours().count(), 2);
    out += ':';
    append_padded(out, time.minutes().count(), 2);
    out += ':';
    append_padded(out, time.seconds().count(), 2);
    out += '.';
    append_padded(out, time.subseconds().count(), 3);
    out += 'Z';
    return out;
}

std::optional<WallTime> parse_iso8601(std::string_view text) {
    // YYYY-MM-DDTHH:MM:SS = 19 characters, then optional fraction, then 'Z'.
    if (text.size() < 20 || text.back() != 'Z' || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    const auto year = fixed_digits(text, 0, 4);
    const auto month = fixed_digits(text, 5, 2);
    const auto day = fixed_digits(text, 8, 2);
    const auto hour = fixed_digits(text, 11, 2);
    const auto minute = fixed_digits(text, 14, 2);
    const auto second = fixed_digits(text, 17, 2);
    if (!year || !month || !day || !hour || !minute || !second || *hour > 23 || *minute > 59 ||
        *second > 59) {
        return std::nullopt;
    }
    std::chrono::nanoseconds fraction{0};
    const std::string_view rest = text.substr(19, text.size() - 20);
    if (!rest.empty()) {
        if (rest.front() != '.' || rest.size() < 2 || rest.size() > 10) {
            return std::nullopt;
        }
        const auto digits = fixed_digits(rest, 1, rest.size() - 1);
        if (!digits) {
            return std::nullopt;
        }
        long long scale = 1;
        for (std::size_t i = rest.size() - 1; i < 9; ++i) {
            scale *= 10;
        }
        fraction = std::chrono::nanoseconds{static_cast<long long>(*digits) * scale};
    }
    const std::chrono::year_month_day ymd{std::chrono::year{*year},
                                          std::chrono::month{static_cast<unsigned>(*month)},
                                          std::chrono::day{static_cast<unsigned>(*day)}};
    if (!ymd.ok()) {
        return std::nullopt;
    }
    const auto since_epoch = std::chrono::sys_days{ymd}.time_since_epoch() +
                             std::chrono::hours{*hour} + std::chrono::minutes{*minute} +
                             std::chrono::seconds{*second} + fraction;
    return WallTime{std::chrono::duration_cast<WallClock::duration>(since_epoch)};
}

std::int64_t to_unix_micros(WallTime t) {
    return std::chrono::floor<std::chrono::microseconds>(t.time_since_epoch()).count();
}

WallTime from_unix_micros(std::int64_t micros) {
    return WallTime{
        std::chrono::duration_cast<WallClock::duration>(std::chrono::microseconds{micros})};
}

}  // namespace faulttwin::core

#include "faulttwin/core/logger.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iostream>

#include "faulttwin/core/json.hpp"

namespace faulttwin::core {

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::kDebug:
            return "debug";
        case LogLevel::kInfo:
            return "info";
        case LogLevel::kWarn:
            return "warn";
        case LogLevel::kError:
            return "error";
    }
    return "unknown";
}

std::optional<LogLevel> log_level_from_string(std::string_view text) noexcept {
    for (const auto level :
         {LogLevel::kDebug, LogLevel::kInfo, LogLevel::kWarn, LogLevel::kError}) {
        if (to_string(level) == text) {
            return level;
        }
    }
    return std::nullopt;
}

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

constexpr std::string_view kRedacted = "[REDACTED]";

}  // namespace

bool is_secret_key(std::string_view key) noexcept {
    static constexpr std::array<std::string_view, 8> kMarkers{
        "password", "passwd", "secret",        "token",
        "api_key",  "apikey", "authorization", "credential"};
    const std::string k = lower(key);
    return std::ranges::any_of(
        kMarkers, [&k](std::string_view marker) { return k.find(marker) != std::string::npos; });
}

namespace {

/// Index of the first character at or after i that is in stops (or text.size()).
std::size_t skip_until(std::string_view text, std::size_t i, std::string_view stops) {
    const auto at = text.find_first_of(stops, i);
    return at == std::string_view::npos ? text.size() : at;
}

/// The identifier (letters, digits, '_') that ends out, i.e. the key before an '='.
std::string trailing_identifier(const std::string& out) {
    std::size_t start = out.size();
    while (start > 0) {
        const auto c = static_cast<unsigned char>(out[start - 1]);
        if (std::isalnum(c) == 0 && c != '_') {
            break;
        }
        --start;
    }
    return out.substr(start);
}

/// End of the value that starts at i: a single-quoted value up to and including its closing
/// quote, otherwise up to the next separator.
std::size_t value_end(std::string_view text, std::size_t i) {
    if (i < text.size() && text[i] == '\'') {
        const auto close = skip_until(text, i + 1, "'");
        return close < text.size() ? close + 1 : close;
    }
    return skip_until(text, i, " &,;\"");
}

}  // namespace

std::string redact_text(std::string_view text) {
    constexpr std::string_view kBearer = "bearer ";
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        // "Bearer <token>": keep the scheme, hide the credentials.
        if (lower(text.substr(i, kBearer.size())) == kBearer) {
            out.append(text.substr(i, kBearer.size()));
            i = skip_until(text, i + kBearer.size(), " \",");
            out += kRedacted;
            continue;
        }
        // key=value, where key is the identifier immediately before '='.
        if (text[i] == '=') {
            const std::string key = trailing_identifier(out);
            out += '=';
            ++i;
            if (!key.empty() && is_secret_key(key)) {
                i = value_end(text, i);
                out += kRedacted;
            }
            continue;
        }
        out += text[i];
        ++i;
    }
    return out;
}

std::string format_log_record(WallTime at, LogLevel level, std::string_view component,
                              std::string_view message, const std::vector<LogField>& fields) {
    std::string line;
    line.reserve(128);
    line += "{\"ts\":";
    line += json_quote(iso8601(at));
    line += ",\"level\":";
    line += json_quote(to_string(level));
    line += ",\"component\":";
    line += json_quote(component);
    line += ",\"msg\":";
    line += json_quote(redact_text(message));
    for (const auto& field : fields) {
        const bool reserved = field.key == "ts" || field.key == "level" ||
                              field.key == "component" || field.key == "msg";
        line += ',';
        line += json_quote(reserved ? "field." + field.key : field.key);
        line += ':';
        if (is_secret_key(field.key)) {
            line += json_quote(kRedacted);
            continue;
        }
        std::visit(
            [&line](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    line += json_quote(redact_text(v));
                } else if constexpr (std::is_same_v<T, bool>) {
                    line += v ? "true" : "false";
                } else if constexpr (std::is_same_v<T, double>) {
                    line += json_number(v);
                } else {
                    line += std::to_string(v);
                }
            },
            field.value);
    }
    line += '}';
    return line;
}

Logger::Logger(Sink sink, LogLevel threshold, ClockFn clock)
    : sink_(std::move(sink)), threshold_(threshold), clock_(std::move(clock)) {}

Logger Logger::to_stderr(LogLevel threshold) {
    return Logger(
        [](std::string_view line) {
            std::cerr << line << '\n';
            std::cerr.flush();
        },
        threshold);
}

void Logger::log(LogLevel level, std::string_view component, std::string_view message,
                 const std::vector<LogField>& fields) {
    if (!enabled(level)) {
        return;
    }
    const std::string line = format_log_record(clock_(), level, component, message, fields);
    const std::scoped_lock lock(mutex_);
    sink_(line);
    ++written_;
}

std::uint64_t Logger::records_written() const {
    const std::scoped_lock lock(mutex_);
    return written_;
}

}  // namespace faulttwin::core

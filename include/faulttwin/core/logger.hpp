#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "faulttwin/core/time.hpp"

namespace faulttwin::core {

enum class LogLevel : std::uint8_t { kDebug, kInfo, kWarn, kError };
[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;
[[nodiscard]] std::optional<LogLevel> log_level_from_string(std::string_view text) noexcept;

/// One structured field. Values are typed so that numbers stay numbers in the JSON output.
struct LogField {
    using Value = std::variant<std::string, std::int64_t, std::uint64_t, double, bool>;
    std::string key;
    Value value;

    LogField(std::string k, std::string v) : key(std::move(k)), value(std::move(v)) {}
    LogField(std::string k, std::string_view v) : key(std::move(k)), value(std::string(v)) {}
    LogField(std::string k, const char* v) : key(std::move(k)), value(std::string(v)) {}
    LogField(std::string k, std::int64_t v) : key(std::move(k)), value(v) {}
    LogField(std::string k, int v) : key(std::move(k)), value(static_cast<std::int64_t>(v)) {}
    LogField(std::string k, std::uint64_t v) : key(std::move(k)), value(v) {}
    LogField(std::string k, std::uint32_t v)
        : key(std::move(k)), value(static_cast<std::uint64_t>(v)) {}
    LogField(std::string k, double v) : key(std::move(k)), value(v) {}
    LogField(std::string k, bool v) : key(std::move(k)), value(v) {}
};

/// True for field names that may carry a secret: they contain "password", "passwd",
/// "secret", "token", "api_key", "apikey", "authorization" or "credential" (case-insensitive).
[[nodiscard]] bool is_secret_key(std::string_view key) noexcept;

/// Replaces the value of key=value pairs whose key is secret (libpq conninfo style, e.g.
/// "password=abc") and the credentials of "Bearer <token>" with "[REDACTED]".
[[nodiscard]] std::string redact_text(std::string_view text);

/// Formats one record as a single JSON line without the trailing newline:
/// {"ts":"...","level":"info","component":"api","msg":"...",<fields>}.
/// Secret fields are written as "[REDACTED]", the message is passed through redact_text, and
/// a field that reuses a reserved name (ts, level, component, msg) is prefixed with "field.".
[[nodiscard]] std::string format_log_record(WallTime at, LogLevel level, std::string_view component,
                                            std::string_view message,
                                            const std::vector<LogField>& fields);

/// Thread-safe structured logger. Records below the threshold are discarded before they are
/// formatted. The sink receives one complete line per call, under a mutex, so lines from
/// different threads never interleave. No global instance: components receive a Logger&.
class Logger {
  public:
    using Sink = std::function<void(std::string_view line)>;
    using ClockFn = std::function<WallTime()>;

    explicit Logger(
        Sink sink, LogLevel threshold = LogLevel::kInfo,
        ClockFn clock = [] { return WallClock::now(); });

    /// A logger that writes to stderr (stdout is kept for program output).
    [[nodiscard]] static Logger to_stderr(LogLevel threshold = LogLevel::kInfo);

    void log(LogLevel level, std::string_view component, std::string_view message,
             const std::vector<LogField>& fields = {});
    void debug(std::string_view c, std::string_view m, const std::vector<LogField>& f = {}) {
        log(LogLevel::kDebug, c, m, f);
    }
    void info(std::string_view c, std::string_view m, const std::vector<LogField>& f = {}) {
        log(LogLevel::kInfo, c, m, f);
    }
    void warn(std::string_view c, std::string_view m, const std::vector<LogField>& f = {}) {
        log(LogLevel::kWarn, c, m, f);
    }
    void error(std::string_view c, std::string_view m, const std::vector<LogField>& f = {}) {
        log(LogLevel::kError, c, m, f);
    }

    [[nodiscard]] bool enabled(LogLevel level) const noexcept { return level >= threshold_; }
    [[nodiscard]] std::uint64_t records_written() const;

  private:
    Sink sink_;
    LogLevel threshold_;
    ClockFn clock_;
    mutable std::mutex mutex_;
    std::uint64_t written_{0};
};

}  // namespace faulttwin::core

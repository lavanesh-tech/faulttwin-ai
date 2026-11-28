#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace faulttwin::core {

// ---- Writing --------------------------------------------------------------------------

/// JSON string literal with escaping of quotes, backslashes and control characters.
[[nodiscard]] std::string json_quote(std::string_view text);
/// Shortest round-trip text with 15 significant digits; "null" for nullopt and non-finite
/// values (JSON has no NaN or infinity).
[[nodiscard]] std::string json_number(std::optional<double> value);

// ---- Reading --------------------------------------------------------------------------

class JsonValue;
using JsonArray = std::vector<JsonValue>;
/// Members in document order. Duplicate keys are rejected by the parser, so lookups are
/// unambiguous.
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;

/// A parsed JSON value. Numbers are doubles (RFC 8259 §6 interoperability range).
class JsonValue {
  public:
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject>;

    JsonValue() : value_(nullptr) {}
    explicit JsonValue(Storage value) : value_(std::move(value)) {}

    [[nodiscard]] bool is_null() const noexcept {
        return std::holds_alternative<std::nullptr_t>(value_);
    }
    [[nodiscard]] const bool* as_bool() const noexcept { return std::get_if<bool>(&value_); }
    [[nodiscard]] const double* as_number() const noexcept { return std::get_if<double>(&value_); }
    [[nodiscard]] const std::string* as_string() const noexcept {
        return std::get_if<std::string>(&value_);
    }
    [[nodiscard]] const JsonArray* as_array() const noexcept {
        return std::get_if<JsonArray>(&value_);
    }
    [[nodiscard]] const JsonObject* as_object() const noexcept {
        return std::get_if<JsonObject>(&value_);
    }
    /// Member of an object, or nullptr when this is not an object or the key is absent.
    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;

  private:
    Storage value_;
};

/// Limits that keep parsing of untrusted input bounded in time and memory.
struct JsonLimits {
    std::size_t max_bytes{64UL * 1024UL};
    std::size_t max_depth{32};
    std::size_t max_string_bytes{16UL * 1024UL};
    std::size_t max_elements{4096};  ///< total array elements + object members
};

struct JsonError {
    std::size_t offset{0};  ///< byte offset of the problem
    std::string message;
};

/// Strict RFC 8259 parser: one value, optional surrounding whitespace, no comments, no
/// trailing commas, no duplicate object keys, UTF-8 validated, \u escapes (including
/// surrogate pairs) decoded. Exceeding a limit is an error, never a truncation.
[[nodiscard]] std::variant<JsonValue, JsonError> parse_json(std::string_view text,
                                                            const JsonLimits& limits = {});

}  // namespace faulttwin::core

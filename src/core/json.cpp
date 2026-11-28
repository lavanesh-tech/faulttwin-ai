#include "faulttwin/core/json.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <system_error>

namespace faulttwin::core {

std::string json_quote(std::string_view text) {
    static constexpr std::string_view kHex = "0123456789abcdef";
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
                    const auto u = static_cast<unsigned char>(c);
                    out += "\\u00";
                    out += kHex.at(u >> 4U);
                    out += kHex.at(u & 0x0FU);
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

std::string json_number(std::optional<double> value) {
    if (!value || !std::isfinite(*value)) {
        return "null";
    }
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), *value,
                                      std::chars_format::general, 15);
    return result.ec == std::errc{} ? std::string(buffer.data(), result.ptr) : "null";
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    const auto* object = as_object();
    if (object == nullptr) {
        return nullptr;
    }
    for (const auto& [name, value] : *object) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

namespace {

/// Recursive-descent parser. Recursion depth is bounded by JsonLimits::max_depth, so stack use
/// is bounded too.
class Parser {
  public:
    Parser(std::string_view text, const JsonLimits& limits) : text_(text), limits_(limits) {}

    std::variant<JsonValue, JsonError> run() {
        if (text_.size() > limits_.max_bytes) {
            return JsonError{0, "document exceeds " + std::to_string(limits_.max_bytes) + " bytes"};
        }
        skip_whitespace();
        auto value = parse_value(0);
        if (error_) {
            return *error_;
        }
        skip_whitespace();
        if (pos_ != text_.size()) {
            return JsonError{pos_, "unexpected trailing characters"};
        }
        return value;
    }

  private:
    JsonValue fail(std::string message) {
        if (!error_) {
            error_ = JsonError{pos_, std::move(message)};
        }
        return {};
    }

    [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }
    [[nodiscard]] char peek() const noexcept { return at_end() ? '\0' : text_[pos_]; }

    void skip_whitespace() noexcept {
        while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
            ++pos_;
        }
    }

    bool count_element() {
        if (++elements_ > limits_.max_elements) {
            fail("more than " + std::to_string(limits_.max_elements) + " elements");
            return false;
        }
        return true;
    }

    // Recursion is bounded: every nested value increments depth, which is capped at
    // JsonLimits::max_depth before the next call.
    // NOLINTNEXTLINE(misc-no-recursion)
    JsonValue parse_value(std::size_t depth) {
        if (depth >= limits_.max_depth) {
            return fail("nesting deeper than " + std::to_string(limits_.max_depth));
        }
        switch (peek()) {
            case '{':
                return parse_object(depth);
            case '[':
                return parse_array(depth);
            case '"': {
                auto text = parse_string();
                return error_ ? JsonValue{} : JsonValue{JsonValue::Storage{std::move(text)}};
            }
            case 't':
                return parse_literal("true", JsonValue::Storage{true});
            case 'f':
                return parse_literal("false", JsonValue::Storage{false});
            case 'n':
                return parse_literal("null", JsonValue::Storage{nullptr});
            default:
                return parse_number();
        }
    }

    JsonValue parse_literal(std::string_view word, JsonValue::Storage value) {
        if (text_.substr(pos_, word.size()) != word) {
            return fail("invalid literal");
        }
        pos_ += word.size();
        return JsonValue{std::move(value)};
    }

    [[nodiscard]] bool peek_digit() const noexcept { return peek() >= '0' && peek() <= '9'; }

    /// Consumes one or more digits; false if there is none.
    bool skip_digits() noexcept {
        if (!peek_digit()) {
            return false;
        }
        while (peek_digit()) {
            ++pos_;
        }
        return true;
    }

    JsonValue parse_number() {
        const std::size_t start = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        if (peek() == '0') {
            ++pos_;
        } else if (!skip_digits()) {
            return fail("invalid value");
        }
        if (peek() == '.') {
            ++pos_;
            if (!skip_digits()) {
                return fail("digit expected after decimal point");
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') {
                ++pos_;
            }
            if (!skip_digits()) {
                return fail("digit expected in exponent");
            }
        }
        // The grammar is checked above, so strtod sees only a valid JSON number. The process
        // never calls setlocale, so the decimal point is always '.'.
        const std::string token(text_.substr(start, pos_ - start));
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (!std::isfinite(value)) {
            pos_ = start;
            return fail("number out of range");
        }
        return JsonValue{JsonValue::Storage{value}};
    }

    static int hex_digit(char c) noexcept {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    std::optional<std::uint32_t> parse_hex4() {
        if (pos_ + 4 > text_.size()) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const int digit = hex_digit(text_[pos_ + i]);
            if (digit < 0) {
                return std::nullopt;
            }
            value = (value << 4U) | static_cast<std::uint32_t>(digit);
        }
        pos_ += 4;
        return value;
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80U) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800U) {
            out += static_cast<char>(0xC0U | (cp >> 6U));
            out += static_cast<char>(0x80U | (cp & 0x3FU));
        } else if (cp < 0x10000U) {
            out += static_cast<char>(0xE0U | (cp >> 12U));
            out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
            out += static_cast<char>(0x80U | (cp & 0x3FU));
        } else {
            out += static_cast<char>(0xF0U | (cp >> 18U));
            out += static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
            out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
            out += static_cast<char>(0x80U | (cp & 0x3FU));
        }
    }

    /// Validates one UTF-8 sequence starting at pos_ and copies it; rejects overlong forms,
    /// surrogates and code points above U+10FFFF.
    bool copy_utf8(std::string& out) {
        const auto byte = [this](std::size_t i) {
            return static_cast<unsigned char>(text_[pos_ + i]);
        };
        const unsigned char lead = byte(0);
        std::size_t length = 0;
        std::uint32_t cp = 0;
        if (lead >= 0xC2U && lead <= 0xDFU) {
            length = 2;
            cp = lead & 0x1FU;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            length = 3;
            cp = lead & 0x0FU;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            length = 4;
            cp = lead & 0x07U;
        } else {
            return false;
        }
        if (pos_ + length > text_.size()) {
            return false;
        }
        for (std::size_t i = 1; i < length; ++i) {
            if ((byte(i) & 0xC0U) != 0x80U) {
                return false;
            }
            cp = (cp << 6U) | (byte(i) & 0x3FU);
        }
        const bool overlong = (length == 3 && cp < 0x800U) || (length == 4 && cp < 0x10000U);
        if (overlong || (cp >= 0xD800U && cp <= 0xDFFFU) || cp > 0x10FFFFU) {
            return false;
        }
        out.append(text_.substr(pos_, length));
        pos_ += length;
        return true;
    }

    /// Decodes \uXXXX (with a surrogate pair when needed) after the "\u"; false on error.
    bool parse_unicode_escape(std::string& out) {
        auto cp = parse_hex4();
        if (!cp) {
            fail("invalid \\u escape");
            return false;
        }
        if (*cp >= 0xDC00U && *cp <= 0xDFFFU) {
            fail("unpaired surrogate");
            return false;
        }
        if (*cp >= 0xD800U && *cp <= 0xDBFFU) {
            if (text_.substr(pos_, 2) != "\\u") {
                fail("unpaired surrogate");
                return false;
            }
            pos_ += 2;
            const auto low = parse_hex4();
            if (!low || *low < 0xDC00U || *low > 0xDFFFU) {
                fail("unpaired surrogate");
                return false;
            }
            *cp = 0x10000U + ((*cp - 0xD800U) << 10U) + (*low - 0xDC00U);
        }
        append_utf8(out, *cp);
        return true;
    }

    /// Decodes the escape after a backslash; false (with the error set) if it is invalid.
    bool parse_escape(std::string& out) {
        const char escape = peek();
        ++pos_;
        switch (escape) {
            case '"':
            case '\\':
            case '/':
                out += escape;
                return true;
            case 'b':
                out += '\b';
                return true;
            case 'f':
                out += '\f';
                return true;
            case 'n':
                out += '\n';
                return true;
            case 'r':
                out += '\r';
                return true;
            case 't':
                out += '\t';
                return true;
            case 'u':
                return parse_unicode_escape(out);
            default:
                fail("invalid escape");
                return false;
        }
    }

    std::string parse_string() {
        std::string out;
        ++pos_;  // opening quote
        while (true) {
            if (at_end()) {
                fail("unterminated string");
                return {};
            }
            if (out.size() > limits_.max_string_bytes) {
                fail("string longer than " + std::to_string(limits_.max_string_bytes) + " bytes");
                return {};
            }
            const char c = peek();
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20U) {
                fail("control character in string");
                return {};
            }
            if (static_cast<unsigned char>(c) >= 0x80U) {
                if (!copy_utf8(out)) {
                    fail("invalid UTF-8");
                    return {};
                }
                continue;
            }
            if (c != '\\') {
                out += c;
                ++pos_;
                continue;
            }
            ++pos_;
            if (!parse_escape(out)) {
                return {};
            }
        }
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by max_depth, see parse_value
    JsonValue parse_array(std::size_t depth) {
        ++pos_;
        JsonArray items;
        skip_whitespace();
        if (peek() == ']') {
            ++pos_;
            return JsonValue{JsonValue::Storage{std::move(items)}};
        }
        while (true) {
            skip_whitespace();
            if (!count_element()) {
                return {};
            }
            items.push_back(parse_value(depth + 1));
            if (error_) {
                return {};
            }
            skip_whitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return JsonValue{JsonValue::Storage{std::move(items)}};
            }
            return fail("',' or ']' expected");
        }
    }

    // NOLINTNEXTLINE(misc-no-recursion): bounded by max_depth, see parse_value
    JsonValue parse_object(std::size_t depth) {
        ++pos_;
        JsonObject members;
        skip_whitespace();
        if (peek() == '}') {
            ++pos_;
            return JsonValue{JsonValue::Storage{std::move(members)}};
        }
        while (true) {
            skip_whitespace();
            if (peek() != '"') {
                return fail("member name expected");
            }
            const std::size_t key_at = pos_;
            std::string key = parse_string();
            if (error_) {
                return {};
            }
            for (const auto& member : members) {
                if (member.first == key) {
                    pos_ = key_at;
                    return fail("duplicate key");
                }
            }
            skip_whitespace();
            if (peek() != ':') {
                return fail("':' expected");
            }
            ++pos_;
            skip_whitespace();
            if (!count_element()) {
                return {};
            }
            JsonValue value = parse_value(depth + 1);
            if (error_) {
                return {};
            }
            members.emplace_back(std::move(key), std::move(value));
            skip_whitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return JsonValue{JsonValue::Storage{std::move(members)}};
            }
            return fail("',' or '}' expected");
        }
    }

    std::string_view text_;
    JsonLimits limits_;
    std::size_t pos_{0};
    std::size_t elements_{0};
    std::optional<JsonError> error_;
};

}  // namespace

std::variant<JsonValue, JsonError> parse_json(std::string_view text, const JsonLimits& limits) {
    return Parser(text, limits).run();
}

}  // namespace faulttwin::core

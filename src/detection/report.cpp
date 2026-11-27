#include "faulttwin/detection/report.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <system_error>

namespace faulttwin::detection {

std::string json_number(std::optional<double> value) {
    if (!value || !std::isfinite(*value)) {
        return "null";
    }
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), *value,
                                      std::chars_format::general, 15);
    return result.ec == std::errc{} ? std::string(buffer.data(), result.ptr) : "null";
}

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

std::string iso8601(Timestamp t) {
    using std::chrono::milliseconds;
    const auto ms = std::chrono::time_point_cast<milliseconds>(t);
    const auto day = std::chrono::floor<std::chrono::days>(ms);
    const std::chrono::year_month_day ymd{day};
    const std::chrono::hh_mm_ss<milliseconds> time{ms - day};
    const auto digits = [](std::string& out, long long value, int width) {
        const std::string text = std::to_string(value);
        if (static_cast<int>(text.size()) < width) {
            out.append(static_cast<std::size_t>(width) - text.size(), '0');
        }
        out += text;
    };
    std::string out;
    digits(out, static_cast<int>(ymd.year()), 4);
    out += '-';
    digits(out, static_cast<unsigned>(ymd.month()), 2);
    out += '-';
    digits(out, static_cast<unsigned>(ymd.day()), 2);
    out += 'T';
    digits(out, time.hours().count(), 2);
    out += ':';
    digits(out, time.minutes().count(), 2);
    out += ':';
    digits(out, time.seconds().count(), 2);
    out += '.';
    digits(out, time.subseconds().count(), 3);
    out += 'Z';
    return out;
}

std::string detection_json(const Detection& d) {
    std::string out = R"({"type":"detection","kind":)";
    out += json_quote(to_string(d.kind));
    out += ",\"rule_set\":";
    out += json_quote(d.rule_set_version);
    out += ",\"rule\":";
    out += json_quote(d.rule_id);
    out += ",\"node\":";
    out += json_quote(d.node);
    out += ",\"severity\":";
    out += json_quote(to_string(d.severity));
    out += ",\"at\":";
    out += json_quote(iso8601(d.at));
    out += ",\"violation_since\":";
    out += json_quote(iso8601(d.violation_since));
    out += ",\"evidence\":{";
    bool first = true;
    for (const auto& e : d.evidence) {
        out += first ? "" : ",";
        first = false;
        out += json_quote(e.feature);
        out += ':';
        out += json_number(e.value);
    }
    out += "}}";
    return out;
}

std::string detection_text(const Detection& d) {
    std::string out = iso8601(d.at);
    out += ' ';
    out += to_string(d.kind);
    out += ' ';
    out += to_string(d.severity);
    out += ' ';
    out += d.rule_id;
    out += " node=";
    out += d.node;
    for (const auto& e : d.evidence) {
        out += ' ';
        out += e.feature;
        out += '=';
        out += json_number(e.value);
    }
    return out;
}

}  // namespace faulttwin::detection

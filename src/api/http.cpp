#include "faulttwin/api/http.hpp"

#include <algorithm>

namespace faulttwin::api {

std::optional<std::string_view> HttpRequest::header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
        if (key == name) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> HttpRequest::param(std::string_view name) const {
    for (const auto& [key, value] : query) {
        if (key == name) {
            return value;
        }
    }
    return std::nullopt;
}

bool HttpRequest::query_repeated(std::string_view name) const {
    std::size_t seen = 0;
    for (const auto& entry : query) {
        if (entry.first == name) {
            ++seen;
        }
    }
    return seen > 1;
}

namespace {

int hex_value(char c) noexcept {
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

std::optional<std::string> decode_component(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%') {
            if (i + 2 >= text.size()) {
                return std::nullopt;
            }
            const int hi = hex_value(text[i + 1]);
            const int lo = hex_value(text[i + 2]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
                return std::nullopt;
            }
            out += static_cast<char>((hi << 4) | lo);
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace

std::optional<std::vector<std::pair<std::string, std::string>>> parse_query_string(
    std::string_view raw) {
    std::vector<std::pair<std::string, std::string>> out;
    while (!raw.empty()) {
        const auto amp = raw.find('&');
        const std::string_view part = raw.substr(0, amp);
        raw = amp == std::string_view::npos ? std::string_view{} : raw.substr(amp + 1);
        if (part.empty()) {
            continue;
        }
        const auto eq = part.find('=');
        auto key = decode_component(part.substr(0, eq));
        auto value = eq == std::string_view::npos ? std::optional<std::string>{""}
                                                  : decode_component(part.substr(eq + 1));
        if (!key || !value) {
            return std::nullopt;
        }
        out.emplace_back(std::move(*key), std::move(*value));
    }
    return out;
}

bool valid_request_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    return std::ranges::all_of(id, [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-';
    });
}

bool constant_time_equals(std::string_view a, std::string_view b) noexcept {
    // Compare over the longer length so that the loop count does not reveal a prefix match.
    const std::size_t n = a.size() > b.size() ? a.size() : b.size();
    unsigned diff = a.size() == b.size() ? 0U : 1U;
    for (std::size_t i = 0; i < n; ++i) {
        const auto x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0U;
        const auto y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0U;
        diff |= x ^ y;
    }
    return diff == 0U;
}

}  // namespace faulttwin::api

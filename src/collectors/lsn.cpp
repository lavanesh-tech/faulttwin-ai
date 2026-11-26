#include "faulttwin/collectors/lsn.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <system_error>

namespace faulttwin::collectors {
namespace {

constexpr std::size_t kMaxHalfDigits = 8;

std::optional<std::uint32_t> parse_half(std::string_view text) noexcept {
    if (text.empty() || text.size() > kMaxHalfDigits) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const char* const last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), last, value, 16);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::optional<Lsn> parse_lsn(std::string_view text) noexcept {
    const auto slash = text.find('/');
    if (slash == std::string_view::npos) {
        return std::nullopt;
    }
    const auto high = parse_half(text.substr(0, slash));
    const auto low = parse_half(text.substr(slash + 1));
    if (!high || !low) {
        return std::nullopt;
    }
    return Lsn{(std::uint64_t{*high} << 32U) | std::uint64_t{*low}};
}

std::string to_string(Lsn lsn) {
    std::array<char, 2 * kMaxHalfDigits + 2> buffer{};
    char* const begin = buffer.data();
    char* const end = begin + buffer.size();
    auto result = std::to_chars(begin, end, static_cast<std::uint32_t>(lsn.value >> 32U), 16);
    *result.ptr = '/';
    result = std::to_chars(result.ptr + 1, end, static_cast<std::uint32_t>(lsn.value), 16);
    std::string text(begin, result.ptr);
    for (char& c : text) {
        if (c >= 'a' && c <= 'f') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return text;
}

std::optional<std::uint64_t> bytes_between(Lsn from, Lsn to) noexcept {
    if (to.value < from.value) {
        return std::nullopt;
    }
    return to.value - from.value;
}

}  // namespace faulttwin::collectors

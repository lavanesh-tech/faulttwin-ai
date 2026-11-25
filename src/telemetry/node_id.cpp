#include "faulttwin/telemetry/node_id.hpp"

namespace faulttwin::telemetry {
namespace {

// ASCII-only classification. std::isalnum is locale-dependent and has
// undefined behavior for negative char values, so it is deliberately avoided.
constexpr bool is_ascii_alnum(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

constexpr bool is_allowed_char(char c) noexcept {
    return is_ascii_alnum(c) || c == '.' || c == '_' || c == '-';
}

}  // namespace

std::string_view to_string(NodeIdError error) noexcept {
    switch (error) {
        case NodeIdError::kEmpty:
            return "empty";
        case NodeIdError::kTooLong:
            return "too_long";
        case NodeIdError::kInvalidFirstCharacter:
            return "invalid_first_character";
        case NodeIdError::kInvalidCharacter:
            return "invalid_character";
    }
    return "unknown_error";
}

std::optional<NodeIdError> check_node_id(std::string_view text) noexcept {
    if (text.empty()) {
        return NodeIdError::kEmpty;
    }
    if (text.size() > kMaxNodeIdLength) {
        return NodeIdError::kTooLong;
    }
    if (!is_ascii_alnum(text.front())) {
        return NodeIdError::kInvalidFirstCharacter;
    }
    for (const char c : text) {
        if (!is_allowed_char(c)) {
            return NodeIdError::kInvalidCharacter;
        }
    }
    return std::nullopt;
}

std::optional<NodeId> NodeId::parse(std::string_view text) {
    if (check_node_id(text).has_value()) {
        return std::nullopt;
    }
    return NodeId{std::string{text}};
}

}  // namespace faulttwin::telemetry

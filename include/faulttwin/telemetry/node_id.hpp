#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace faulttwin::telemetry {

/// Maximum length of a node identifier in bytes. Bounded so that identifiers can
/// be used as map keys, metric labels and log fields without unbounded growth.
inline constexpr std::size_t kMaxNodeIdLength = 64;

/// Reasons a node identifier can be rejected. Checks run in the order declared
/// here and the first failure is reported, so the result is deterministic.
enum class NodeIdError : std::uint8_t {
    kEmpty,
    kTooLong,
    kInvalidFirstCharacter,  ///< must start with an ASCII letter or digit
    kInvalidCharacter,       ///< allowed: ASCII letters, digits, '.', '_', '-'
};

[[nodiscard]] std::string_view to_string(NodeIdError error) noexcept;

/// Pure check used by both NodeId::parse and sample validation.
[[nodiscard]] std::optional<NodeIdError> check_node_id(std::string_view text) noexcept;

/// A node identifier that is valid by construction: the only way to obtain one
/// is NodeId::parse, so code holding a NodeId never needs to re-validate it.
class NodeId {
  public:
    [[nodiscard]] static std::optional<NodeId> parse(std::string_view text);

    [[nodiscard]] const std::string& value() const noexcept { return value_; }

    friend bool operator==(const NodeId&, const NodeId&) = default;

  private:
    explicit NodeId(std::string value) : value_(std::move(value)) {}

    std::string value_;
};

}  // namespace faulttwin::telemetry

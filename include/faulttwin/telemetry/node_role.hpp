#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace faulttwin::telemetry {

/// Role of a database node as reported by its collector.
///
/// kUnknown is a legitimate state (for example, the collector could not yet
/// determine whether the node is in recovery). It is different from an *invalid*
/// role, which is any underlying value outside the enumerators below, e.g. a
/// value produced by casting corrupted input.
enum class NodeRole : std::uint8_t {
    kUnknown = 0,
    kPrimary = 1,
    kReplica = 2,
};

[[nodiscard]] constexpr bool is_valid(NodeRole role) noexcept {
    switch (role) {
        case NodeRole::kUnknown:
        case NodeRole::kPrimary:
        case NodeRole::kReplica:
            return true;
    }
    return false;
}

/// Returns "unknown", "primary", "replica", or "invalid" for out-of-range values.
[[nodiscard]] std::string_view to_string(NodeRole role) noexcept;

/// Parses the exact lowercase names returned by to_string. "invalid" is rejected.
[[nodiscard]] std::optional<NodeRole> parse_node_role(std::string_view text) noexcept;

}  // namespace faulttwin::telemetry

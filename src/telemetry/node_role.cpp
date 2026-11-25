#include "faulttwin/telemetry/node_role.hpp"

namespace faulttwin::telemetry {

std::string_view to_string(NodeRole role) noexcept {
    switch (role) {
        case NodeRole::kUnknown:
            return "unknown";
        case NodeRole::kPrimary:
            return "primary";
        case NodeRole::kReplica:
            return "replica";
    }
    return "invalid";
}

std::optional<NodeRole> parse_node_role(std::string_view text) noexcept {
    if (text == "unknown") {
        return NodeRole::kUnknown;
    }
    if (text == "primary") {
        return NodeRole::kPrimary;
    }
    if (text == "replica") {
        return NodeRole::kReplica;
    }
    return std::nullopt;
}

}  // namespace faulttwin::telemetry

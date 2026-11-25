#include "faulttwin/telemetry/node_role.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace faulttwin::telemetry {
namespace {

TEST(NodeRoleTest, DeclaredEnumeratorsAreValid) {
    EXPECT_TRUE(is_valid(NodeRole::kUnknown));
    EXPECT_TRUE(is_valid(NodeRole::kPrimary));
    EXPECT_TRUE(is_valid(NodeRole::kReplica));
}

TEST(NodeRoleTest, OutOfRangeUnderlyingValuesAreInvalid) {
    for (const std::uint8_t raw : {std::uint8_t{3}, std::uint8_t{42}, std::uint8_t{255}}) {
        const auto role = static_cast<NodeRole>(raw);
        EXPECT_FALSE(is_valid(role)) << static_cast<int>(raw);
        EXPECT_EQ(to_string(role), "invalid");
    }
}

TEST(NodeRoleTest, StringRoundTrip) {
    for (const NodeRole role : {NodeRole::kUnknown, NodeRole::kPrimary, NodeRole::kReplica}) {
        EXPECT_EQ(parse_node_role(to_string(role)), role);
    }
}

TEST(NodeRoleTest, ParseIsExactAndCaseSensitive) {
    EXPECT_EQ(parse_node_role("Primary"), std::nullopt);
    EXPECT_EQ(parse_node_role("primary "), std::nullopt);
    EXPECT_EQ(parse_node_role("invalid"), std::nullopt);
    EXPECT_EQ(parse_node_role(""), std::nullopt);
}

}  // namespace
}  // namespace faulttwin::telemetry

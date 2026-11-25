#include "faulttwin/telemetry/node_id.hpp"

#include <gtest/gtest.h>

#include <string>

namespace faulttwin::telemetry {
namespace {

TEST(NodeIdTest, AcceptsTypicalIdentifiers) {
    for (const char* text : {"pg-primary", "pg-replica-1", "a", "node_7.eu-west-1", "9node"}) {
        EXPECT_EQ(check_node_id(text), std::nullopt) << text;
        const auto id = NodeId::parse(text);
        ASSERT_TRUE(id.has_value()) << text;
        EXPECT_EQ(id->value(), text);
    }
}

TEST(NodeIdTest, RejectsEmpty) {
    EXPECT_EQ(check_node_id(""), NodeIdError::kEmpty);
    EXPECT_FALSE(NodeId::parse("").has_value());
}

TEST(NodeIdTest, LengthBoundaryIsInclusive) {
    const std::string at_limit(kMaxNodeIdLength, 'n');
    const std::string over_limit(kMaxNodeIdLength + 1, 'n');
    EXPECT_EQ(check_node_id(at_limit), std::nullopt);
    EXPECT_EQ(check_node_id(over_limit), NodeIdError::kTooLong);
}

TEST(NodeIdTest, TooLongIsReportedBeforeCharacterErrors) {
    // Deterministic check order: a long string of invalid characters is "too long".
    const std::string text(kMaxNodeIdLength + 10, '/');
    EXPECT_EQ(check_node_id(text), NodeIdError::kTooLong);
}

TEST(NodeIdTest, RejectsNonAlphanumericFirstCharacter) {
    for (const char* text : {"-node", ".node", "_node", " node"}) {
        EXPECT_EQ(check_node_id(text), NodeIdError::kInvalidFirstCharacter) << text;
    }
}

TEST(NodeIdTest, RejectsDisallowedCharacters) {
    for (const char* text : {"node/1", "node 1", "node\n", "node;drop", "nöde"}) {
        EXPECT_EQ(check_node_id(text), NodeIdError::kInvalidCharacter) << text;
    }
}

TEST(NodeIdTest, RejectsEmbeddedNul) {
    const std::string text{"node\0x", 6};
    EXPECT_EQ(check_node_id(text), NodeIdError::kInvalidCharacter);
}

}  // namespace
}  // namespace faulttwin::telemetry

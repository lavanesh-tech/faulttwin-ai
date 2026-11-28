#include "faulttwin/core/json.hpp"

#include <gtest/gtest.h>

#include <string>

namespace faulttwin::core {
namespace {

JsonValue parse_ok(std::string_view text) {
    auto result = parse_json(text);
    EXPECT_TRUE(std::holds_alternative<JsonValue>(result)) << text;
    return std::holds_alternative<JsonValue>(result) ? std::get<JsonValue>(result) : JsonValue{};
}

/// Dereferences a pointer the test expects to be non-null, failing (not crashing) otherwise.
/// Plain `*p` after an EXPECT trips GCC's -Wnull-dereference at -O3.
template <typename T>
const T& deref(const T* p) {
    if (p == nullptr) {
        ADD_FAILURE() << "unexpected null";
        static const T empty{};
        return empty;
    }
    return *p;
}

std::string parse_error(std::string_view text, const JsonLimits& limits = {}) {
    auto result = parse_json(text, limits);
    if (const auto* error = std::get_if<JsonError>(&result)) {
        return error->message;
    }
    return "<parsed>";
}

TEST(JsonParse, ParsesEveryValueType) {
    const auto v = parse_ok(R"( {"a":1.5,"b":[true,false,null],"c":"x","d":{},"e":-0,"f":2e3} )");
    ASSERT_NE(v.as_object(), nullptr);
    EXPECT_DOUBLE_EQ(deref(deref(v.find("a")).as_number()), 1.5);
    const auto& b = deref(deref(v.find("b")).as_array());
    ASSERT_EQ(b.size(), 3U);
    EXPECT_TRUE(deref(b[0].as_bool()));
    EXPECT_TRUE(b[2].is_null());
    EXPECT_EQ(deref(deref(v.find("c")).as_string()), "x");
    EXPECT_TRUE(deref(deref(v.find("d")).as_object()).empty());
    EXPECT_DOUBLE_EQ(deref(deref(v.find("f")).as_number()), 2000.0);
    EXPECT_EQ(v.find("missing"), nullptr);
}

TEST(JsonParse, KeepsMemberOrder) {
    const auto v = parse_ok(R"({"z":1,"a":2,"m":3})");
    const auto& members = deref(v.as_object());
    ASSERT_EQ(members.size(), 3U);
    EXPECT_EQ(members[0].first, "z");
    EXPECT_EQ(members[1].first, "a");
    EXPECT_EQ(members[2].first, "m");
}

TEST(JsonParse, DecodesEscapesAndSurrogatePairs) {
    const auto v = parse_ok(R"(["\"\\\/\b\f\n\r\t", "é", "😀"])");
    const auto& items = deref(v.as_array());
    ASSERT_EQ(items.size(), 3U);
    EXPECT_EQ(deref(items[0].as_string()), "\"\\/\b\f\n\r\t");
    EXPECT_EQ(deref(items[1].as_string()), "\xc3\xa9");
    EXPECT_EQ(deref(items[2].as_string()), "\xf0\x9f\x98\x80");
}

TEST(JsonParse, RejectsMalformedDocuments) {
    for (const std::string_view bad :
         {"",          "{",     "[1,]", "{\"a\":1,}", "{'a':1}", "01",       "1.",
          "1e",        "+1",    ".5",   "tru",        "nul",     "\"abc",    "[1 2]",
          "{\"a\" 1}", "{1:2}", "1 2",  "// x\n1",    "NaN",     "Infinity", "1e999"}) {
        EXPECT_NE(parse_error(bad), "<parsed>") << bad;
    }
}

TEST(JsonParse, RejectsDuplicateKeys) {
    EXPECT_EQ(parse_error(R"({"a":1,"a":2})"), "duplicate key");
}

TEST(JsonParse, RejectsInvalidUtf8AndControlCharacters) {
    EXPECT_EQ(parse_error("\"\xff\""), "invalid UTF-8");
    EXPECT_EQ(parse_error("\"\xc0\xaf\""), "invalid UTF-8");      // overlong '/'
    EXPECT_EQ(parse_error("\"\xed\xa0\x80\""), "invalid UTF-8");  // encoded surrogate
    EXPECT_EQ(parse_error("\"a\nb\""), "control character in string");
    EXPECT_EQ(parse_error(R"("\ud800")"), "unpaired surrogate");
    EXPECT_EQ(parse_error(R"("\udc00")"), "unpaired surrogate");
    EXPECT_EQ(parse_error(R"("\x")"), "invalid escape");
}

TEST(JsonParse, EnforcesLimits) {
    JsonLimits limits;
    limits.max_depth = 3;
    EXPECT_EQ(parse_error("[[[1]]]", limits), "nesting deeper than 3");
    EXPECT_NE(parse_error("[[1]]", limits), "nesting deeper than 3");
    limits = {};
    limits.max_bytes = 4;
    EXPECT_EQ(parse_error("[1,2]", limits), "document exceeds 4 bytes");
    limits = {};
    limits.max_elements = 2;
    EXPECT_EQ(parse_error("[1,2,3]", limits), "more than 2 elements");
    limits = {};
    limits.max_string_bytes = 3;
    EXPECT_NE(parse_error("\"abcdef\"", limits), "<parsed>");
}

TEST(JsonParse, DeepNestingDoesNotOverflowTheStack) {
    const std::string deep = std::string(100000, '[') + std::string(100000, ']');
    JsonLimits limits;
    limits.max_bytes = deep.size();
    EXPECT_EQ(parse_error(deep, limits), "nesting deeper than 32");
}

TEST(JsonWrite, QuotesAndEscapes) {
    EXPECT_EQ(json_quote("a\"b\\c\n\x01"), R"("a\"b\\c\n\u0001")");
    EXPECT_EQ(json_number(1.25), "1.25");
    EXPECT_EQ(json_number(std::nullopt), "null");
    EXPECT_EQ(json_number(std::numeric_limits<double>::quiet_NaN()), "null");
}

TEST(JsonWrite, RoundTripsThroughTheParser) {
    const std::string text = "tab\t quote\" slash\\ unicode \xc3\xa9 ctl \x1f";
    const auto v = parse_ok(json_quote(text));
    EXPECT_EQ(deref(v.as_string()), text);
}

}  // namespace
}  // namespace faulttwin::core

#include <gtest/gtest.h>

#include <variant>

#include "faulttwin/collectors/endpoint.hpp"
#include "faulttwin/collectors/lsn.hpp"
#include "faulttwin/collectors/reading.hpp"
#include "faulttwin/collectors/sql.hpp"

namespace faulttwin::collectors {
namespace {

TEST(ReadingTest, DefaultIsMissingNotZero) {
    const Reading<double> r;
    EXPECT_EQ(r.quality(), Quality::kMissing);
    EXPECT_FALSE(r.value().has_value());
}

TEST(ReadingTest, ValueEngagedOnlyForOkAndStale) {
    auto ok = Reading<double>::ok(0.0);
    EXPECT_TRUE(ok.is_ok());
    ASSERT_TRUE(ok.value().has_value());
    EXPECT_EQ(*ok.value(), 0.0);  // a measured zero is a value
    EXPECT_FALSE(Reading<double>::missing("x").value().has_value());
    EXPECT_FALSE(Reading<double>::invalid("x").value().has_value());
    EXPECT_FALSE(Reading<double>::not_applicable().value().has_value());

    ok.mark_stale("old");
    EXPECT_EQ(ok.quality(), Quality::kStale);
    ASSERT_TRUE(ok.value().has_value());  // kept for context
    EXPECT_EQ(ok.detail(), "old");
}

TEST(ReadingTest, MarkStaleLeavesNonOkReadingsAlone) {
    auto missing = Reading<double>::missing("down");
    missing.mark_stale("old");
    EXPECT_EQ(missing.quality(), Quality::kMissing);
    EXPECT_EQ(missing.detail(), "down");
    auto invalid = Reading<double>::invalid("negative");
    invalid.mark_stale("old");
    EXPECT_EQ(invalid.quality(), Quality::kInvalid);
}

TEST(ReadingTest, QualityStrings) {
    EXPECT_EQ(to_string(Quality::kOk), "ok");
    EXPECT_EQ(to_string(Quality::kMissing), "missing");
    EXPECT_EQ(to_string(Quality::kStale), "stale");
    EXPECT_EQ(to_string(Quality::kInvalid), "invalid");
    EXPECT_EQ(to_string(Quality::kNotApplicable), "not_applicable");
}

TEST(LsnTest, ParsesServerFormat) {
    EXPECT_EQ(parse_lsn("0/0")->value, 0U);
    EXPECT_EQ(parse_lsn("0/3000100")->value, 0x3000100U);
    EXPECT_EQ(parse_lsn("16/B374D848")->value, (0x16ULL << 32U) | 0xB374D848ULL);
    EXPECT_EQ(parse_lsn("ffffffff/ffffffff")->value, ~0ULL);
}

TEST(LsnTest, RejectsMalformedText) {
    for (const char* bad : {"", "/", "0/", "/0", "0", "0/0/0", " 0/0", "0/0 ", "g/0", "0/-1",
                            "123456789/0", "0/123456789", "+1/0"}) {
        EXPECT_FALSE(parse_lsn(bad).has_value()) << bad;
    }
}

TEST(LsnTest, FormatsLikeTheServerAndRoundTrips) {
    EXPECT_EQ(to_string(Lsn{0}), "0/0");
    EXPECT_EQ(to_string(Lsn{(0x16ULL << 32U) | 0xB374D848ULL}), "16/B374D848");
    const Lsn lsn{0x1234ABCD5678ULL};
    EXPECT_EQ(parse_lsn(to_string(lsn)), lsn);
}

TEST(LsnTest, BytesBetweenRefusesNegativeDistances) {
    EXPECT_EQ(bytes_between(Lsn{100}, Lsn{350}), 250U);
    EXPECT_EQ(bytes_between(Lsn{100}, Lsn{100}), 0U);
    EXPECT_FALSE(bytes_between(Lsn{350}, Lsn{100}).has_value());
}

TEST(SqlTest, ClassifiesSqlstates) {
    EXPECT_EQ(classify_sqlstate("42501"), SqlErrorKind::kPermissionDenied);
    EXPECT_EQ(classify_sqlstate("42P01"), SqlErrorKind::kUndefinedObject);
    EXPECT_EQ(classify_sqlstate("42883"), SqlErrorKind::kUndefinedObject);
    EXPECT_EQ(classify_sqlstate("57014"), SqlErrorKind::kTimeout);
    EXPECT_EQ(classify_sqlstate("25006"), SqlErrorKind::kReadOnly);
    EXPECT_EQ(classify_sqlstate("28P01"), SqlErrorKind::kAuthenticationFailed);
    EXPECT_EQ(classify_sqlstate("08006"), SqlErrorKind::kConnectionFailed);
    EXPECT_EQ(classify_sqlstate("57P01"), SqlErrorKind::kConnectionFailed);
    EXPECT_EQ(classify_sqlstate("22012"), SqlErrorKind::kQueryFailed);
    EXPECT_EQ(classify_sqlstate(""), SqlErrorKind::kQueryFailed);
    EXPECT_EQ(classify_sqlstate("4250"), SqlErrorKind::kQueryFailed);
}

TEST(SqlTest, SanitizeCollapsesWhitespaceDropsControlsAndBounds) {
    EXPECT_EQ(sanitize_message("  connection to server\n\tfailed:\r\n refused \n"),
              "connection to server failed: refused");
    EXPECT_EQ(sanitize_message(std::string("a\x01\x1b[31mb\x7f")), "a[31mb");
    EXPECT_EQ(sanitize_message(std::string(1000, 'x')).size(), 240U);
    EXPECT_EQ(sanitize_message("abcdef", 3), "abc");
}

TEST(EndpointTest, ParsesList) {
    const auto parsed =
        parse_endpoint_list("pg-primary=127.0.0.1:55432, pg-replica-1=localhost:55433");
    const auto* list = std::get_if<std::vector<PgEndpoint>>(&parsed);
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->size(), 2U);
    EXPECT_EQ((*list)[0], (PgEndpoint{"pg-primary", "127.0.0.1", 55432}));
    EXPECT_EQ((*list)[1], (PgEndpoint{"pg-replica-1", "localhost", 55433}));
}

TEST(EndpointTest, AcceptsIpv6HostByLastColon) {
    const auto parsed = parse_endpoint_list("a=::1:5432");
    const auto* list = std::get_if<std::vector<PgEndpoint>>(&parsed);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ((*list)[0].host, "::1");
    EXPECT_EQ((*list)[0].port, 5432);
}

TEST(EndpointTest, RejectsBadEntries) {
    for (const char* bad : {"", "a", "a=", "a=host", "a=:5432", "a=h:0", "a=h:65536", "a=h:x",
                            "a=h:1,a=h:2", "-a=h:1", "a b=h:1", "a=h:1,"}) {
        EXPECT_TRUE(std::holds_alternative<EndpointParseError>(parse_endpoint_list(bad))) << bad;
    }
}

}  // namespace
}  // namespace faulttwin::collectors

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

#include "faulttwin/core/logger.hpp"
#include "faulttwin/core/time.hpp"

namespace faulttwin::core {
namespace {

using namespace std::chrono_literals;

const WallTime kT0 =
    WallTime{std::chrono::sys_days{std::chrono::year{2026} / 1 / 2}} + 3h + 4min + 5s + 678ms;

TEST(Time, FormatsUtcMilliseconds) {
    EXPECT_EQ(iso8601(kT0), "2026-01-02T03:04:05.678Z");
}

TEST(Time, ParsesWhatItFormats) {
    EXPECT_EQ(parse_iso8601(iso8601(kT0)), kT0);
    EXPECT_EQ(parse_iso8601("2026-01-02T03:04:05Z"), kT0 - 678ms);
    EXPECT_EQ(parse_iso8601("2026-01-02T03:04:05.6Z"), kT0 - 78ms);
}

TEST(Time, RejectsAmbiguousOrImpossibleTimes) {
    for (const std::string_view bad :
         {"2026-01-02T03:04:05", "2026-01-02T03:04:05+01:00", "2026-02-30T00:00:00Z",
          "2026-13-01T00:00:00Z", "2026-01-02T24:00:00Z", "2026-01-02 03:04:05Z", "",
          "2026-01-02T03:04:05.Z", "2026-01-02T03:04:05.1234567890Z", "yesterday"}) {
        EXPECT_FALSE(parse_iso8601(bad).has_value()) << bad;
    }
}

TEST(Time, MicrosecondsRoundTripAtPostgresPrecision) {
    const WallTime t = kT0 + 123us;
    EXPECT_EQ(from_unix_micros(to_unix_micros(t)), t);
}

TEST(Logger, WritesOneJsonObjectPerLine) {
    std::vector<std::string> lines;
    Logger logger([&](std::string_view l) { lines.emplace_back(l); }, LogLevel::kInfo,
                  [] { return kT0; });
    logger.info("api", "request",
                {{"status", 200}, {"route", "/healthz"}, {"ok", true}, {"duration_ms", 1.5}});
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_EQ(
        lines[0],
        R"({"ts":"2026-01-02T03:04:05.678Z","level":"info","component":"api","msg":"request",)"
        R"("status":200,"route":"/healthz","ok":true,"duration_ms":1.5})");
}

TEST(Logger, DropsRecordsBelowTheThreshold) {
    std::vector<std::string> lines;
    Logger logger([&](std::string_view l) { lines.emplace_back(l); }, LogLevel::kWarn);
    logger.info("x", "hidden");
    logger.debug("x", "hidden");
    logger.error("x", "shown");
    EXPECT_EQ(lines.size(), 1U);
    EXPECT_EQ(logger.records_written(), 1U);
}

TEST(Logger, RedactsSecretFieldsAndSecretsInText) {
    std::vector<std::string> lines;
    Logger logger([&](std::string_view l) { lines.emplace_back(l); });
    logger.error("db", "connect failed: host=pg password=hunter2 user=ft",
                 {{"FT_MONITOR_PASSWORD", "hunter2"},
                  {"api_key", "k123"},
                  {"authorization", "Bearer abc.def"},
                  {"detail", "header was Bearer s3cr3t, retrying"},
                  {"dsn", "dbname=x password='a b' sslmode=prefer"}});
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_EQ(lines[0].find("hunter2"), std::string::npos) << lines[0];
    EXPECT_EQ(lines[0].find("k123"), std::string::npos) << lines[0];
    EXPECT_EQ(lines[0].find("abc.def"), std::string::npos) << lines[0];
    EXPECT_EQ(lines[0].find("s3cr3t"), std::string::npos) << lines[0];
    EXPECT_EQ(lines[0].find("a b"), std::string::npos) << lines[0];
    EXPECT_NE(lines[0].find("user=ft"), std::string::npos) << lines[0];
    EXPECT_NE(lines[0].find("sslmode=prefer"), std::string::npos) << lines[0];
}

TEST(Logger, ReservedFieldNamesCannotOverwriteTheEnvelope) {
    const auto line = format_log_record(kT0, LogLevel::kInfo, "c", "m", {{"level", "fake"}});
    EXPECT_NE(line.find(R"("level":"info")"), std::string::npos);
    EXPECT_NE(line.find(R"("field.level":"fake")"), std::string::npos);
}

TEST(Logger, EscapesInjectedNewlines) {
    const auto line =
        format_log_record(kT0, LogLevel::kInfo, "c", "line1\n{\"level\":\"error\"}", {});
    EXPECT_EQ(line.find('\n'), std::string::npos);
}

TEST(Logger, LinesFromManyThreadsNeverInterleave) {
    std::vector<std::string> lines;
    Logger logger([&](std::string_view l) { lines.emplace_back(l); });
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&logger, t] {
                for (int i = 0; i < 200; ++i) {
                    logger.info("t", "x", {{"thread", t}, {"i", i}});
                }
            });
        }
    }
    ASSERT_EQ(lines.size(), 800U);
    for (const auto& l : lines) {
        EXPECT_EQ(l.front(), '{');
        EXPECT_EQ(l.back(), '}');
    }
}

}  // namespace
}  // namespace faulttwin::core

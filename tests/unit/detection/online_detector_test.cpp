#include "faulttwin/detection/online_detector.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace col = faulttwin::collectors;
namespace det = faulttwin::detection;
using faulttwin::telemetry::NodeRole;
using std::chrono::milliseconds;
using std::chrono::seconds;

const col::Timestamp kT0{std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}};

col::PgNodeSnapshot node(const std::string& name, NodeRole role, bool up, col::Timestamp at) {
    col::PgNodeSnapshot s;
    s.node = name;
    s.role = up ? role : NodeRole::kUnknown;
    s.availability = up ? col::Availability::kUp : col::Availability::kDown;
    s.attempted_at = at;
    s.collected_at = at;
    if (up) {
        s.probe_latency_ms = col::Reading<double>::ok(0.3);
        s.connections.utilization_percent = col::Reading<double>::ok(5.0);
    }
    return s;
}

std::vector<col::PgNodeSnapshot> cycle(std::int64_t second, bool replica_up,
                                       milliseconds replica_delay = milliseconds{0}) {
    const auto t = kT0 + seconds{second};
    auto primary = node("p", NodeRole::kPrimary, true, t);
    col::StandbyLink link;
    link.application_name = "r";
    link.replay_lag_bytes = col::Reading<double>::ok(0.0);
    link.replay_lag_seconds = col::Reading<double>::missing("caught up");
    if (replica_up) {
        primary.standbys = {link};
    }
    return {primary, node("r", NodeRole::kReplica, replica_up, t + replica_delay)};
}

std::size_t count(const std::vector<det::Detection>& d, det::DetectionKind kind,
                  const std::string& rule, const std::string& n) {
    std::size_t c = 0;
    for (const auto& x : d) {
        c += x.kind == kind && x.rule_id == rule && x.node == n ? 1U : 0U;
    }
    return c;
}

TEST(OnlineDetectorTest, DownReplicaRaisesStaleTelemetryThenResolves) {
    det::OnlineDetector detector;
    std::vector<det::Detection> all;
    std::int64_t t = 0;
    for (; t < 10; ++t) {
        auto d = detector.on_cycle(cycle(t, true), {});
        EXPECT_TRUE(d.empty()) << "healthy cluster at t=" << t;
    }
    std::optional<std::int64_t> fired_at;
    for (; t < 30; ++t) {
        auto d = detector.on_cycle(cycle(t, false), {});
        if (!fired_at && count(d, det::DetectionKind::kFired, "telemetry_stale", "r") == 1) {
            fired_at = t;
        }
        EXPECT_EQ(count(d, det::DetectionKind::kFired, "telemetry_stale", "p"), 0U);
    }
    ASSERT_TRUE(fired_at);
    EXPECT_LE(*fired_at - 10, 5) << "fired within the stale threshold plus sustain";
    std::size_t resolved = 0;
    for (; t < 60; ++t) {
        resolved += count(detector.on_cycle(cycle(t, true), {}), det::DetectionKind::kResolved,
                          "telemetry_stale", "r");
    }
    EXPECT_EQ(resolved, 1U);
    EXPECT_EQ(detector.counters().observations_refused, 0U);
    EXPECT_EQ(detector.counters().vectors_refused, 0U);
}

TEST(OnlineDetectorTest, ASlowNodeDoesNotMakeTheOthersLookStale) {
    // The replica answers 2.5 s after the primary in every cycle (e.g. a client deadline).
    // Each node is evaluated as of its own snapshot, so nobody is stale.
    det::OnlineDetector detector;
    for (std::int64_t t = 0; t < 60; t += 3) {
        const auto d = detector.on_cycle(cycle(t, true, milliseconds{2500}), {});
        EXPECT_TRUE(d.empty()) << "t=" << t;
    }
}

}  // namespace

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "faulttwin/storage/incident_recorder.hpp"
#include "flaky_store.hpp"

namespace faulttwin::storage {
namespace {

using namespace std::chrono_literals;
using detection::Detection;

const Timestamp kT0{std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}};

Detection det(DetectionKind kind, std::string rule, std::string node, Timestamp at,
              Severity severity = Severity::kWarning) {
    Detection d;
    d.kind = kind;
    d.rule_set_version = "faulttwin.rules.v1";
    d.rule_id = std::move(rule);
    d.node = std::move(node);
    d.severity = severity;
    d.at = at;
    d.violation_since = at - 10s;
    d.evidence = {{"x.last", 1.0}};
    return d;
}

class RecorderTest : public ::testing::Test {
  protected:
    std::vector<std::string> lines;
    core::Logger logger{[this](std::string_view l) { lines.emplace_back(l); }};
    testing::FlakyStore store;
    observability::Registry metrics;
};

TEST_F(RecorderTest, FollowsTheAlertLifecycle) {
    IncidentRecorder recorder(store, logger, "faulttwin.features.v1", 16, &metrics);
    recorder.start(kT0);
    recorder.record({det(DetectionKind::kFired, "r", "n", kT0)});
    recorder.record({det(DetectionKind::kEscalated, "r", "n", kT0 + 5s, Severity::kCritical)});
    recorder.record({det(DetectionKind::kResolved, "r", "n", kT0 + 20s, Severity::kCritical)});
    const auto incident = std::get<Incident>(store.get(1));
    EXPECT_EQ(incident.state, IncidentState::kResolved);
    EXPECT_EQ(incident.severity, Severity::kCritical);
    EXPECT_EQ(incident.feature_schema_version, "faulttwin.features.v1");
    EXPECT_EQ(incident.events.size(), 3U);
    EXPECT_EQ(recorder.counters().opened, 1U);
    EXPECT_EQ(recorder.counters().escalated, 1U);
    EXPECT_EQ(recorder.counters().resolved, 1U);
    EXPECT_NE(metrics.render().find("faulttwin_incident_operations_total{op=\"open\"} 1"),
              std::string::npos);
}

TEST_F(RecorderTest, KeepsOrderAcrossAnOutageAndRetries) {
    IncidentRecorder recorder(store, logger, "v1", 16);
    recorder.start(kT0);
    store.available = false;
    recorder.record({det(DetectionKind::kFired, "r", "n", kT0)});
    recorder.record({det(DetectionKind::kResolved, "r", "n", kT0 + 20s)});
    EXPECT_EQ(recorder.pending(), 2U);
    EXPECT_GE(recorder.counters().retries, 2U);
    store.available = true;
    recorder.flush();
    EXPECT_EQ(recorder.pending(), 0U);
    const auto incident = std::get<Incident>(store.get(1));
    EXPECT_EQ(incident.state, IncidentState::kResolved);
    EXPECT_EQ(incident.resolved_at, kT0 + 20s);
}

TEST_F(RecorderTest, BoundedQueueDropsAndCountsInsteadOfGrowing) {
    IncidentRecorder recorder(store, logger, "v1", 2, &metrics);
    recorder.start(kT0);
    store.available = false;
    recorder.record({det(DetectionKind::kFired, "r", "a", kT0),
                     det(DetectionKind::kFired, "r", "b", kT0),
                     det(DetectionKind::kFired, "r", "c", kT0)});
    EXPECT_EQ(recorder.pending(), 2U);
    EXPECT_EQ(recorder.counters().dropped, 1U);
    EXPECT_NE(metrics.render().find("faulttwin_incident_detections_dropped_total 1"),
              std::string::npos);
    store.available = true;
    recorder.flush();
    EXPECT_EQ(recorder.pending(), 0U);
    recorder.record({det(DetectionKind::kResolved, "r", "c", kT0 + 1s)});
    // The open for "c" was dropped, so its resolve has nothing to attach to.
    EXPECT_EQ(recorder.counters().unmatched, 1U);
    EXPECT_EQ(std::get<StoreStats>(store.stats()).open, 2U);
}

TEST_F(RecorderTest, ClosesOrphansAtStartEvenIfTheStoreWasDownFirst) {
    (void)store.open({"v", "f", "r", "n", Severity::kWarning, kT0, kT0, {}});
    store.available = false;
    IncidentRecorder recorder(store, logger, "v1", 16);
    recorder.start(kT0 + 1h);
    EXPECT_FALSE(recorder.started());
    recorder.record({det(DetectionKind::kFired, "r", "n", kT0 + 2h)});
    EXPECT_EQ(recorder.pending(), 1U);
    store.available = true;
    recorder.flush();
    EXPECT_TRUE(recorder.started());
    EXPECT_EQ(recorder.counters().orphans_closed, 1U);
    const auto old = std::get<Incident>(store.get(1));
    EXPECT_EQ(old.resolution, Resolution::kMonitorRestarted);
    EXPECT_EQ(std::get<Incident>(store.get(2)).state, IncidentState::kOpen);
}

TEST_F(RecorderTest, PermanentErrorsAreSkippedAndLogged) {
    IncidentRecorder recorder(store, logger, "v1", 16);
    recorder.start(kT0);
    recorder.record({det(DetectionKind::kFired, "", "n", kT0)});  // invalid: empty rule id
    EXPECT_EQ(recorder.pending(), 0U);
    EXPECT_EQ(recorder.counters().rejected, 1U);
    bool logged = false;
    for (const auto& l : lines) {
        logged = logged || l.find("incident store refused a detection") != std::string::npos;
    }
    EXPECT_TRUE(logged);
}

}  // namespace
}  // namespace faulttwin::storage

#pragma once

// Behaviour every IncidentStore must have. The unit tests run it against MemoryIncidentStore;
// the integration tests run the same cases against PgIncidentStore on the meta-db.

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "faulttwin/storage/incident_store.hpp"

namespace faulttwin::storage::contract {

using namespace std::chrono_literals;

/// Returns a store that is empty when the test starts.
using StoreFactory = std::function<std::unique_ptr<IncidentStore>()>;

inline const Timestamp kT0{std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}};

inline NewIncident new_incident(std::string rule, std::string node, Timestamp at = kT0) {
    return {"faulttwin.rules.v1",
            "faulttwin.features.v1",
            std::move(rule),
            std::move(node),
            Severity::kWarning,
            at - 10s,
            at,
            {{"lag.last", 7.5}, {"lag.slope", std::nullopt}}};
}

inline IncidentEvent event(DetectionKind kind, Severity severity, Timestamp at) {
    return {kind, severity, at, {{"lag.last", 12.25}}};
}

class IncidentStoreContract : public ::testing::TestWithParam<StoreFactory> {
  protected:
    void SetUp() override {
        store_ = GetParam()();
        if (store_ == nullptr) {
            // A factory returns nullptr when its backend is not enabled (the Postgres
            // instantiation without FT_INTEGRATION=1). A factory that fails to build an enabled
            // backend reports the failure itself with ADD_FAILURE.
            GTEST_SKIP() << "store backend not enabled";
        }
    }
    std::uint64_t open_ok(const NewIncident& n) {
        auto r = store_->open(n);
        EXPECT_TRUE(std::holds_alternative<std::uint64_t>(r))
            << (std::holds_alternative<StoreError>(r) ? std::get<StoreError>(r).message : "");
        return std::holds_alternative<std::uint64_t>(r) ? std::get<std::uint64_t>(r) : 0;
    }
    Incident get_ok(std::uint64_t id) {
        auto r = store_->get(id);
        EXPECT_TRUE(std::holds_alternative<Incident>(r));
        return std::holds_alternative<Incident>(r) ? std::get<Incident>(r) : Incident{};
    }
    static StoreErrorKind error_kind(const auto& result) {
        return std::holds_alternative<StoreError>(result) ? std::get<StoreError>(result).kind
                                                          : StoreErrorKind::kInternal;
    }
    std::unique_ptr<IncidentStore> store_;
};

TEST_P(IncidentStoreContract, OpenThenGetRoundTripsEveryField) {
    const auto id = open_ok(new_incident("replication_lag_high", "pg-replica-1"));
    const auto incident = get_ok(id);
    EXPECT_EQ(incident.id, id);
    EXPECT_EQ(incident.rule_set_version, "faulttwin.rules.v1");
    EXPECT_EQ(incident.feature_schema_version, "faulttwin.features.v1");
    EXPECT_EQ(incident.rule_id, "replication_lag_high");
    EXPECT_EQ(incident.node, "pg-replica-1");
    EXPECT_EQ(incident.severity, Severity::kWarning);
    EXPECT_EQ(incident.state, IncidentState::kOpen);
    EXPECT_FALSE(incident.resolution.has_value());
    EXPECT_EQ(incident.violation_since, kT0 - 10s);
    EXPECT_EQ(incident.opened_at, kT0);
    EXPECT_EQ(incident.updated_at, kT0);
    EXPECT_FALSE(incident.resolved_at.has_value());
    EXPECT_FALSE(incident.acknowledgement.has_value());
    ASSERT_EQ(incident.events.size(), 1U);
    EXPECT_EQ(incident.events[0].kind, DetectionKind::kFired);
    ASSERT_EQ(incident.events[0].evidence.size(), 2U);
    EXPECT_EQ(incident.events[0].evidence[0].feature, "lag.last");
    EXPECT_EQ(incident.events[0].evidence[0].value, 7.5);
    EXPECT_EQ(incident.events[0].evidence[1].feature, "lag.slope");
    EXPECT_FALSE(incident.events[0].evidence[1].value.has_value());
}

TEST_P(IncidentStoreContract, OneOpenIncidentPerRuleAndNode) {
    (void)open_ok(new_incident("r", "n1"));
    EXPECT_EQ(error_kind(store_->open(new_incident("r", "n1", kT0 + 1s))),
              StoreErrorKind::kConflict);
    (void)open_ok(new_incident("r", "n2"));
    (void)open_ok(new_incident("other", "n1"));
}

TEST_P(IncidentStoreContract, EscalateRaisesSeverityAndAppendsEvent) {
    const auto id = open_ok(new_incident("r", "n"));
    ASSERT_TRUE(std::holds_alternative<Done>(
        store_->escalate(id, event(DetectionKind::kEscalated, Severity::kCritical, kT0 + 5s))));
    const auto incident = get_ok(id);
    EXPECT_EQ(incident.severity, Severity::kCritical);
    EXPECT_EQ(incident.updated_at, kT0 + 5s);
    ASSERT_EQ(incident.events.size(), 2U);
    EXPECT_EQ(incident.events[1].kind, DetectionKind::kEscalated);
    EXPECT_EQ(incident.events[1].at, kT0 + 5s);
}

TEST_P(IncidentStoreContract, ResolveClosesAndAllowsANewIncident) {
    const auto id = open_ok(new_incident("r", "n"));
    ASSERT_TRUE(std::holds_alternative<Done>(
        store_->resolve(id, event(DetectionKind::kResolved, Severity::kWarning, kT0 + 30s),
                        Resolution::kRecovered)));
    const auto incident = get_ok(id);
    EXPECT_EQ(incident.state, IncidentState::kResolved);
    EXPECT_EQ(incident.resolution, Resolution::kRecovered);
    EXPECT_EQ(incident.resolved_at, kT0 + 30s);
    EXPECT_EQ(incident.events.back().kind, DetectionKind::kResolved);
    EXPECT_EQ(error_kind(store_->resolve(
                  id, event(DetectionKind::kResolved, Severity::kWarning, kT0 + 31s),
                  Resolution::kRecovered)),
              StoreErrorKind::kConflict);
    EXPECT_EQ(error_kind(store_->escalate(
                  id, event(DetectionKind::kEscalated, Severity::kCritical, kT0 + 31s))),
              StoreErrorKind::kConflict);
    const auto again = open_ok(new_incident("r", "n", kT0 + 60s));
    EXPECT_GT(again, id);
}

TEST_P(IncidentStoreContract, MissingIdsAreNotFound) {
    EXPECT_EQ(error_kind(store_->get(424242)), StoreErrorKind::kNotFound);
    EXPECT_EQ(error_kind(store_->escalate(
                  424242, event(DetectionKind::kEscalated, Severity::kCritical, kT0))),
              StoreErrorKind::kNotFound);
    EXPECT_EQ(
        error_kind(store_->resolve(424242, event(DetectionKind::kResolved, Severity::kWarning, kT0),
                                   Resolution::kRecovered)),
        StoreErrorKind::kNotFound);
    EXPECT_EQ(error_kind(store_->acknowledge(424242, {"alice", "", kT0})),
              StoreErrorKind::kNotFound);
}

TEST_P(IncidentStoreContract, AcknowledgeOnceAndNeverOverwrite) {
    const auto id = open_ok(new_incident("r", "n"));
    auto first = store_->acknowledge(id, {"alice", "looking into it\nline 2", kT0 + 2s});
    ASSERT_TRUE(std::holds_alternative<Incident>(first));
    ASSERT_TRUE(std::get<Incident>(first).acknowledgement.has_value());
    EXPECT_EQ(std::get<Incident>(first).acknowledgement->by, "alice");
    EXPECT_EQ(std::get<Incident>(first).acknowledgement->note, "looking into it\nline 2");
    EXPECT_EQ(std::get<Incident>(first).acknowledgement->at, kT0 + 2s);
    EXPECT_EQ(error_kind(store_->acknowledge(id, {"bob", "", kT0 + 3s})),
              StoreErrorKind::kConflict);
    EXPECT_EQ(get_ok(id).acknowledgement->by, "alice");
}

TEST_P(IncidentStoreContract, RejectsInvalidInput) {
    auto bad = new_incident("", "n");
    EXPECT_EQ(error_kind(store_->open(bad)), StoreErrorKind::kInvalid);
    bad = new_incident("r", std::string(200, 'x'));
    EXPECT_EQ(error_kind(store_->open(bad)), StoreErrorKind::kInvalid);
    bad = new_incident("r", "n");
    bad.violation_since = bad.opened_at + 1s;
    EXPECT_EQ(error_kind(store_->open(bad)), StoreErrorKind::kInvalid);
    const auto id = open_ok(new_incident("r", "n"));
    EXPECT_EQ(error_kind(store_->acknowledge(id, {"", "", kT0})), StoreErrorKind::kInvalid);
    EXPECT_EQ(error_kind(store_->acknowledge(id, {"a\x01", "", kT0})), StoreErrorKind::kInvalid);
    EXPECT_EQ(error_kind(store_->acknowledge(id, {"a", std::string(2000, 'n'), kT0})),
              StoreErrorKind::kInvalid);
}

TEST_P(IncidentStoreContract, CloseOrphansResolvesEveryOpenIncident) {
    const auto a = open_ok(new_incident("r", "n1"));
    const auto b = open_ok(new_incident("r", "n2"));
    ASSERT_TRUE(std::holds_alternative<Done>(store_->resolve(
        b, event(DetectionKind::kResolved, Severity::kWarning, kT0 + 1s), Resolution::kRecovered)));
    auto closed = store_->close_orphans(kT0 + 1h);
    ASSERT_TRUE(std::holds_alternative<std::uint64_t>(closed));
    EXPECT_EQ(std::get<std::uint64_t>(closed), 1U);
    const auto incident = get_ok(a);
    EXPECT_EQ(incident.state, IncidentState::kResolved);
    EXPECT_EQ(incident.resolution, Resolution::kMonitorRestarted);
    EXPECT_EQ(incident.resolved_at, kT0 + 1h);
    EXPECT_EQ(get_ok(b).resolution, Resolution::kRecovered);
    (void)open_ok(new_incident("r", "n1", kT0 + 2h));
}

TEST_P(IncidentStoreContract, ListFiltersAndPagesNewestFirst) {
    std::vector<std::uint64_t> ids;
    for (int i = 0; i < 5; ++i) {
        ids.push_back(
            open_ok(new_incident("r", "n" + std::to_string(i), kT0 + std::chrono::seconds{i})));
    }
    ASSERT_TRUE(std::holds_alternative<Done>(
        store_->resolve(ids[1], event(DetectionKind::kResolved, Severity::kWarning, kT0 + 9s),
                        Resolution::kRecovered)));

    IncidentQuery q;
    q.limit = 2;
    auto page = store_->list(q);
    ASSERT_TRUE(std::holds_alternative<IncidentPage>(page));
    auto& p1 = std::get<IncidentPage>(page);
    ASSERT_EQ(p1.items.size(), 2U);
    EXPECT_EQ(p1.items[0].id, ids[4]);
    EXPECT_EQ(p1.items[1].id, ids[3]);
    ASSERT_TRUE(p1.next_before_id.has_value());

    q.before_id = p1.next_before_id;
    auto p2 = std::get<IncidentPage>(store_->list(q));
    ASSERT_EQ(p2.items.size(), 2U);
    EXPECT_EQ(p2.items[0].id, ids[2]);
    q.before_id = p2.next_before_id;
    auto p3 = std::get<IncidentPage>(store_->list(q));
    ASSERT_EQ(p3.items.size(), 1U);
    EXPECT_EQ(p3.items[0].id, ids[0]);
    EXPECT_FALSE(p3.next_before_id.has_value());

    IncidentQuery resolved;
    resolved.state = IncidentState::kResolved;
    auto r = std::get<IncidentPage>(store_->list(resolved));
    ASSERT_EQ(r.items.size(), 1U);
    EXPECT_EQ(r.items[0].id, ids[1]);

    IncidentQuery by_node;
    by_node.node = "n3";
    EXPECT_EQ(std::get<IncidentPage>(store_->list(by_node)).items.size(), 1U);

    IncidentQuery window;
    window.opened_since = kT0 + 1s;
    window.opened_until = kT0 + 3s;
    auto w = std::get<IncidentPage>(store_->list(window));
    ASSERT_EQ(w.items.size(), 2U);
    EXPECT_EQ(w.items[0].id, ids[2]);
    EXPECT_EQ(w.items[1].id, ids[1]);

    IncidentQuery none;
    none.rule_id = "no_such_rule";
    EXPECT_TRUE(std::get<IncidentPage>(store_->list(none)).items.empty());

    IncidentQuery invalid;
    invalid.limit = 0;
    EXPECT_EQ(error_kind(store_->list(invalid)), StoreErrorKind::kInvalid);
}

TEST_P(IncidentStoreContract, StatsCountOpenAndResolved) {
    const auto a = open_ok(new_incident("r", "n1"));
    (void)open_ok(new_incident("r", "n2"));
    ASSERT_TRUE(std::holds_alternative<Done>(store_->resolve(
        a, event(DetectionKind::kResolved, Severity::kWarning, kT0 + 1s), Resolution::kRecovered)));
    auto s = store_->stats();
    ASSERT_TRUE(std::holds_alternative<StoreStats>(s));
    EXPECT_EQ(std::get<StoreStats>(s).open, 1U);
    EXPECT_EQ(std::get<StoreStats>(s).resolved, 1U);
    EXPECT_TRUE(std::holds_alternative<Done>(store_->ping()));
}

TEST_P(IncidentStoreContract, StoresUntrustedTextVerbatim) {
    auto n = new_incident("r'; DROP TABLE faulttwin.incidents; --", "node \"x\" \\ \xc3\xa9");
    const auto id = open_ok(n);
    const auto incident = get_ok(id);
    EXPECT_EQ(incident.rule_id, n.rule_id);
    EXPECT_EQ(incident.node, n.node);
    IncidentQuery q;
    q.rule_id = n.rule_id;
    EXPECT_EQ(std::get<IncidentPage>(store_->list(q)).items.size(), 1U);
}

}  // namespace faulttwin::storage::contract

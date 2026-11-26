#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "faulttwin/collectors/pg_snapshot.hpp"
#include "faulttwin/collectors/sql.hpp"

namespace faulttwin::collectors::testing {

/// A scripted SqlSession. Each rule maps a substring of the SQL text to an outcome; the first
/// matching rule wins. Transaction control statements succeed unless scripted otherwise.
class FakeSqlSession final : public SqlSession {
  public:
    void on(std::string fragment, SqlOutcome outcome) {
        rules_.emplace_back(std::move(fragment), std::move(outcome));
    }

    SqlOutcome query(std::string_view sql) override {
        log.emplace_back(sql);
        for (const auto& [fragment, outcome] : rules_) {
            if (sql.find(fragment) != std::string_view::npos) {
                return outcome;
            }
        }
        if (sql.starts_with("BEGIN") || sql == "COMMIT" || sql == "ROLLBACK") {
            return SqlTable{};
        }
        return SqlError{SqlErrorKind::kQueryFailed, "XX000", "no rule for: " + std::string(sql)};
    }

    [[nodiscard]] int count(std::string_view fragment) const {
        int n = 0;
        for (const auto& q : log) {
            n += q.find(fragment) != std::string::npos ? 1 : 0;
        }
        return n;
    }

    std::vector<std::string> log;

  private:
    std::vector<std::pair<std::string, SqlOutcome>> rules_;
};

using Row = std::vector<std::optional<std::string>>;

inline SqlTable table(std::size_t columns, std::vector<Row> rows) {
    SqlTable t;
    for (std::size_t i = 0; i < columns; ++i) {
        t.columns.push_back("c" + std::to_string(i));
    }
    t.rows = std::move(rows);
    return t;
}

/// Wall clock starts at a fixed instant; both clocks advance 1 ms per call.
struct FakeClocks {
    telemetry::Timestamp wall_now{std::chrono::sys_days{std::chrono::year{2026} / 11 / 26}};
    SteadyTime steady_now{};

    CollectorClock clock() {
        return CollectorClock{[this] { return wall_now += std::chrono::milliseconds{1}; },
                              [this] { return steady_now += std::chrono::milliseconds{1}; }};
    }
};

/// Healthy primary on PostgreSQL 17 with two streaming standbys and two active slots.
inline void script_primary(FakeSqlSession& s) {
    s.on("pg_is_in_recovery()", table(4, {{"f", "170012", "pg-primary", "100"}}));
    s.on("SELECT 1", table(1, {{"1"}}));
    s.on("FROM pg_stat_activity", table(4, {{"7", "2", "1", "0"}}));
    s.on("FROM pg_stat_database", table(2, {{"1500", "3"}}));
    s.on("to_regclass", table(1, {{"t"}}));
    s.on("FROM pg_stat_statements", table(2, {{"200", "50.5"}}));
    s.on("pg_stat_force_next_flush", table(1, {{""}}));
    s.on("FROM pg_stat_replication",
         table(11, {{"pg-replica-1", "172.18.0.3", "streaming", "async", "0/3000100", "0/3000100",
                     "0/3000000", "0.001", "0.002", "0.003", "0/3000100"},
                    {"pg-replica-2", "172.18.0.4", "streaming", "async", "0/3000100", "0/3000100",
                     "0/3000100", std::nullopt, std::nullopt, std::nullopt, "0/3000100"}}));
    s.on("FROM pg_replication_slots",
         table(5, {{"pg_replica_1", "t", "reserved", "0/3000000", "0/3000100"},
                   {"pg_replica_2", "t", "reserved", "0/3000100", "0/3000100"}}));
    s.on("SELECT pg_current_wal_lsn()", table(1, {{"0/3000100"}}));
}

/// Healthy, caught-up replica.
inline void script_replica(FakeSqlSession& s, std::string cluster_name = "pg-replica-1") {
    s.on("pg_is_in_recovery()", table(4, {{"t", "170012", std::move(cluster_name), "100"}}));
    s.on("SELECT 1", table(1, {{"1"}}));
    s.on("FROM pg_stat_activity", table(4, {{"1", "1", "0", "0"}}));
    s.on("FROM pg_stat_database", table(2, {{"900", "0"}}));
    s.on("to_regclass", table(1, {{"t"}}));
    s.on("FROM pg_stat_statements", table(2, {{"0", "0"}}));
    s.on("pg_stat_force_next_flush", table(1, {{""}}));
    s.on("pg_last_wal_receive_lsn", table(4, {{"0/3000100", "0/3000100", "600.5", "f"}}));
    s.on("FROM pg_stat_wal_receiver", table(3, {{"streaming", "pg-primary", "0.4"}}));
}

}  // namespace faulttwin::collectors::testing

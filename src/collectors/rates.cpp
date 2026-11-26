#include "faulttwin/collectors/rates.hpp"

#include <chrono>
#include <string>

namespace faulttwin::collectors {
namespace {

/// `own` is the number of increments the collector itself caused in the interval; it is
/// subtracted (clamped at 0, because statistics are flushed asynchronously and an interval can
/// see fewer of the collector's own transactions than it issued).
Reading<double> counter_rate(const Reading<double>& previous, const Reading<double>& current,
                             double seconds, double own) {
    if (!previous.is_ok() || !current.is_ok()) {
        return Reading<double>::missing("counter unavailable in one of the snapshots");
    }
    const double delta = current.measured() - previous.measured();
    if (delta < 0.0) {
        return Reading<double>::invalid("counter decreased (statistics reset or restart)");
    }
    return Reading<double>::ok((delta > own ? delta - own : 0.0) / seconds,
                               own > 0.0 ? "excludes the collector's own transactions" : "");
}

}  // namespace

PgRates compute_rates(const PgNodeSnapshot& previous, const PgNodeSnapshot& current) {
    PgRates rates;
    if (previous.node != current.node) {
        const std::string reason = "snapshots belong to different nodes";
        rates.interval_seconds = Reading<double>::invalid(reason);
        rates.commits_per_second = Reading<double>::invalid(reason);
        rates.rollbacks_per_second = Reading<double>::invalid(reason);
        rates.wal_bytes_per_second = Reading<double>::invalid(reason);
        rates.mean_statement_latency_ms = Reading<double>::invalid(reason);
        return rates;
    }
    const double seconds =
        std::chrono::duration<double>(current.collected_steady - previous.collected_steady).count();
    if (!(seconds > 0.0)) {
        const std::string reason = "non-positive interval between snapshots";
        rates.interval_seconds = Reading<double>::invalid(reason);
        rates.commits_per_second = Reading<double>::invalid(reason);
        rates.rollbacks_per_second = Reading<double>::invalid(reason);
        rates.wal_bytes_per_second = Reading<double>::invalid(reason);
        rates.mean_statement_latency_ms = Reading<double>::invalid(reason);
        return rates;
    }
    rates.interval_seconds = Reading<double>::ok(seconds);
    rates.commits_per_second = counter_rate(previous.xact_commit_total, current.xact_commit_total,
                                            seconds, static_cast<double>(previous.own_commits));
    rates.rollbacks_per_second =
        counter_rate(previous.xact_rollback_total, current.xact_rollback_total, seconds,
                     static_cast<double>(previous.own_rollbacks));

    if (previous.role != NodeRole::kPrimary || current.role != NodeRole::kPrimary) {
        rates.wal_bytes_per_second = Reading<double>::not_applicable("not a primary in both");
    } else if (!previous.current_wal_lsn.is_ok() || !current.current_wal_lsn.is_ok()) {
        rates.wal_bytes_per_second = Reading<double>::missing("WAL position unavailable");
    } else if (const auto bytes = bytes_between(previous.current_wal_lsn.measured(),
                                                current.current_wal_lsn.measured())) {
        rates.wal_bytes_per_second = Reading<double>::ok(static_cast<double>(*bytes) / seconds);
    } else {
        rates.wal_bytes_per_second = Reading<double>::invalid("WAL position moved backwards");
    }

    const auto& c0 = previous.statements_calls_total;
    const auto& c1 = current.statements_calls_total;
    const auto& t0 = previous.statements_exec_time_ms_total;
    const auto& t1 = current.statements_exec_time_ms_total;
    if (!c0.is_ok() || !c1.is_ok() || !t0.is_ok() || !t1.is_ok()) {
        rates.mean_statement_latency_ms =
            Reading<double>::missing("pg_stat_statements unavailable in one of the snapshots");
    } else {
        const double calls = c1.measured() - c0.measured();
        const double time_ms = t1.measured() - t0.measured();
        if (calls < 0.0 || time_ms < 0.0) {
            // pg_stat_statements can also evict entries when it reaches pg_stat_statements.max.
            rates.mean_statement_latency_ms =
                Reading<double>::invalid("statement totals decreased (reset or eviction)");
        } else if (calls == 0.0) {
            rates.mean_statement_latency_ms =
                Reading<double>::missing("no statements completed in the interval");
        } else {
            rates.mean_statement_latency_ms = Reading<double>::ok(time_ms / calls);
        }
    }
    return rates;
}

}  // namespace faulttwin::collectors

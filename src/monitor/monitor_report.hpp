#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "faulttwin/collectors/linux_resources.hpp"
#include "faulttwin/collectors/pg_snapshot.hpp"
#include "faulttwin/collectors/rates.hpp"
#include "faulttwin/collectors/topology.hpp"

namespace faulttwin::monitor {

/// Everything observed in one collection cycle, plus how much the cycle cost.
struct CycleRecord {
    std::uint32_t cycle{0};
    std::vector<collectors::PgNodeSnapshot> nodes;
    std::vector<std::optional<collectors::PgRates>> rates;  ///< parallel to nodes
    std::vector<double> node_wall_ms;                       ///< collection time per node
    std::vector<double> freshness_ms;  ///< report time - collected_at, per node (MON-FRESH)
    collectors::TopologySnapshot topology;
    std::optional<collectors::ResourceSnapshot> resources;
    double cycle_wall_ms{0.0};
    double client_cpu_ms{0.0};  ///< user + system CPU of this process during the cycle
};

/// Totals over a run, for MON-SUCC, MON-COV, MON-OVH and MON-FRESH.
struct RunSummary {
    std::uint64_t cycles{0};
    std::uint64_t attempts{0};
    std::uint64_t successes{0};  ///< attempts where the node was reachable (up or degraded)
    std::uint64_t readings_ok{0};
    std::uint64_t readings_applicable{0};
    std::vector<double> cycle_wall_ms;
    std::vector<double> node_wall_ms;
    std::vector<double> client_cpu_ms;
    std::vector<double> freshness_ms;
};

void accumulate(RunSummary& summary, const CycleRecord& record);

/// Nearest-rank percentile (p in [0, 100]) of an unsorted sample; nullopt when empty.
[[nodiscard]] std::optional<double> percentile(std::vector<double> values, double p);

[[nodiscard]] std::string json_escape(std::string_view text);

void write_cycle_json(std::ostream& out, const CycleRecord& record);
void write_cycle_text(std::ostream& out, const CycleRecord& record);
void write_summary_json(std::ostream& out, const RunSummary& summary);
void write_summary_text(std::ostream& out, const RunSummary& summary);

}  // namespace faulttwin::monitor

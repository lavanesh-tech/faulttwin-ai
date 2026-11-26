#include "monitor_report.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <ostream>
#include <system_error>

namespace faulttwin::monitor {
namespace {

using collectors::Lsn;
using collectors::Quality;
using collectors::Reading;

std::string number_text(double value) {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                      std::chars_format::general, 15);
    return result.ec == std::errc{} ? std::string(buffer.data(), result.ptr) : std::string("0");
}

std::string quote(std::string_view text) {
    std::string out(1, '"');
    out += json_escape(text);
    out += '"';
    return out;
}

/// Minimal JSON object builder. Values passed to raw() are already JSON-encoded.
class JsonObject {
  public:
    JsonObject& raw(std::string_view key, const std::string& json) {
        text_ += first_ ? "" : ",";
        first_ = false;
        text_ += quote(key);
        text_ += ':';
        text_ += json;
        return *this;
    }
    JsonObject& str(std::string_view key, std::string_view value) { return raw(key, quote(value)); }
    JsonObject& num(std::string_view key, double value) { return raw(key, number_text(value)); }
    [[nodiscard]] std::string done() const { return text_ + "}"; }

  private:
    std::string text_{"{"};
    bool first_{true};
};

template <typename Range, typename Fn>
std::string json_array(const Range& items, const Fn& encode) {
    std::string out = "[";
    bool first = true;
    for (const auto& item : items) {
        out += first ? "" : ",";
        first = false;
        out += encode(item);
    }
    return out + "]";
}

std::string iso8601(telemetry::Timestamp t) {
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    const auto seconds = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    std::array<char, 32> buffer{};
    const std::size_t n = std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%S", &tm);
    const auto millis = static_cast<int>(ms % 1000);
    std::string out(buffer.data(), n);
    out += '.';
    out += static_cast<char>('0' + millis / 100);
    out += static_cast<char>('0' + (millis / 10) % 10);
    out += static_cast<char>('0' + millis % 10);
    out += 'Z';
    return out;
}

std::string value_json(double v) {
    return number_text(v);
}
std::string value_json(const Lsn& v) {
    return quote(collectors::to_string(v));
}
std::string value_json(bool v) {
    return v ? "true" : "false";
}
std::string value_json(const std::string& v) {
    return quote(v);
}

template <typename T>
std::string reading_json(const Reading<T>& r) {
    JsonObject o;
    o.str("q", collectors::to_string(r.quality()));
    if (r.value()) {
        o.raw("v", value_json(r.measured()));
    }
    if (!r.detail().empty()) {
        o.str("why", r.detail());
    }
    return o.done();
}

template <typename T>
std::string reading_text(const Reading<T>& r) {
    if (r.value()) {
        std::string s;
        if constexpr (std::is_same_v<T, double>) {
            s = number_text(std::round(r.measured() * 100.0) / 100.0);
        } else if constexpr (std::is_same_v<T, Lsn>) {
            s = collectors::to_string(r.measured());
        } else if constexpr (std::is_same_v<T, bool>) {
            s = r.measured() ? "true" : "false";
        } else {
            s = r.measured();
        }
        return r.quality() == Quality::kOk ? s
                                           : s + " (" + std::string(to_string(r.quality())) + ")";
    }
    return "<" + std::string(collectors::to_string(r.quality())) +
           (r.detail().empty() ? "" : ": " + r.detail()) + ">";
}

std::string rates_json(const collectors::PgRates& r) {
    return JsonObject{}
        .raw("interval_seconds", reading_json(r.interval_seconds))
        .raw("commits_per_second", reading_json(r.commits_per_second))
        .raw("rollbacks_per_second", reading_json(r.rollbacks_per_second))
        .raw("wal_bytes_per_second", reading_json(r.wal_bytes_per_second))
        .raw("mean_statement_latency_ms", reading_json(r.mean_statement_latency_ms))
        .done();
}

std::string error_json(const collectors::SqlError& e) {
    return JsonObject{}
        .str("kind", to_string(e.kind))
        .str("sqlstate", e.sqlstate)
        .str("message", e.message)
        .done();
}

std::string standby_json(const collectors::StandbyLink& l) {
    return JsonObject{}
        .str("application_name", l.application_name)
        .str("client_addr", l.client_addr)
        .str("state", l.state)
        .str("sync_state", l.sync_state)
        .raw("replay_lsn", reading_json(l.replay_lsn))
        .raw("replay_lag_bytes", reading_json(l.replay_lag_bytes))
        .raw("write_lag_seconds", reading_json(l.write_lag_seconds))
        .raw("flush_lag_seconds", reading_json(l.flush_lag_seconds))
        .raw("replay_lag_seconds", reading_json(l.replay_lag_seconds))
        .done();
}

std::string slot_json(const collectors::ReplicationSlot& slot) {
    return JsonObject{}
        .str("name", slot.name)
        .raw("active", value_json(slot.active))
        .str("wal_status", slot.wal_status)
        .raw("retained_bytes", reading_json(slot.retained_bytes))
        .done();
}

/// The node object without its closing brace, so per-cycle fields can be appended.
JsonObject node_json(const collectors::PgNodeSnapshot& n) {
    JsonObject metrics;
    collectors::for_each_reading(n, [&](std::string_view name, const auto& reading) {
        metrics.raw(name, reading_json(reading));
    });
    JsonObject o;
    o.str("node", n.node)
        .str("cluster_name", n.cluster_name)
        .str("availability", to_string(n.availability))
        .str("role", telemetry::to_string(n.role))
        .str("collected_at", iso8601(n.collected_at))
        .raw("errors", json_array(n.errors, error_json))
        .raw("metrics", metrics.done())
        .raw("standbys", json_array(n.standbys, standby_json))
        .raw("slots", json_array(n.slots, slot_json));
    return o;
}

std::string edge_json(const collectors::ReplicationEdge& e) {
    return JsonObject{}
        .str("upstream", e.upstream)
        .str("downstream", e.downstream)
        .str("state", e.state)
        .raw("replay_lag_bytes", reading_json(e.replay_lag_bytes))
        .raw("replay_lag_seconds", reading_json(e.replay_lag_seconds))
        .done();
}

std::string issue_json(const collectors::TopologyIssue& i) {
    return JsonObject{}
        .str("code", to_string(i.code))
        .str("node", i.node)
        .str("detail", i.detail)
        .done();
}

std::string resources_json(const collectors::ResourceSnapshot& r) {
    return JsonObject{}
        .str("collected_at", iso8601(r.collected_at))
        .raw("cpu_used_percent", reading_json(r.cpu_used_percent))
        .raw("memory_used_percent", reading_json(r.memory_used_percent))
        .raw("disk_used_percent", reading_json(r.disk_used_percent))
        .raw("load_average_1m", reading_json(r.load_average_1m))
        .done();
}

}  // namespace

std::string json_escape(std::string_view text) {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += R"(\n)";
        } else if (c == '\r') {
            out += R"(\r)";
        } else if (c == '\t') {
            out += R"(\t)";
        } else if (uc < 0x20U) {
            out += R"(\u00)";
            out += kHex[uc >> 4U];
            out += kHex[uc & 0x0FU];
        } else {
            out += c;
        }
    }
    return out;
}

std::optional<double> percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        return std::nullopt;
    }
    std::ranges::sort(values);
    const double rank = std::ceil(p / 100.0 * static_cast<double>(values.size()));
    const auto index = static_cast<std::size_t>(std::max(rank, 1.0)) - 1;
    return values[std::min(index, values.size() - 1)];
}

void accumulate(RunSummary& summary, const CycleRecord& record) {
    ++summary.cycles;
    summary.cycle_wall_ms.push_back(record.cycle_wall_ms);
    summary.client_cpu_ms.push_back(record.client_cpu_ms);
    for (std::size_t i = 0; i < record.nodes.size(); ++i) {
        const auto& node = record.nodes[i];
        ++summary.attempts;
        if (node.availability != collectors::Availability::kDown) {
            ++summary.successes;
        }
        const auto cov = collectors::coverage(node);
        summary.readings_ok += static_cast<std::uint64_t>(cov.ok);
        summary.readings_applicable += static_cast<std::uint64_t>(cov.applicable);
        if (i < record.node_wall_ms.size()) {
            summary.node_wall_ms.push_back(record.node_wall_ms[i]);
        }
        if (i < record.freshness_ms.size()) {
            summary.freshness_ms.push_back(record.freshness_ms[i]);
        }
    }
}

void write_cycle_json(std::ostream& out, const CycleRecord& record) {
    std::vector<std::string> nodes;
    for (std::size_t i = 0; i < record.nodes.size(); ++i) {
        JsonObject node = node_json(record.nodes[i]);
        node.num("collect_wall_ms", record.node_wall_ms.at(i))
            .num("freshness_ms", record.freshness_ms.at(i));
        if (const auto& rates = record.rates.at(i); rates.has_value()) {
            node.raw("rates", rates_json(*rates));
        } else {
            node.raw("rates", "null");
        }
        nodes.push_back(node.done());
    }
    const auto& t = record.topology;
    const std::string topology =
        JsonObject{}
            .raw("primary", t.primary ? quote(*t.primary) : "null")
            .raw("replicas", json_array(t.replicas, [](const std::string& r) { return quote(r); }))
            .raw("edges", json_array(t.edges, edge_json))
            .raw("issues", json_array(t.issues, issue_json))
            .done();
    out << JsonObject{}
               .str("type", "cycle")
               .num("cycle", record.cycle)
               .num("cycle_wall_ms", record.cycle_wall_ms)
               .num("client_cpu_ms", record.client_cpu_ms)
               .raw("nodes", json_array(nodes, [](const std::string& n) { return n; }))
               .raw("topology", topology)
               .raw("resources", record.resources ? resources_json(*record.resources) : "null")
               .done()
        << '\n';
}

void write_cycle_text(std::ostream& out, const CycleRecord& record) {
    out << "cycle " << record.cycle << "  (" << number_text(std::round(record.cycle_wall_ms))
        << " ms, client CPU " << number_text(std::round(record.client_cpu_ms * 10) / 10)
        << " ms)\n";
    for (std::size_t i = 0; i < record.nodes.size(); ++i) {
        const auto& n = record.nodes[i];
        out << "  " << n.node << ": " << to_string(n.availability) << ", "
            << telemetry::to_string(n.role) << "\n";
        for (const auto& e : n.errors) {
            out << "    error " << to_string(e.kind) << ": " << e.message << "\n";
        }
        if (n.availability == collectors::Availability::kDown) {
            continue;
        }
        out << "    probe " << reading_text(n.probe_latency_ms) << " ms, connections "
            << reading_text(n.connections.client_backends) << "/"
            << reading_text(n.connections.max_connections) << "\n";
        if (n.role == telemetry::NodeRole::kPrimary) {
            out << "    wal " << reading_text(n.current_wal_lsn) << ", standbys "
                << reading_text(n.standby_count) << "\n";
        } else if (n.role == telemetry::NodeRole::kReplica) {
            out << "    receiver " << reading_text(n.replica.receiver_status) << ", replay lag "
                << reading_text(n.replica.replay_lag_seconds) << " s, apply backlog "
                << reading_text(n.replica.apply_backlog_bytes) << " B\n";
        }
        if (const auto& rates = record.rates.at(i)) {
            out << "    commits/s " << reading_text(rates->commits_per_second) << ", WAL B/s "
                << reading_text(rates->wal_bytes_per_second) << ", mean statement "
                << reading_text(rates->mean_statement_latency_ms) << " ms\n";
        }
    }
    out << "  topology: primary "
        << (record.topology.primary ? *record.topology.primary : std::string("<none>"));
    for (const auto& e : record.topology.edges) {
        out << ", " << e.upstream << "->" << e.downstream << " " << e.state << " lag "
            << reading_text(e.replay_lag_bytes) << " B";
    }
    out << "\n";
    for (const auto& issue : record.topology.issues) {
        out << "  issue " << to_string(issue.code) << " " << issue.node
            << (issue.detail.empty() ? "" : " (" + issue.detail + ")") << "\n";
    }
    if (record.resources) {
        const auto& r = *record.resources;
        out << "  host: cpu " << reading_text(r.cpu_used_percent) << " %, memory "
            << reading_text(r.memory_used_percent) << " %, disk "
            << reading_text(r.disk_used_percent) << " %, load " << reading_text(r.load_average_1m)
            << "\n";
    }
}

namespace {

std::string stats_json(const std::vector<double>& values) {
    const auto opt = [](std::optional<double> v) {
        return v ? number_text(*v) : std::string("null");
    };
    double sum = 0.0;
    for (const double v : values) {
        sum += v;
    }
    return JsonObject{}
        .num("n", static_cast<double>(values.size()))
        .raw("mean",
             values.empty() ? "null" : number_text(sum / static_cast<double>(values.size())))
        .raw("p50", opt(percentile(values, 50)))
        .raw("p95", opt(percentile(values, 95)))
        .raw("p99", opt(percentile(values, 99)))
        .raw("max", opt(percentile(values, 100)))
        .done();
}

}  // namespace

void write_summary_json(std::ostream& out, const RunSummary& summary) {
    const auto ratio = [](std::uint64_t a, std::uint64_t b) {
        return b == 0 ? std::string("null")
                      : number_text(100.0 * static_cast<double>(a) / static_cast<double>(b));
    };
    const auto count = [](std::uint64_t v) { return std::to_string(v); };
    out << JsonObject{}
               .str("type", "summary")
               .raw("cycles", count(summary.cycles))
               .raw("attempts", count(summary.attempts))
               .raw("successes", count(summary.successes))
               .raw("collection_success_percent", ratio(summary.successes, summary.attempts))
               .raw("readings_ok", count(summary.readings_ok))
               .raw("readings_applicable", count(summary.readings_applicable))
               .raw("coverage_percent", ratio(summary.readings_ok, summary.readings_applicable))
               .raw("cycle_wall_ms", stats_json(summary.cycle_wall_ms))
               .raw("node_collect_wall_ms", stats_json(summary.node_wall_ms))
               .raw("client_cpu_ms_per_cycle", stats_json(summary.client_cpu_ms))
               .raw("freshness_ms", stats_json(summary.freshness_ms))
               .done()
        << '\n';
}

void write_summary_text(std::ostream& out, const RunSummary& summary) {
    const auto pct = [](std::uint64_t a, std::uint64_t b) {
        return b == 0 ? std::string("n/a")
                      : number_text(
                            std::round(1000.0 * static_cast<double>(a) / static_cast<double>(b)) /
                            10.0) +
                            " %";
    };
    const auto p = [](const std::vector<double>& v, double q) {
        const auto value = percentile(v, q);
        return value ? number_text(std::round(*value * 100.0) / 100.0) : std::string("n/a");
    };
    out << "summary: " << summary.cycles << " cycles, collection success "
        << pct(summary.successes, summary.attempts) << " (" << summary.successes << "/"
        << summary.attempts << "), coverage "
        << pct(summary.readings_ok, summary.readings_applicable) << "\n  cycle wall ms p50 "
        << p(summary.cycle_wall_ms, 50) << " p95 " << p(summary.cycle_wall_ms, 95)
        << ", client CPU ms/cycle p50 " << p(summary.client_cpu_ms, 50) << ", freshness ms p95 "
        << p(summary.freshness_ms, 95) << "\n";
}

}  // namespace faulttwin::monitor

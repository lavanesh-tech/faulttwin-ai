#include "faulttwin/detection/scenario.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace faulttwin::detection {

std::string_view to_string(FaultKind kind) noexcept {
    switch (kind) {
        case FaultKind::kReplicationLagBuildup:
            return "replication_lag_buildup";
        case FaultKind::kCpuSaturation:
            return "cpu_saturation";
        case FaultKind::kOverload:
            return "overload";
        case FaultKind::kNodeDown:
            return "node_down";
    }
    return "unknown";
}

std::string_view to_string(DistractorKind kind) noexcept {
    switch (kind) {
        case DistractorKind::kCpuSpike:
            return "cpu_spike";
        case DistractorKind::kLatencySpike:
            return "latency_spike";
        case DistractorKind::kTelemetryGap:
            return "telemetry_gap";
        case DistractorKind::kLagBlip:
            return "lag_blip";
        case DistractorKind::kBusyPeriod:
            return "busy_period";
    }
    return "unknown";
}

namespace {

using features::MetricId;
using features::Quality;

class SplitMix64 {
  public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    /// Uniform in [0, 1) with 53 random bits.
    double uniform() { return static_cast<double>(next() >> 11U) * 0x1.0p-53; }
    double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }
    /// Integer in [lo, hi].
    std::int64_t integer(std::int64_t lo, std::int64_t hi) {
        const auto span = static_cast<std::uint64_t>(hi - lo) + 1U;
        return lo + static_cast<std::int64_t>(next() % span);
    }
    /// Approximately standard normal: Irwin-Hall sum of 12 uniforms minus 6 (no libm).
    double normal() {
        double sum = 0.0;
        for (int i = 0; i < 12; ++i) {
            sum += uniform();
        }
        return sum - 6.0;
    }

  private:
    std::uint64_t state_;
};

double clamp(double v, double lo, double hi) {
    return std::min(std::max(v, lo), hi);
}

/// What is happening to one node at one tick.
struct Effects {
    bool silent{false};
    bool down{false};
    double cpu_override{-1.0};
    double latency_factor{1.0};
    double connection_target{-1.0};
    double lag_seconds{0.0};  ///< added replication lag
};

struct TimedEvent {
    std::size_t node{0};
    std::int64_t start_tick{0};
    std::int64_t end_tick{0};  ///< exclusive
    bool is_fault{true};
    FaultKind fault{FaultKind::kNodeDown};
    DistractorKind distractor{DistractorKind::kCpuSpike};
    double intensity{1.0};
    double rate{0.0};
    double level{0.0};
    bool silent{false};
};

/// Per-node smooth state (AR(1) processes), so values are autocorrelated like real metrics.
struct NodeSignals {
    double cpu{35.0};
    double connections{30.0};
    double latency{8.0};
    double lag{0.05};
    double lag_recovering{0.0};  ///< residual lag decaying after a lag fault ends
};

void apply_event(const TimedEvent& e, std::int64_t tick, Effects& fx) {
    const auto into = static_cast<double>(tick - e.start_tick);
    if (e.is_fault) {
        switch (e.fault) {
            case FaultKind::kReplicationLagBuildup:
                fx.lag_seconds += e.rate * (into + 1.0);
                break;
            case FaultKind::kCpuSaturation:
                fx.cpu_override = e.level;
                break;
            case FaultKind::kOverload:
                fx.latency_factor *= e.level;
                fx.connection_target = e.rate;
                break;
            case FaultKind::kNodeDown:
                fx.down = !e.silent;
                fx.silent = e.silent;
                break;
        }
        return;
    }
    switch (e.distractor) {
        case DistractorKind::kCpuSpike:
            fx.cpu_override = e.level;
            break;
        case DistractorKind::kLatencySpike:
            fx.latency_factor *= e.level;
            break;
        case DistractorKind::kTelemetryGap:
            fx.silent = true;
            break;
        case DistractorKind::kLagBlip:
            fx.lag_seconds += e.level;
            break;
        case DistractorKind::kBusyPeriod:
            fx.cpu_override = e.level;
            fx.latency_factor *= 1.6;
            fx.connection_target = e.rate;
            break;
    }
}

std::vector<TimedEvent> plan_events(const ScenarioConfig& config, std::size_t node_count,
                                    std::int64_t ticks, std::int64_t ticks_per_second,
                                    SplitMix64& rng) {
    const std::size_t total = config.faults + config.distractors;
    std::vector<TimedEvent> events;
    if (total == 0) {
        return events;
    }
    // Non-overlapping slots after a 60 s warm-up and before a 60 s tail, assigned in random
    // order, so the ground truth is never ambiguous.
    const std::int64_t margin = 60 * ticks_per_second;
    const std::int64_t usable = std::max<std::int64_t>(ticks - 2 * margin, 0);
    const std::int64_t slot = usable / static_cast<std::int64_t>(total);
    std::vector<std::size_t> order(total);
    for (std::size_t i = 0; i < total; ++i) {
        order[i] = i;
    }
    for (std::size_t i = total; i > 1; --i) {  // Fisher-Yates with our PRNG
        const auto j = static_cast<std::size_t>(rng.integer(0, static_cast<std::int64_t>(i) - 1));
        std::swap(order[i - 1], order[j]);
    }
    const auto seconds = [&](double s) {
        return static_cast<std::int64_t>(s * static_cast<double>(ticks_per_second));
    };
    const std::int64_t recovery_gap = seconds(40.0);

    for (std::size_t k = 0; k < total; ++k) {
        TimedEvent e;
        e.is_fault = k < config.faults;
        const std::int64_t slot_start = margin + static_cast<std::int64_t>(order[k]) * slot;
        const std::int64_t max_len = std::max<std::int64_t>(slot - recovery_gap, seconds(5.0));
        e.intensity = rng.uniform(config.intensity_min, config.intensity_max);
        std::int64_t len = 0;
        if (e.is_fault) {
            e.fault = static_cast<FaultKind>(rng.integer(0, kFaultKindCount - 1));
            const bool replica_only = e.fault == FaultKind::kReplicationLagBuildup;
            e.node = replica_only && node_count > 1
                         ? static_cast<std::size_t>(
                               rng.integer(1, static_cast<std::int64_t>(node_count) - 1))
                         : static_cast<std::size_t>(
                               rng.integer(0, static_cast<std::int64_t>(node_count) - 1));
            switch (e.fault) {
                case FaultKind::kReplicationLagBuildup:
                    // seconds of lag added per second
                    e.rate = rng.uniform(0.1, 1.0) * e.intensity;
                    len = seconds(rng.uniform(30.0, 70.0));
                    break;
                case FaultKind::kCpuSaturation:
                    e.level = clamp(70.0 + 25.0 * e.intensity, 0.0, 99.5);
                    len = seconds(rng.uniform(30.0, 70.0));
                    break;
                case FaultKind::kOverload:
                    e.level = 1.5 + 4.0 * e.intensity;                     // latency multiplier
                    e.rate = clamp(45.0 + 30.0 * e.intensity, 0.0, 98.0);  // connections %
                    len = seconds(rng.uniform(30.0, 70.0));
                    break;
                case FaultKind::kNodeDown:
                    e.silent = rng.uniform() < 0.5;
                    len = seconds(rng.uniform(4.0, 60.0));
                    break;
            }
        } else {
            e.distractor = static_cast<DistractorKind>(rng.integer(0, kDistractorKindCount - 1));
            const bool replica_only = e.distractor == DistractorKind::kLagBlip;
            e.node = replica_only && node_count > 1
                         ? static_cast<std::size_t>(
                               rng.integer(1, static_cast<std::int64_t>(node_count) - 1))
                         : static_cast<std::size_t>(
                               rng.integer(0, static_cast<std::int64_t>(node_count) - 1));
            switch (e.distractor) {
                case DistractorKind::kCpuSpike:
                    e.level = rng.uniform(90.0, 99.0);
                    len = seconds(rng.uniform(3.0, 12.0));
                    break;
                case DistractorKind::kLatencySpike:
                    e.level = rng.uniform(4.0, 12.0);
                    len = seconds(rng.uniform(3.0, 12.0));
                    break;
                case DistractorKind::kTelemetryGap:
                    len = seconds(rng.uniform(1.0, 3.5));
                    break;
                case DistractorKind::kLagBlip:
                    e.level = rng.uniform(5.0, 15.0);
                    len = seconds(rng.uniform(1.0, 4.0));
                    break;
                case DistractorKind::kBusyPeriod:
                    e.level = rng.uniform(65.0, 82.0);  // CPU
                    e.rate = rng.uniform(45.0, 62.0);   // connections %
                    len = seconds(rng.uniform(30.0, 60.0));
                    break;
            }
        }
        len = std::clamp<std::int64_t>(len, 1, max_len);
        e.start_tick =
            slot_start + rng.integer(0, std::max<std::int64_t>(slot - len - recovery_gap, 0));
        e.end_tick = e.start_tick + len;
        events.push_back(e);
    }
    std::sort(events.begin(), events.end(),
              [](const TimedEvent& a, const TimedEvent& b) { return a.start_tick < b.start_tick; });
    return events;
}

void put(std::vector<Observation>& out, const std::string& node, MetricId metric, Timestamp at,
         double value) {
    out.push_back(Observation::ok(node, metric, at, value));
}

void put_quality(std::vector<Observation>& out, const std::string& node, MetricId metric,
                 Timestamp at, Quality quality) {
    out.push_back(Observation::missing(node, metric, at, quality));
}

void emit_lag(std::vector<Observation>& out, const std::string& node, bool is_primary,
              bool primary_down, const Effects& f, NodeSignals& sig, Timestamp at) {
    if (is_primary) {
        put_quality(out, node, MetricId::kReplicationLagSeconds, at, Quality::kNotApplicable);
        put_quality(out, node, MetricId::kReplicationLagBytes, at, Quality::kNotApplicable);
        return;
    }
    if (primary_down) {
        put_quality(out, node, MetricId::kReplicationLagSeconds, at, Quality::kMissing);
        put_quality(out, node, MetricId::kReplicationLagBytes, at, Quality::kMissing);
        return;
    }
    sig.lag_recovering = std::max(sig.lag_recovering, f.lag_seconds);
    const double lag = sig.lag + std::max(f.lag_seconds, sig.lag_recovering);
    put(out, node, MetricId::kReplicationLagSeconds, at, lag);
    put(out, node, MetricId::kReplicationLagBytes, at, lag * 2.0e6);
}

/// Advances one node's signals by one tick and appends its observations.
void emit_node(std::vector<Observation>& out, const std::string& node, bool is_primary,
               bool primary_down, const Effects& f, NodeSignals& sig, SplitMix64& rng,
               Timestamp at) {
    // Smooth baselines evolve every tick, even while the node is down, and every tick draws
    // the same number of random values, so one node's faults never shift another's noise.
    sig.cpu = clamp(0.85 * sig.cpu + 0.15 * 35.0 + 2.5 * rng.normal(), 1.0, 85.0);
    sig.connections = clamp(0.9 * sig.connections + 0.1 * 30.0 + 1.5 * rng.normal(), 1.0, 55.0);
    sig.latency = clamp(0.8 * sig.latency + 0.2 * 8.0 + 1.2 * rng.normal(), 1.0, 25.0);
    sig.lag = clamp(0.7 * sig.lag + 0.3 * 0.05 + 0.03 * rng.normal(), 0.0, 0.6);
    sig.lag_recovering *= 0.85;
    const double probe_noise = rng.normal();
    const double commits_noise = rng.normal();
    const double memory_noise = rng.normal();
    const double cpu_noise = rng.normal();
    const double connection_noise = rng.normal();

    if (f.silent) {
        return;
    }
    if (f.down) {
        put_quality(out, node, MetricId::kAvailability, at, Quality::kMissing);
        for (const MetricId m :
             {MetricId::kProbeLatencyMs, MetricId::kConnectionUtilization,
              MetricId::kCommitsPerSecond, MetricId::kStatementLatencyMs, MetricId::kCpuPercent,
              MetricId::kMemoryPercent, MetricId::kReplicationLagSeconds,
              MetricId::kReplicationLagBytes}) {
            put_quality(out, node, m, at, Quality::kMissing);
        }
        return;
    }
    const double cpu =
        f.cpu_override >= 0.0 ? clamp(f.cpu_override + 2.5 * cpu_noise, 0.0, 100.0) : sig.cpu;
    const double connections = f.connection_target >= 0.0
                                   ? clamp(f.connection_target + 2.0 * connection_noise, 0.0, 100.0)
                                   : sig.connections;
    put(out, node, MetricId::kAvailability, at, 1.0);
    put(out, node, MetricId::kProbeLatencyMs, at, clamp(0.5 + 0.08 * probe_noise, 0.1, 5.0));
    put(out, node, MetricId::kConnectionUtilization, at, connections);
    if (is_primary) {
        put(out, node, MetricId::kCommitsPerSecond, at,
            clamp(400.0 + 40.0 * commits_noise, 0.0, 1e6));
    } else {
        put_quality(out, node, MetricId::kCommitsPerSecond, at, Quality::kNotApplicable);
    }
    put(out, node, MetricId::kStatementLatencyMs, at, sig.latency * f.latency_factor);
    put(out, node, MetricId::kCpuPercent, at, cpu);
    put(out, node, MetricId::kMemoryPercent, at, clamp(55.0 + 1.5 * memory_noise, 0.0, 100.0));
    emit_lag(out, node, is_primary, primary_down, f, sig, at);
}

}  // namespace

Scenario generate_scenario(const ScenarioConfig& config) {
    Scenario s;
    s.config = config;
    const std::size_t node_count = 1U + config.replicas;
    s.nodes.emplace_back("primary");
    for (std::uint32_t r = 1; r <= config.replicas; ++r) {
        s.nodes.push_back("replica-" + std::to_string(r));
    }

    SplitMix64 rng{config.seed};
    const auto tick = config.tick.count() > 0 ? config.tick : std::chrono::milliseconds{1000};
    const std::int64_t ticks_per_second = std::max<std::int64_t>(1000 / tick.count(), 1);
    const std::int64_t ticks =
        std::chrono::duration_cast<std::chrono::milliseconds>(config.duration).count() /
        tick.count();
    const auto at_tick = [&](std::int64_t t) { return config.start + tick * t; };

    const auto events = plan_events(config, node_count, ticks, ticks_per_second, rng);
    for (const auto& e : events) {
        if (e.is_fault) {
            s.episodes.push_back(GroundTruthEpisode{s.nodes[e.node], e.fault, at_tick(e.start_tick),
                                                    at_tick(e.end_tick), e.intensity});
        } else {
            s.distractors.push_back(Distractor{s.nodes[e.node], e.distractor, at_tick(e.start_tick),
                                               at_tick(e.end_tick)});
        }
    }

    std::vector<NodeSignals> signals(node_count);
    s.observations.reserve(static_cast<std::size_t>(ticks) * node_count * 8U);
    for (std::int64_t t = 0; t < ticks; ++t) {
        std::vector<Effects> fx(node_count);
        for (const auto& e : events) {
            if (t >= e.start_tick && t < e.end_tick) {
                apply_event(e, t, fx[e.node]);
            }
        }
        const bool primary_down = fx[0].down || fx[0].silent;
        for (std::size_t n = 0; n < node_count; ++n) {
            emit_node(s.observations, s.nodes[n], n == 0, primary_down, fx[n], signals[n], rng,
                      at_tick(t));
        }
    }
    return s;
}

}  // namespace faulttwin::detection

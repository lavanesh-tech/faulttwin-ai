#pragma once

#include <chrono>
#include <string>

#include "faulttwin/telemetry/node_id.hpp"
#include "faulttwin/telemetry/telemetry_sample.hpp"

namespace faulttwin::ingest::testing {

/// Fixed reference time so that validation is deterministic in tests.
inline telemetry::Timestamp test_now() {
    return telemetry::Timestamp{std::chrono::sys_days{std::chrono::year{2026} / 6 / 1}};
}

inline telemetry::RawTelemetrySample valid_raw(const std::string& node, int seconds_before_now) {
    telemetry::RawTelemetrySample raw;
    raw.node_id = node;
    raw.role = telemetry::NodeRole::kReplica;
    raw.collected_at = test_now() - std::chrono::seconds{seconds_before_now};
    raw.cpu_usage_percent = 10.0;
    raw.memory_usage_percent = 20.0;
    raw.disk_usage_percent = 30.0;
    raw.replication_lag_ms = 5.0;
    return raw;
}

inline telemetry::RawTelemetrySample invalid_raw() {
    telemetry::RawTelemetrySample raw = valid_raw("pg-replica-1", 1);
    raw.cpu_usage_percent = 150.0;
    return raw;
}

inline telemetry::NodeId node(const std::string& text) {
    return telemetry::NodeId::parse(text).value();
}

}  // namespace faulttwin::ingest::testing

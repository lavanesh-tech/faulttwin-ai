#pragma once

#include <vector>

#include "faulttwin/ingest/event_processor.hpp"
#include "faulttwin/telemetry/telemetry_sample.hpp"

namespace faulttwin::ingest {

/// Single-threaded baseline for Experiment 1 (docs/evaluation/baselines.md):
/// the same EventProcessor work (validate, order check, policy, sink) in a plain
/// loop on the calling thread, with no queue and no extra threads. Each event
/// gets a sequence number and a submitted_at time exactly as submit() does, so
/// the per-event work matches the pipeline.
///
/// Precondition: processor.workers() >= 1; worker index 0 is used.
/// Returns the processor's counters after the run.
ProcessingCounters run_single_threaded_baseline(std::vector<telemetry::RawTelemetrySample> samples,
                                                EventProcessor& processor);

}  // namespace faulttwin::ingest

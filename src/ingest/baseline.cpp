#include "faulttwin/ingest/baseline.hpp"

#include <cstdint>
#include <utility>

namespace faulttwin::ingest {

ProcessingCounters run_single_threaded_baseline(std::vector<telemetry::RawTelemetrySample> samples,
                                                EventProcessor& processor) {
    std::uint64_t sequence = 0;
    for (telemetry::RawTelemetrySample& raw : samples) {
        const IngestEvent event{
            .sequence = sequence++, .raw = std::move(raw), .submitted_at = SteadyClock::now()};
        processor.process(0, event);
    }
    return processor.counters();
}

}  // namespace faulttwin::ingest

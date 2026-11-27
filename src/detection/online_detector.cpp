#include "faulttwin/detection/online_detector.hpp"

#include <utility>
#include <variant>

#include "faulttwin/features/adapters.hpp"

namespace faulttwin::detection {

OnlineDetector::OnlineDetector(RuleSet rules, features::FeatureSchema schema)
    : extractor_(schema), engine_(std::move(rules), std::move(schema)) {}

std::vector<Detection> OnlineDetector::on_cycle(
    std::span<const collectors::PgNodeSnapshot> nodes,
    std::span<const std::optional<collectors::PgRates>> rates) {
    const auto observations = features::observations_from_cycle(nodes, rates);
    ++counters_.cycles;
    feed(observations);
    // Each node is evaluated as of its own snapshot's event time (collected_at, or
    // attempted_at when down). Using the newest time of the whole cycle instead would make a
    // node collected early look "old" whenever a later node is slow (e.g. a frozen replica
    // costing a 2 s deadline), and the stale-telemetry rule would fire on healthy nodes.
    std::vector<Detection> out;
    for (const auto& node : nodes) {
        const Timestamp as_of = node.availability == collectors::Availability::kDown
                                    ? node.attempted_at
                                    : node.collected_at;
        evaluate(node.node, as_of, out);
    }
    return out;
}

void OnlineDetector::feed(const std::vector<features::Observation>& observations) {
    for (const auto& o : observations) {
        ++counters_.observations;
        const auto result = extractor_.observe(o);
        if (result != features::ObserveResult::kAccepted &&
            result != features::ObserveResult::kNotApplicable &&
            result != features::ObserveResult::kNotInSchema) {
            ++counters_.observations_refused;
        }
    }
}

void OnlineDetector::evaluate(const std::string& node, Timestamp as_of,
                              std::vector<Detection>& out) {
    auto result = engine_.evaluate(extractor_.extract(node, as_of));
    if (auto* detections = std::get_if<std::vector<Detection>>(&result)) {
        for (auto& d : *detections) {
            out.push_back(std::move(d));
        }
    } else {
        ++counters_.vectors_refused;
    }
}

std::vector<Detection> OnlineDetector::on_observations(
    const std::vector<features::Observation>& observations, Timestamp as_of) {
    ++counters_.cycles;
    feed(observations);
    std::vector<Detection> out;
    for (const auto& node : extractor_.nodes()) {
        evaluate(node, as_of, out);
    }
    return out;
}

}  // namespace faulttwin::detection

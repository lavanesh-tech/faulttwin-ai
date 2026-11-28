#include "faulttwin/storage/incident_recorder.hpp"

namespace faulttwin::storage {

namespace {
void bump(observability::Counter* counter) {
    if (counter != nullptr) {
        counter->inc();
    }
}
}  // namespace

IncidentRecorder::IncidentRecorder(IncidentStore& store, core::Logger& logger,
                                   std::string feature_schema_version, std::size_t max_pending,
                                   observability::Registry* metrics)
    : store_(&store),
      logger_(&logger),
      feature_schema_version_(std::move(feature_schema_version)),
      max_pending_(max_pending) {
    if (metrics != nullptr) {
        const std::string ops = "faulttwin_incident_operations_total";
        const std::string help = "Incident store operations applied by the recorder, by result";
        metrics_.opened = &metrics->counter(ops, help, {"op"}, {"open"});
        metrics_.escalated = &metrics->counter(ops, help, {"op"}, {"escalate"});
        metrics_.resolved = &metrics->counter(ops, help, {"op"}, {"resolve"});
        metrics_.dropped = &metrics->counter(
            "faulttwin_incident_detections_dropped_total",
            "Detections dropped because the recorder's retry queue was full", {}, {});
        metrics_.rejected = &metrics->counter(
            "faulttwin_incident_detections_rejected_total",
            "Detections the store refused permanently (conflict, invalid, capacity, unmatched)", {},
            {});
        metrics_.retries = &metrics->counter(
            "faulttwin_incident_store_retries_total",
            "Flushes that stopped because the incident store was unavailable", {}, {});
        metrics_.pending = &metrics->gauge("faulttwin_incident_recorder_pending",
                                           "Detections queued for the incident store", {}, {});
    }
}

void IncidentRecorder::start(Timestamp now) {
    start_time_ = now;
    auto result = store_->close_orphans(now);
    if (const auto* closed = std::get_if<std::uint64_t>(&result)) {
        started_ = true;
        counters_.orphans_closed += *closed;
        if (*closed > 0) {
            logger_->warn("incidents", "closed incidents left open by a previous run",
                          {{"count", *closed}, {"resolution", "monitor_restarted"}});
        }
        return;
    }
    const auto& error = std::get<StoreError>(result);
    logger_->error("incidents", "could not close orphaned incidents; will retry",
                   {{"error", std::string(to_string(error.kind))}, {"detail", error.message}});
}

void IncidentRecorder::record(const std::vector<detection::Detection>& detections) {
    for (const auto& d : detections) {
        if (queue_.size() >= max_pending_) {
            ++counters_.dropped;
            bump(metrics_.dropped);
            logger_->error("incidents", "retry queue full; detection dropped",
                           {{"rule", d.rule_id},
                            {"node", d.node},
                            {"kind", std::string(detection::to_string(d.kind))},
                            {"max_pending", static_cast<std::uint64_t>(max_pending_)}});
            continue;
        }
        queue_.push_back(d);
    }
    flush();
}

void IncidentRecorder::flush() {
    if (!started_) {
        start(start_time_);
        if (!started_) {
            ++counters_.retries;
            bump(metrics_.retries);
            update_gauges();
            return;
        }
    }
    while (!queue_.empty()) {
        if (apply(queue_.front()) == Outcome::kRetryLater) {
            ++counters_.retries;
            bump(metrics_.retries);
            break;
        }
        queue_.pop_front();
    }
    update_gauges();
}

void IncidentRecorder::update_gauges() {
    if (metrics_.pending != nullptr) {
        metrics_.pending->set(static_cast<double>(queue_.size()));
    }
}

IncidentRecorder::Outcome IncidentRecorder::on_error(const detection::Detection& d,
                                                     const StoreError& error) {
    if (error.kind == StoreErrorKind::kUnavailable) {
        logger_->warn("incidents", "incident store unavailable; detection kept for retry",
                      {{"rule", d.rule_id}, {"node", d.node}, {"detail", error.message}});
        return Outcome::kRetryLater;
    }
    ++counters_.rejected;
    bump(metrics_.rejected);
    logger_->error("incidents", "incident store refused a detection",
                   {{"rule", d.rule_id},
                    {"node", d.node},
                    {"kind", std::string(detection::to_string(d.kind))},
                    {"error", std::string(to_string(error.kind))},
                    {"detail", error.message}});
    return Outcome::kSkipped;
}

IncidentRecorder::Outcome IncidentRecorder::apply(const detection::Detection& d) {
    auto key = std::make_pair(d.rule_id, d.node);
    if (d.kind == detection::DetectionKind::kFired) {
        const NewIncident incident{
            d.rule_set_version, feature_schema_version_, d.rule_id, d.node,
            d.severity,         d.violation_since,       d.at,      d.evidence};
        auto result = store_->open(incident);
        if (const auto* id = std::get_if<std::uint64_t>(&result)) {
            open_ids_[key] = *id;
            ++counters_.opened;
            bump(metrics_.opened);
            logger_->info("incidents", "incident opened",
                          {{"incident_id", *id},
                           {"rule", d.rule_id},
                           {"node", d.node},
                           {"severity", std::string(detection::to_string(d.severity))}});
            return Outcome::kApplied;
        }
        return on_error(d, std::get<StoreError>(result));
    }

    const auto it = open_ids_.find(key);
    if (it == open_ids_.end()) {
        // Its open was dropped or refused; there is nothing to attach this change to.
        ++counters_.unmatched;
        ++counters_.rejected;
        bump(metrics_.rejected);
        logger_->error("incidents", "state change without an open incident",
                       {{"rule", d.rule_id},
                        {"node", d.node},
                        {"kind", std::string(detection::to_string(d.kind))}});
        return Outcome::kSkipped;
    }
    const IncidentEvent event{d.kind, d.severity, d.at, d.evidence};
    const std::uint64_t id = it->second;
    if (d.kind == detection::DetectionKind::kEscalated) {
        auto result = store_->escalate(id, event);
        if (std::holds_alternative<Done>(result)) {
            ++counters_.escalated;
            bump(metrics_.escalated);
            logger_->warn("incidents", "incident escalated",
                          {{"incident_id", id}, {"rule", d.rule_id}, {"node", d.node}});
            return Outcome::kApplied;
        }
        return on_error(d, std::get<StoreError>(result));
    }
    auto result = store_->resolve(id, event, Resolution::kRecovered);
    if (std::holds_alternative<Done>(result)) {
        open_ids_.erase(it);
        ++counters_.resolved;
        bump(metrics_.resolved);
        logger_->info("incidents", "incident resolved",
                      {{"incident_id", id}, {"rule", d.rule_id}, {"node", d.node}});
        return Outcome::kApplied;
    }
    const auto& error = std::get<StoreError>(result);
    if (error.kind != StoreErrorKind::kUnavailable) {
        open_ids_.erase(it);
    }
    return on_error(d, error);
}

}  // namespace faulttwin::storage

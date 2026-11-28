#include "faulttwin/storage/memory_incident_store.hpp"

#include <algorithm>
#include <mutex>

namespace faulttwin::storage {

namespace {

StoreError not_found(std::uint64_t id) {
    return {StoreErrorKind::kNotFound, "incident " + std::to_string(id) + " not found"};
}

StoreError resolved_conflict(std::uint64_t id) {
    return {StoreErrorKind::kConflict, "incident " + std::to_string(id) + " is already resolved"};
}

}  // namespace

bool matches(const Incident& incident, const IncidentQuery& query) {
    return (!query.state || incident.state == *query.state) &&
           (!query.node || incident.node == *query.node) &&
           (!query.rule_id || incident.rule_id == *query.rule_id) &&
           (!query.severity || incident.severity == *query.severity) &&
           (!query.opened_since || incident.opened_at >= *query.opened_since) &&
           (!query.opened_until || incident.opened_at < *query.opened_until);
}

MemoryIncidentStore::MemoryIncidentStore(std::size_t capacity)
    : capacity_(std::max<std::size_t>(capacity, 1)) {}

StoreResult<std::uint64_t> MemoryIncidentStore::open(const NewIncident& incident) {
    if (auto problem = validate_new_incident(incident); !problem.empty()) {
        return StoreError{StoreErrorKind::kInvalid, problem};
    }
    const std::unique_lock lock(mutex_);
    auto key = std::make_pair(incident.rule_id, incident.node);
    if (open_by_key_.contains(key)) {
        return StoreError{StoreErrorKind::kConflict, "an incident for rule " + incident.rule_id +
                                                         " on node " + incident.node +
                                                         " is already open"};
    }
    if (incidents_.size() >= capacity_) {
        const auto victim = std::find_if(incidents_.begin(), incidents_.end(), [](const auto& e) {
            return e.second.state == IncidentState::kResolved;
        });
        if (victim == incidents_.end()) {
            return StoreError{StoreErrorKind::kCapacity,
                              "store holds " + std::to_string(capacity_) + " open incidents"};
        }
        incidents_.erase(victim);
        ++evicted_;
    }
    Incident created;
    created.id = next_id_++;
    created.rule_set_version = incident.rule_set_version;
    created.feature_schema_version = incident.feature_schema_version;
    created.rule_id = incident.rule_id;
    created.node = incident.node;
    created.severity = incident.severity;
    created.violation_since = incident.violation_since;
    created.opened_at = incident.opened_at;
    created.updated_at = incident.opened_at;
    created.events.push_back(
        {DetectionKind::kFired, incident.severity, incident.opened_at, incident.evidence});
    const auto id = created.id;
    incidents_.emplace(id, std::move(created));
    open_by_key_.emplace(std::move(key), id);
    return id;
}

StoreResult<Done> MemoryIncidentStore::escalate(std::uint64_t id, const IncidentEvent& event) {
    const std::unique_lock lock(mutex_);
    const auto it = incidents_.find(id);
    if (it == incidents_.end()) {
        return not_found(id);
    }
    Incident& incident = it->second;
    if (incident.state != IncidentState::kOpen) {
        return resolved_conflict(id);
    }
    if (incident.events.size() >= kMaxEventsPerIncident) {
        return StoreError{StoreErrorKind::kConflict, "incident has the maximum number of events"};
    }
    incident.severity = std::max(incident.severity, event.severity);
    incident.updated_at = event.at;
    incident.events.push_back(event);
    return Done{};
}

StoreResult<Done> MemoryIncidentStore::resolve(std::uint64_t id, const IncidentEvent& event,
                                               Resolution resolution) {
    const std::unique_lock lock(mutex_);
    const auto it = incidents_.find(id);
    if (it == incidents_.end()) {
        return not_found(id);
    }
    Incident& incident = it->second;
    if (incident.state != IncidentState::kOpen) {
        return resolved_conflict(id);
    }
    incident.state = IncidentState::kResolved;
    incident.resolution = resolution;
    incident.resolved_at = event.at;
    incident.updated_at = event.at;
    if (incident.events.size() < kMaxEventsPerIncident) {
        incident.events.push_back(event);
    }
    open_by_key_.erase({incident.rule_id, incident.node});
    return Done{};
}

StoreResult<std::uint64_t> MemoryIncidentStore::close_orphans(Timestamp at) {
    const std::unique_lock lock(mutex_);
    std::uint64_t closed = 0;
    for (auto& [id, incident] : incidents_) {
        if (incident.state == IncidentState::kOpen) {
            incident.state = IncidentState::kResolved;
            incident.resolution = Resolution::kMonitorRestarted;
            incident.resolved_at = at;
            incident.updated_at = at;
            ++closed;
        }
    }
    open_by_key_.clear();
    return closed;
}

StoreResult<Incident> MemoryIncidentStore::acknowledge(std::uint64_t id,
                                                       const Acknowledgement& ack) {
    if (auto problem = validate_acknowledgement(ack); !problem.empty()) {
        return StoreError{StoreErrorKind::kInvalid, problem};
    }
    const std::unique_lock lock(mutex_);
    const auto it = incidents_.find(id);
    if (it == incidents_.end()) {
        return not_found(id);
    }
    if (it->second.acknowledgement) {
        return StoreError{StoreErrorKind::kConflict,
                          "incident " + std::to_string(id) + " is already acknowledged"};
    }
    it->second.acknowledgement = ack;
    return it->second;
}

StoreResult<Incident> MemoryIncidentStore::get(std::uint64_t id) {
    const std::shared_lock lock(mutex_);
    const auto it = incidents_.find(id);
    if (it == incidents_.end()) {
        return not_found(id);
    }
    return it->second;
}

StoreResult<IncidentPage> MemoryIncidentStore::list(const IncidentQuery& query) {
    if (auto problem = validate_query(query); !problem.empty()) {
        return StoreError{StoreErrorKind::kInvalid, problem};
    }
    const std::shared_lock lock(mutex_);
    IncidentPage page;
    // Walk ids from newest to oldest, starting below the cursor. O(n) in the worst case
    // (a filter that matches nothing); n is bounded by the capacity.
    auto it = query.before_id ? incidents_.lower_bound(*query.before_id) : incidents_.end();
    while (it != incidents_.begin()) {
        --it;
        if (!matches(it->second, query)) {
            continue;
        }
        if (page.items.size() == query.limit) {
            page.next_before_id = page.items.back().id;
            break;
        }
        page.items.push_back(it->second);
    }
    return page;
}

StoreResult<StoreStats> MemoryIncidentStore::stats() {
    const std::shared_lock lock(mutex_);
    StoreStats s;
    s.open = open_by_key_.size();
    s.resolved = incidents_.size() - open_by_key_.size();
    return s;
}

std::uint64_t MemoryIncidentStore::evicted() const {
    const std::shared_lock lock(mutex_);
    return evicted_;
}

}  // namespace faulttwin::storage

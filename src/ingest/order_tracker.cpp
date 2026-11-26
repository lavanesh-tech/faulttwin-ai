#include "faulttwin/ingest/order_tracker.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <utility>

namespace faulttwin::ingest {

std::string_view to_string(OrderVerdict verdict) noexcept {
    switch (verdict) {
        case OrderVerdict::kInOrder:
            return "in-order";
        case OrderVerdict::kOutOfOrder:
            return "out-of-order";
        case OrderVerdict::kDuplicate:
            return "duplicate";
        case OrderVerdict::kNodeLimitExceeded:
            return "node-limit-exceeded";
    }
    return "invalid";
}

namespace {

const OrderTrackerConfig& checked(const OrderTrackerConfig& config) {
    if (config.recent_window == 0 || config.max_nodes == 0 || config.shards == 0) {
        throw std::invalid_argument("OrderTrackerConfig fields must all be at least 1");
    }
    return config;
}

}  // namespace

NodeOrderTracker::NodeOrderTracker(const OrderTrackerConfig& config)
    : config_(checked(config)),
      shards_(std::make_unique<Shard[]>(config.shards)) {}  // NOLINT(*-avoid-c-arrays)

bool NodeOrderTracker::reserve_node_slot() noexcept {
    std::size_t current = node_count_.load(std::memory_order_relaxed);
    while (current < config_.max_nodes) {
        if (node_count_.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void NodeOrderTracker::remember(NodeState& state, telemetry::Timestamp collected_at) const {
    if (state.recent.size() < config_.recent_window) {
        state.recent.push_back(collected_at);
        return;
    }
    state.recent[state.next] = collected_at;
    state.next = (state.next + 1) % config_.recent_window;
}

OrderVerdict NodeOrderTracker::observe(const telemetry::NodeId& node,
                                       telemetry::Timestamp collected_at) {
    const std::string& key = node.value();
    Shard& shard = shards_[std::hash<std::string>{}(key) % config_.shards];
    const std::scoped_lock lock(shard.mutex);

    const auto it = shard.nodes.find(key);
    if (it == shard.nodes.end()) {
        if (!reserve_node_slot()) {
            return OrderVerdict::kNodeLimitExceeded;
        }
        NodeState state{.newest = collected_at, .recent = {}, .next = 0};
        state.recent.reserve(std::min<std::size_t>(config_.recent_window, 16));
        state.recent.push_back(collected_at);
        shard.nodes.emplace(key, std::move(state));
        return OrderVerdict::kInOrder;
    }

    NodeState& state = it->second;
    // The newest time is checked explicitly because the ring may have evicted it.
    if (collected_at == state.newest ||
        std::find(state.recent.begin(), state.recent.end(), collected_at) != state.recent.end()) {
        return OrderVerdict::kDuplicate;
    }
    remember(state, collected_at);
    if (collected_at < state.newest) {
        return OrderVerdict::kOutOfOrder;
    }
    state.newest = collected_at;
    return OrderVerdict::kInOrder;
}

}  // namespace faulttwin::ingest

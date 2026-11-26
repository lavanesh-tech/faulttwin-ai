#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "faulttwin/telemetry/node_id.hpp"
#include "faulttwin/telemetry/telemetry_sample.hpp"

namespace faulttwin::ingest {

/// How a sample's collection time relates to what was already seen for its node.
enum class OrderVerdict : std::uint8_t {
    kInOrder,            ///< newer than every sample seen for this node
    kOutOfOrder,         ///< older than the newest sample seen, and not a known duplicate
    kDuplicate,          ///< same node and collected_at as a recently seen sample
    kNodeLimitExceeded,  ///< a new node, but max_nodes are already tracked
};

[[nodiscard]] std::string_view to_string(OrderVerdict verdict) noexcept;

struct OrderTrackerConfig {
    /// Collection times remembered per node for duplicate detection. A duplicate
    /// of a sample older than the last `recent_window` samples is reported as
    /// kOutOfOrder instead (bounded memory; documented limitation).
    std::size_t recent_window{64};
    /// Upper bound on tracked nodes, so hostile or buggy collectors that invent
    /// node ids cannot grow memory without limit.
    std::size_t max_nodes{1024};
    /// Independent lock stripes; nodes hash to a stripe.
    std::size_t shards{16};
};

/// Thread-safe per-node ordering and duplicate detector.
///
/// Identity of a sample is (node id, collected_at). State is striped across
/// `shards` mutexes so consumers working on different nodes rarely contend.
/// Memory is bounded by max_nodes x recent_window timestamps.
///
/// With several consumers, samples of one node can be *processed* in a
/// different order than they were *queued*, so kOutOfOrder can be caused by
/// the pipeline itself and not only by the source. See ADR-0012.
class NodeOrderTracker {
  public:
    /// Throws std::invalid_argument if any config field is 0.
    explicit NodeOrderTracker(const OrderTrackerConfig& config = {});

    [[nodiscard]] OrderVerdict observe(const telemetry::NodeId& node,
                                       telemetry::Timestamp collected_at);

    [[nodiscard]] std::size_t tracked_nodes() const noexcept {
        return node_count_.load(std::memory_order_relaxed);
    }

  private:
    struct NodeState {
        telemetry::Timestamp newest;
        std::vector<telemetry::Timestamp> recent;  ///< ring buffer, size <= recent_window
        std::size_t next{0};                       ///< ring write position once full
    };

    struct Shard {
        std::mutex mutex;
        std::unordered_map<std::string, NodeState> nodes;
    };

    bool reserve_node_slot() noexcept;
    void remember(NodeState& state, telemetry::Timestamp collected_at) const;

    OrderTrackerConfig config_;
    std::unique_ptr<Shard[]> shards_;  // NOLINT(*-avoid-c-arrays): fixed count, mutex not movable
    std::atomic<std::size_t> node_count_{0};
};

}  // namespace faulttwin::ingest

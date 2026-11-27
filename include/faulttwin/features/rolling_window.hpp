#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "faulttwin/telemetry/telemetry_sample.hpp"

namespace faulttwin::features {

using telemetry::Timestamp;

/// Running mean and variance with Welford's update, plus the inverse update for removing a
/// value. Adding is numerically stable; repeated removal can accumulate rounding error, which
/// RollingWindow bounds by re-seeding from its buffer (see there).
class WelfordAccumulator {
  public:
    void add(double x) noexcept;
    /// Removes a value that was previously added. Precondition: count() > 0.
    void remove(double x) noexcept;
    void reset() noexcept { *this = WelfordAccumulator{}; }

    [[nodiscard]] std::size_t count() const noexcept { return count_; }
    [[nodiscard]] double mean() const noexcept { return mean_; }
    /// Sum of squared deviations from the mean (M2).
    [[nodiscard]] double m2() const noexcept { return m2_; }
    /// Sample variance (n - 1 denominator); nullopt for fewer than two values.
    [[nodiscard]] std::optional<double> sample_variance() const noexcept;

  private:
    std::size_t count_{0};
    double mean_{0.0};
    double m2_{0.0};
};

/// Outcome of offering one entry to a window. Nothing is dropped silently: every refusal has
/// its own code, and RollingWindow::Counters counts each one.
enum class PushResult : std::uint8_t {
    kAccepted,
    kOutOfOrder,  ///< event time not after the newest entry (duplicates included)
    kNonFinite,   ///< ok entry with NaN or infinity
};

struct WindowConfig {
    /// Entries with event time <= newest_time - span are evicted. (t - span, t] is the window.
    std::chrono::milliseconds span{std::chrono::seconds{60}};
    /// Hard memory bound (NFR-4). When full, the oldest entry is evicted even if it is still
    /// inside the span; that is counted as a capacity eviction.
    std::size_t capacity{256};
    /// Evictions between exact re-computations of the Welford state from the buffer.
    std::size_t reseed_interval{1024};
};

/// Summary of the ok values currently in the window. Every optional is nullopt when it cannot
/// be computed from the data (too few ok values, or zero elapsed time), never 0.
struct WindowStats {
    std::size_t entries{0};     ///< ok + not-ok entries in the window
    std::size_t ok_entries{0};  ///< entries that carry a value
    std::optional<double> last;
    std::optional<double> mean;
    std::optional<double> stddev;  ///< sample standard deviation (n - 1)
    std::optional<double> min;
    std::optional<double> max;
    /// (last ok value - first ok value) / elapsed seconds between them.
    std::optional<double> rate_per_second;
    /// Ordinary least-squares slope of value over event time, per second (centred two-pass).
    std::optional<double> slope_per_second;
    /// Not-ok entries / entries; nullopt for an empty window.
    std::optional<double> missing_ratio;
};

/// Event-time sliding window over one series, stored in a fixed-capacity ring buffer.
///
/// Complexity (n = entries in the window, bounded by capacity):
/// - push: O(1) amortized. Each entry is evicted once (O(1) Welford removal); every
///   `reseed_interval` evictions the Welford state is recomputed from the buffer in O(n), which
///   is O(n / reseed_interval) amortized per push.
/// - count, mean, variance: O(1).
/// - min, max, rate, slope, missing ratio: O(n) per stats() call. Features are read once per
///   evaluation tick, so this is O(n) per tick per series.
/// - Memory: capacity entries, allocated once.
class RollingWindow {
  public:
    struct Entry {
        Timestamp at;
        double value{0.0};
        bool ok{false};
    };

    struct Counters {
        std::uint64_t accepted{0};
        std::uint64_t out_of_order{0};
        std::uint64_t non_finite{0};
        std::uint64_t span_evictions{0};
        std::uint64_t capacity_evictions{0};
        std::uint64_t reseeds{0};
    };

    explicit RollingWindow(WindowConfig config);

    /// Offers an ok value (ok = true) or a marker for a missing/stale/invalid value (ok =
    /// false; `value` ignored). Entries must arrive in strictly increasing event time.
    PushResult push(Timestamp at, double value, bool ok);

    /// Evicts entries that fell out of the span as of `now` (event time). Called by
    /// stats(now); exposed so a caller can advance time without reading.
    void advance_to(Timestamp now);

    /// Stats over (now - span, now]. Precondition: now >= newest(); otherwise entries "from
    /// the future" would be included, so nullopt is returned.
    [[nodiscard]] std::optional<WindowStats> stats(Timestamp now);

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::optional<Timestamp> newest() const noexcept;
    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }
    [[nodiscard]] const WelfordAccumulator& accumulator() const noexcept { return welford_; }
    [[nodiscard]] const WindowConfig& config() const noexcept { return config_; }

    /// Entries oldest first (for tests and reference checks).
    [[nodiscard]] std::vector<Entry> entries() const;

  private:
    [[nodiscard]] const Entry& at_index(std::size_t i) const noexcept;
    void evict_oldest(bool capacity_eviction);
    void reseed();

    WindowConfig config_;
    std::vector<Entry> ring_;
    std::size_t head_{0};  ///< index of the oldest entry
    std::size_t size_{0};
    std::size_t evictions_since_reseed_{0};
    WelfordAccumulator welford_;
    Counters counters_;
};

}  // namespace faulttwin::features

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace faulttwin::observability {

/// Monotonic counter. Lock-free; increments use relaxed ordering because a scrape needs a
/// recent value, not a happens-before relation with other memory.
class Counter {
  public:
    void inc(std::uint64_t n = 1) noexcept { value_.fetch_add(n, std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<std::uint64_t> value_{0};
};

/// A value that can go up and down.
class Gauge {
  public:
    void set(double v) noexcept { value_.store(v, std::memory_order_relaxed); }
    void add(double delta) noexcept;
    [[nodiscard]] double value() const noexcept { return value_.load(std::memory_order_relaxed); }

  private:
    std::atomic<double> value_{0.0};
};

/// Fixed-bucket histogram. Buckets are upper bounds (le), strictly increasing and finite;
/// +Inf is implicit. observe() is O(log buckets) and lock-free. A scrape that runs during
/// observe() can see a bucket count and a sum that are one observation apart; Prometheus
/// tolerates this, and each individual value is exact.
class Histogram {
  public:
    explicit Histogram(std::vector<double> bounds);
    void observe(double value) noexcept;

    struct Snapshot {
        std::vector<double> bounds;
        std::vector<std::uint64_t> cumulative;  ///< one per bound, then +Inf
        double sum{0.0};
        std::uint64_t count{0};
    };
    [[nodiscard]] Snapshot snapshot() const;
    [[nodiscard]] const std::vector<double>& bounds() const noexcept { return bounds_; }

  private:
    std::vector<double> bounds_;
    // NOLINTNEXTLINE(*-avoid-c-arrays): atomics are not movable, so no std::vector
    std::unique_ptr<std::atomic<std::uint64_t>[]> buckets_;  ///< bounds_.size() + 1 slots
    std::atomic<double> sum_{0.0};
};

/// Default latency buckets in seconds: 0.5 ms to 10 s.
[[nodiscard]] std::vector<double> default_latency_buckets();

enum class MetricType : std::uint8_t { kCounter, kGauge, kHistogram };

/// Metric names: [a-zA-Z_:][a-zA-Z0-9_:]*. Label names: [a-zA-Z_][a-zA-Z0-9_]*, not starting
/// with "__" (reserved by Prometheus).
[[nodiscard]] bool valid_metric_name(std::string_view name) noexcept;
[[nodiscard]] bool valid_label_name(std::string_view name) noexcept;

/// Owns every metric of a process. Families are registered once at startup (registration
/// with a bad name, a clash or a wrong label count throws std::invalid_argument, a
/// programming error). Series are created on first use and live as long as the registry, so
/// the references handed out stay valid. Each family has a series limit: label values beyond
/// it go to one shared overflow series that is not exported, and every such lookup increments
/// faulttwin_metrics_series_overflow_total, so a cardinality bug is visible but bounded.
class Registry {
  public:
    explicit Registry(std::size_t max_series_per_family = 256);

    Counter& counter(const std::string& name, const std::string& help,
                     const std::vector<std::string>& label_names = {},
                     const std::vector<std::string>& label_values = {});
    Gauge& gauge(const std::string& name, const std::string& help,
                 const std::vector<std::string>& label_names = {},
                 const std::vector<std::string>& label_values = {});
    Histogram& histogram(const std::string& name, const std::string& help,
                         const std::vector<double>& bounds,
                         const std::vector<std::string>& label_names = {},
                         const std::vector<std::string>& label_values = {});

    /// Prometheus text exposition format 0.0.4. Families sorted by name, series by labels,
    /// so the output is deterministic.
    [[nodiscard]] std::string render() const;

    [[nodiscard]] std::uint64_t overflow_count() const noexcept { return overflow_.value(); }

  private:
    struct Series {
        std::unique_ptr<Counter> counter;
        std::unique_ptr<Gauge> gauge;
        std::unique_ptr<Histogram> histogram;
    };
    struct Family {
        MetricType type{MetricType::kCounter};
        std::string help;
        std::vector<std::string> label_names;
        std::vector<double> bounds;
        std::map<std::vector<std::string>, Series> series;
        Series overflow;
    };

    Series& lookup(const std::string& name, MetricType type, const std::string& help,
                   const std::vector<std::string>& label_names,
                   const std::vector<std::string>& label_values, const std::vector<double>& bounds);

    std::size_t max_series_;
    mutable std::mutex mutex_;
    std::map<std::string, Family, std::less<>> families_;
    Counter overflow_;
};

/// Escapes a label value for the exposition format (backslash, double quote, newline).
[[nodiscard]] std::string escape_label_value(std::string_view value);

}  // namespace faulttwin::observability

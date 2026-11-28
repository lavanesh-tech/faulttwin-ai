#include "faulttwin/observability/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "faulttwin/core/json.hpp"

namespace faulttwin::observability {

namespace {

void atomic_add(std::atomic<double>& target, double delta) noexcept {
    double current = target.load(std::memory_order_relaxed);
    while (!target.compare_exchange_weak(current, current + delta, std::memory_order_relaxed)) {
    }
}

/// Prometheus float text: integers without a fraction, otherwise 15 significant digits;
/// +Inf, -Inf and NaN spelled the way the format requires.
std::string format_value(double v) {
    if (std::isnan(v)) {
        return "NaN";
    }
    if (std::isinf(v)) {
        return v > 0 ? "+Inf" : "-Inf";
    }
    return core::json_number(v);
}

std::string escape_help(std::string_view help) {
    std::string out;
    for (const char c : help) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

std::string label_block(const std::vector<std::string>& names,
                        const std::vector<std::string>& values, std::string_view extra = {}) {
    if (names.empty() && extra.empty()) {
        return {};
    }
    std::string out = "{";
    for (std::size_t i = 0; i < names.size(); ++i) {
        out += i == 0 ? "" : ",";
        out += names[i];
        out += "=\"";
        out += escape_label_value(values[i]);
        out += '"';
    }
    if (!extra.empty()) {
        out += names.empty() ? "" : ",";
        out += extra;
    }
    out += '}';
    return out;
}

std::string_view type_name(MetricType type) {
    switch (type) {
        case MetricType::kCounter:
            return "counter";
        case MetricType::kGauge:
            return "gauge";
        case MetricType::kHistogram:
            return "histogram";
    }
    return "untyped";
}

}  // namespace

void Gauge::add(double delta) noexcept {
    atomic_add(value_, delta);
}

Histogram::Histogram(std::vector<double> bounds)
    : bounds_(std::move(bounds)),
      // NOLINTNEXTLINE(*-avoid-c-arrays): see the member declaration
      buckets_(std::make_unique<std::atomic<std::uint64_t>[]>(bounds_.size() + 1)) {
    for (std::size_t i = 0; i < bounds_.size(); ++i) {
        if (!std::isfinite(bounds_[i]) || (i > 0 && bounds_[i] <= bounds_[i - 1])) {
            throw std::invalid_argument("histogram bounds must be finite and strictly increasing");
        }
    }
}

void Histogram::observe(double value) noexcept {
    const auto it = std::lower_bound(bounds_.begin(), bounds_.end(), value);
    const auto index = static_cast<std::size_t>(it - bounds_.begin());
    buckets_[index].fetch_add(1, std::memory_order_relaxed);
    if (std::isfinite(value)) {
        atomic_add(sum_, value);
    }
}

Histogram::Snapshot Histogram::snapshot() const {
    Snapshot s;
    s.bounds = bounds_;
    std::uint64_t running = 0;
    for (std::size_t i = 0; i <= bounds_.size(); ++i) {
        running += buckets_[i].load(std::memory_order_relaxed);
        s.cumulative.push_back(running);
    }
    s.sum = sum_.load(std::memory_order_relaxed);
    // _count is the +Inf bucket by definition, so it is derived from the buckets just read.
    s.count = running;
    return s;
}

std::vector<double> default_latency_buckets() {
    return {0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};
}

bool valid_metric_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
        const bool digit = c >= '0' && c <= '9';
        if (!alpha && (i == 0 || !digit)) {
            return false;
        }
    }
    return true;
}

bool valid_label_name(std::string_view name) noexcept {
    if (name.empty() || name.starts_with("__")) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        const bool digit = c >= '0' && c <= '9';
        if (!alpha && (i == 0 || !digit)) {
            return false;
        }
    }
    return true;
}

std::string escape_label_value(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '"') {
            out += "\\\"";
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

Registry::Registry(std::size_t max_series_per_family) : max_series_(max_series_per_family) {}

Registry::Series& Registry::lookup(const std::string& name, MetricType type,
                                   const std::string& help,
                                   const std::vector<std::string>& label_names,
                                   const std::vector<std::string>& label_values,
                                   const std::vector<double>& bounds) {
    if (!valid_metric_name(name)) {
        throw std::invalid_argument("invalid metric name: " + name);
    }
    if (label_names.size() != label_values.size()) {
        throw std::invalid_argument("label count mismatch for " + name);
    }
    const std::scoped_lock lock(mutex_);
    auto it = families_.find(name);
    if (it == families_.end()) {
        for (const auto& label : label_names) {
            if (!valid_label_name(label) || label == "le") {
                std::string message = "invalid label name '";
                message.append(label).append("' for ").append(name);
                throw std::invalid_argument(message);
            }
        }
        Family family;
        family.type = type;
        family.help = help;
        family.label_names = label_names;
        family.bounds = bounds;
        it = families_.emplace(name, std::move(family)).first;
    }
    Family& family = it->second;
    if (family.type != type || family.label_names != label_names || family.bounds != bounds) {
        throw std::invalid_argument("metric " + name + " re-registered with a different shape");
    }
    auto series_it = family.series.find(label_values);
    if (series_it == family.series.end()) {
        Series* target = nullptr;
        if (family.series.size() >= max_series_) {
            overflow_.inc();
            target = &family.overflow;
        } else {
            target = &family.series[label_values];
        }
        if (!target->counter && !target->gauge && !target->histogram) {
            switch (type) {
                case MetricType::kCounter:
                    target->counter = std::make_unique<Counter>();
                    break;
                case MetricType::kGauge:
                    target->gauge = std::make_unique<Gauge>();
                    break;
                case MetricType::kHistogram:
                    target->histogram = std::make_unique<Histogram>(bounds);
                    break;
            }
        }
        return *target;
    }
    return series_it->second;
}

Counter& Registry::counter(const std::string& name, const std::string& help,
                           const std::vector<std::string>& label_names,
                           const std::vector<std::string>& label_values) {
    return *lookup(name, MetricType::kCounter, help, label_names, label_values, {}).counter;
}

Gauge& Registry::gauge(const std::string& name, const std::string& help,
                       const std::vector<std::string>& label_names,
                       const std::vector<std::string>& label_values) {
    return *lookup(name, MetricType::kGauge, help, label_names, label_values, {}).gauge;
}

Histogram& Registry::histogram(const std::string& name, const std::string& help,
                               const std::vector<double>& bounds,
                               const std::vector<std::string>& label_names,
                               const std::vector<std::string>& label_values) {
    return *lookup(name, MetricType::kHistogram, help, label_names, label_values, bounds).histogram;
}

std::string Registry::render() const {
    std::string out;
    const auto render_family_header = [&out](const std::string& name, std::string_view help,
                                             MetricType type) {
        out += "# HELP " + name + ' ' + escape_help(help) + '\n';
        out += "# TYPE " + name + ' ';
        out += type_name(type);
        out += '\n';
    };
    const std::scoped_lock lock(mutex_);
    bool overflow_written = false;
    const std::string overflow_name = "faulttwin_metrics_series_overflow_total";
    const auto write_overflow = [&] {
        render_family_header(
            overflow_name, "Lookups that exceeded a family's series limit (label cardinality bug)",
            MetricType::kCounter);
        out += overflow_name + ' ' + std::to_string(overflow_.value()) + '\n';
        overflow_written = true;
    };
    for (const auto& [name, family] : families_) {
        if (!overflow_written && overflow_name < name) {
            write_overflow();
        }
        render_family_header(name, family.help, family.type);
        for (const auto& [values, series] : family.series) {
            switch (family.type) {
                case MetricType::kCounter:
                    out += name + label_block(family.label_names, values) + ' ' +
                           std::to_string(series.counter->value()) + '\n';
                    break;
                case MetricType::kGauge:
                    out += name + label_block(family.label_names, values) + ' ' +
                           format_value(series.gauge->value()) + '\n';
                    break;
                case MetricType::kHistogram: {
                    const auto snap = series.histogram->snapshot();
                    for (std::size_t i = 0; i <= snap.bounds.size(); ++i) {
                        const std::string le =
                            i < snap.bounds.size() ? format_value(snap.bounds[i]) : "+Inf";
                        out += name + "_bucket" +
                               label_block(family.label_names, values, "le=\"" + le + "\"") + ' ' +
                               std::to_string(snap.cumulative[i]) + '\n';
                    }
                    out += name + "_sum" + label_block(family.label_names, values) + ' ' +
                           format_value(snap.sum) + '\n';
                    out += name + "_count" + label_block(family.label_names, values) + ' ' +
                           std::to_string(snap.count) + '\n';
                    break;
                }
            }
        }
    }
    if (!overflow_written) {
        write_overflow();
    }
    return out;
}

}  // namespace faulttwin::observability

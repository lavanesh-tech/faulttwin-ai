#include "faulttwin/features/rolling_window.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace faulttwin::features {

void WelfordAccumulator::add(double x) noexcept {
    ++count_;
    const double delta = x - mean_;
    mean_ += delta / static_cast<double>(count_);
    m2_ += delta * (x - mean_);
}

void WelfordAccumulator::remove(double x) noexcept {
    if (count_ <= 1) {
        reset();
        return;
    }
    const double old_mean = mean_;
    --count_;
    mean_ = old_mean - (x - old_mean) / static_cast<double>(count_);
    m2_ -= (x - old_mean) * (x - mean_);
    // Rounding can push M2 slightly below zero when the remaining values are (nearly) equal.
    m2_ = std::max(m2_, 0.0);
}

std::optional<double> WelfordAccumulator::sample_variance() const noexcept {
    if (count_ < 2) {
        return std::nullopt;
    }
    return m2_ / static_cast<double>(count_ - 1);
}

RollingWindow::RollingWindow(WindowConfig config) : config_(config) {
    if (config_.capacity == 0) {
        throw std::invalid_argument("RollingWindow capacity must be > 0");
    }
    if (config_.span <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("RollingWindow span must be > 0");
    }
    if (config_.reseed_interval == 0) {
        throw std::invalid_argument("RollingWindow reseed_interval must be > 0");
    }
    ring_.resize(config_.capacity);
}

const RollingWindow::Entry& RollingWindow::at_index(std::size_t i) const noexcept {
    return ring_[(head_ + i) % ring_.size()];
}

std::optional<Timestamp> RollingWindow::newest() const noexcept {
    if (size_ == 0) {
        return std::nullopt;
    }
    return at_index(size_ - 1).at;
}

PushResult RollingWindow::push(Timestamp at, double value, bool ok) {
    if (const auto last = newest(); last && at <= *last) {
        ++counters_.out_of_order;
        return PushResult::kOutOfOrder;
    }
    if (ok && !std::isfinite(value)) {
        ++counters_.non_finite;
        return PushResult::kNonFinite;
    }
    advance_to(at);
    if (size_ == ring_.size()) {
        evict_oldest(/*capacity_eviction=*/true);
    }
    ring_[(head_ + size_) % ring_.size()] = Entry{at, ok ? value : 0.0, ok};
    ++size_;
    if (ok) {
        welford_.add(value);
    }
    ++counters_.accepted;
    return PushResult::kAccepted;
}

void RollingWindow::advance_to(Timestamp now) {
    const Timestamp cutoff = now - config_.span;
    while (size_ > 0 && at_index(0).at <= cutoff) {
        evict_oldest(/*capacity_eviction=*/false);
    }
}

void RollingWindow::evict_oldest(bool capacity_eviction) {
    const Entry& oldest = ring_[head_];
    if (oldest.ok) {
        welford_.remove(oldest.value);
    }
    head_ = (head_ + 1) % ring_.size();
    --size_;
    if (capacity_eviction) {
        ++counters_.capacity_evictions;
    } else {
        ++counters_.span_evictions;
    }
    if (++evictions_since_reseed_ >= config_.reseed_interval) {
        reseed();
    }
}

void RollingWindow::reseed() {
    welford_.reset();
    for (std::size_t i = 0; i < size_; ++i) {
        const Entry& e = at_index(i);
        if (e.ok) {
            welford_.add(e.value);
        }
    }
    evictions_since_reseed_ = 0;
    ++counters_.reseeds;
}

namespace {

double seconds_between(Timestamp from, Timestamp to) {
    return std::chrono::duration<double>(to - from).count();
}

}  // namespace

std::optional<WindowStats> RollingWindow::stats(Timestamp now) {
    if (const auto last = newest(); last && now < *last) {
        return std::nullopt;
    }
    advance_to(now);

    WindowStats s;
    s.entries = size_;
    if (size_ == 0) {
        return s;
    }

    std::optional<Entry> first_ok;
    std::optional<Entry> last_ok;
    double min = 0.0;
    double max = 0.0;
    double t_sum = 0.0;
    for (std::size_t i = 0; i < size_; ++i) {
        const Entry& e = at_index(i);
        if (!e.ok) {
            continue;
        }
        if (!first_ok) {
            first_ok = e;
            min = e.value;
            max = e.value;
        }
        last_ok = e;
        min = std::min(min, e.value);
        max = std::max(max, e.value);
        t_sum += seconds_between(first_ok->at, e.at);
        ++s.ok_entries;
    }
    s.missing_ratio = static_cast<double>(size_ - s.ok_entries) / static_cast<double>(size_);
    if (s.ok_entries == 0 || !first_ok || !last_ok) {
        return s;
    }

    s.last = last_ok->value;
    s.min = min;
    s.max = max;
    s.mean = welford_.mean();
    if (const auto var = welford_.sample_variance()) {
        s.stddev = std::sqrt(*var);
    }

    const double elapsed = seconds_between(first_ok->at, last_ok->at);
    if (elapsed > 0.0) {
        s.rate_per_second = (last_ok->value - first_ok->value) / elapsed;
    }

    // Least-squares slope with time and value centred on their means (two passes over the
    // buffer), which avoids the cancellation of the textbook sum-of-products formula.
    if (s.ok_entries >= 2) {
        const double t_mean = t_sum / static_cast<double>(s.ok_entries);
        const double v_mean = *s.mean;
        double sxy = 0.0;
        double sxx = 0.0;
        for (std::size_t i = 0; i < size_; ++i) {
            const Entry& e = at_index(i);
            if (!e.ok) {
                continue;
            }
            const double dt = seconds_between(first_ok->at, e.at) - t_mean;
            sxy += dt * (e.value - v_mean);
            sxx += dt * dt;
        }
        if (sxx > 0.0) {
            s.slope_per_second = sxy / sxx;
        }
    }
    return s;
}

std::vector<RollingWindow::Entry> RollingWindow::entries() const {
    std::vector<Entry> out;
    out.reserve(size_);
    for (std::size_t i = 0; i < size_; ++i) {
        out.push_back(at_index(i));
    }
    return out;
}

}  // namespace faulttwin::features

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace faulttwin::collectors {

/// Quality of one collected value. Healthy values are kOk. Everything else says *why* there is
/// no trustworthy value, so a consumer can never confuse "missing" with zero, or "stale" with
/// current.
enum class Quality : std::uint8_t {
    kOk,             ///< measured in this cycle and passed every check
    kMissing,        ///< no value: node unreachable, query failed, NULL from the server, ...
    kStale,          ///< a value exists but is older than the allowed age; kept for context
    kInvalid,        ///< the source returned something that fails a check (negative, garbage)
    kNotApplicable,  ///< the metric has no meaning here (e.g. replica lag on a primary)
};

[[nodiscard]] std::string_view to_string(Quality quality) noexcept;

/// One metric value together with its quality and a short human-readable reason.
/// Invariant: value() is engaged if and only if quality() is kOk or kStale.
template <typename T>
class Reading {
  public:
    /// Default: missing, because nothing has been collected.
    Reading() : Reading(Quality::kMissing, std::nullopt, "not collected") {}

    /// `note` optionally explains how an ok value was derived (e.g. "caught up").
    [[nodiscard]] static Reading ok(T value, std::string note = {}) {
        return {Quality::kOk, std::move(value), std::move(note)};
    }
    [[nodiscard]] static Reading missing(std::string reason) {
        return {Quality::kMissing, std::nullopt, std::move(reason)};
    }
    [[nodiscard]] static Reading invalid(std::string reason) {
        return {Quality::kInvalid, std::nullopt, std::move(reason)};
    }
    [[nodiscard]] static Reading not_applicable(std::string reason = {}) {
        return {Quality::kNotApplicable, std::nullopt, std::move(reason)};
    }

    /// Marks an ok reading stale (value kept). Other qualities are unchanged: a missing value
    /// does not become "stale", and an invalid one stays invalid.
    void mark_stale(std::string reason) {
        if (quality_ == Quality::kOk) {
            quality_ = Quality::kStale;
            detail_ = std::move(reason);
        }
    }

    [[nodiscard]] Quality quality() const noexcept { return quality_; }
    [[nodiscard]] bool is_ok() const noexcept { return quality_ == Quality::kOk; }
    [[nodiscard]] const std::optional<T>& value() const noexcept { return value_; }
    /// The value of an ok or stale reading. Throws std::logic_error when there is none, so a
    /// caller that skipped the quality check fails loudly instead of reading garbage.
    [[nodiscard]] const T& measured() const {
        if (!value_) {
            throw std::logic_error("Reading has no value: " + detail_);
        }
        return *value_;
    }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

  private:
    Reading(Quality quality, std::optional<T> value, std::string detail)
        : quality_(quality), value_(std::move(value)), detail_(std::move(detail)) {}

    Quality quality_;
    std::optional<T> value_;
    std::string detail_;
};

}  // namespace faulttwin::collectors

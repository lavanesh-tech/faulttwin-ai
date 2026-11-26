#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

#include "faulttwin/collectors/reading.hpp"
#include "faulttwin/telemetry/telemetry_sample.hpp"

namespace faulttwin::collectors {

/// Aggregate CPU time from the first line of /proc/stat, in clock ticks.
struct CpuTicks {
    std::uint64_t total{0};
    std::uint64_t idle{0};  ///< idle + iowait
};

[[nodiscard]] std::optional<CpuTicks> parse_proc_stat(std::string_view text);
/// (MemTotal - MemAvailable) / MemTotal × 100 from /proc/meminfo.
[[nodiscard]] Reading<double> parse_meminfo_used_percent(std::string_view text);
/// First field of /proc/loadavg.
[[nodiscard]] Reading<double> parse_loadavg_1m(std::string_view text);
/// Busy share of CPU time between two samples. Needs two samples; a counter going backwards
/// or no elapsed ticks is kInvalid.
[[nodiscard]] Reading<double> cpu_used_percent(const std::optional<CpuTicks>& previous,
                                               const std::optional<CpuTicks>& current);
/// used / (used + available) × 100, the same rule as df.
[[nodiscard]] Reading<double> disk_used_percent(const std::filesystem::space_info& space);

struct ResourceSnapshot {
    telemetry::Timestamp collected_at;
    Reading<double> cpu_used_percent;
    Reading<double> memory_used_percent;
    Reading<double> disk_used_percent;
    Reading<double> load_average_1m;
};

/// Host resources of the machine (or container) the collector runs on, read from procfs.
/// The proc root is injectable for tests. Where procfs does not exist (macOS), the procfs
/// readings are kMissing with that reason; disk usage still works (std::filesystem::space).
class LinuxResourceCollector {
  public:
    explicit LinuxResourceCollector(std::filesystem::path proc_root = "/proc",
                                    std::filesystem::path disk_path = "/");
    [[nodiscard]] ResourceSnapshot collect(telemetry::Timestamp now);

  private:
    std::filesystem::path proc_root_;
    std::filesystem::path disk_path_;
    std::optional<CpuTicks> previous_cpu_;
};

}  // namespace faulttwin::collectors

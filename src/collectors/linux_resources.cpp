#include "faulttwin/collectors/linux_resources.hpp"

#include <charconv>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace faulttwin::collectors {
namespace {

std::optional<std::string> read_file(const std::filesystem::path& path) {
    const std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::optional<std::uint64_t> parse_u64(std::string_view text) {
    std::uint64_t value = 0;
    const char* const last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), last, value);
    if (text.empty() || ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::string_view> split_whitespace(std::string_view line) {
    std::vector<std::string_view> parts;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        if (i > start) {
            parts.push_back(line.substr(start, i - start));
        }
    }
    return parts;
}

/// Value in kB of a "Key:   1234 kB" line in /proc/meminfo.
std::optional<std::uint64_t> meminfo_value(std::string_view text, std::string_view key) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto end = text.find('\n', pos);
        const std::string_view line =
            text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        if (line.starts_with(key) && line.size() > key.size() && line[key.size()] == ':') {
            const auto parts = split_whitespace(line.substr(key.size() + 1));
            if (parts.empty()) {
                return std::nullopt;
            }
            return parse_u64(parts.front());
        }
        if (end == std::string_view::npos) {
            break;
        }
        pos = end + 1;
    }
    return std::nullopt;
}

}  // namespace

std::optional<CpuTicks> parse_proc_stat(std::string_view text) {
    const std::string_view first_line = text.substr(0, text.find('\n'));
    const auto parts = split_whitespace(first_line);
    // "cpu user nice system idle iowait irq softirq steal [guest guest_nice]"
    constexpr std::size_t kMinimumFields = 1 + 4;
    if (parts.size() < kMinimumFields || parts.front() != "cpu") {
        return std::nullopt;
    }
    CpuTicks ticks;
    // guest and guest_nice (fields 9 and 10) are already included in user and nice.
    constexpr std::size_t kCountedFields = 8;
    for (std::size_t i = 1; i < parts.size() && i <= kCountedFields; ++i) {
        const auto value = parse_u64(parts[i]);
        if (!value) {
            return std::nullopt;
        }
        ticks.total += *value;
        if (i == 4 || i == 5) {  // idle, iowait
            ticks.idle += *value;
        }
    }
    return ticks;
}

Reading<double> parse_meminfo_used_percent(std::string_view text) {
    const auto total = meminfo_value(text, "MemTotal");
    const auto available = meminfo_value(text, "MemAvailable");
    if (!total || !available) {
        return Reading<double>::missing("MemTotal/MemAvailable not found");
    }
    if (*total == 0 || *available > *total) {
        return Reading<double>::invalid("inconsistent MemTotal/MemAvailable");
    }
    return Reading<double>::ok(100.0 * static_cast<double>(*total - *available) /
                               static_cast<double>(*total));
}

Reading<double> parse_loadavg_1m(std::string_view text) {
    const auto parts = split_whitespace(text.substr(0, text.find('\n')));
    if (parts.empty()) {
        return Reading<double>::missing("empty loadavg");
    }
    const std::string first(parts.front());
    std::istringstream in(first);
    in.imbue(std::locale::classic());
    double value = 0.0;
    if (!(in >> value) || !in.eof() || value < 0.0) {
        return Reading<double>::invalid("unparseable load average");
    }
    return Reading<double>::ok(value);
}

Reading<double> cpu_used_percent(const std::optional<CpuTicks>& previous,
                                 const std::optional<CpuTicks>& current) {
    if (!current) {
        return Reading<double>::missing("CPU counters unavailable");
    }
    if (!previous) {
        return Reading<double>::missing("needs two samples");
    }
    if (current->total < previous->total || current->idle < previous->idle) {
        return Reading<double>::invalid("CPU counters went backwards");
    }
    const std::uint64_t total = current->total - previous->total;
    const std::uint64_t idle = current->idle - previous->idle;
    if (total == 0 || idle > total) {
        return Reading<double>::invalid("no CPU time elapsed between samples");
    }
    return Reading<double>::ok(100.0 * static_cast<double>(total - idle) /
                               static_cast<double>(total));
}

Reading<double> disk_used_percent(const std::filesystem::space_info& space) {
    if (space.capacity == 0 || space.free > space.capacity || space.available > space.free) {
        return Reading<double>::invalid("inconsistent filesystem space values");
    }
    const auto used = space.capacity - space.free;
    const auto denominator = used + space.available;
    if (denominator == 0) {
        return Reading<double>::invalid("filesystem reports no usable space");
    }
    return Reading<double>::ok(100.0 * static_cast<double>(used) /
                               static_cast<double>(denominator));
}

LinuxResourceCollector::LinuxResourceCollector(std::filesystem::path proc_root,
                                               std::filesystem::path disk_path)
    : proc_root_(std::move(proc_root)), disk_path_(std::move(disk_path)) {}

ResourceSnapshot LinuxResourceCollector::collect(telemetry::Timestamp now) {
    ResourceSnapshot snapshot;
    snapshot.collected_at = now;
    const std::string no_procfs = "procfs not available at " + proc_root_.string();

    const auto stat = read_file(proc_root_ / "stat");
    std::optional<CpuTicks> ticks;
    if (stat) {
        ticks = parse_proc_stat(*stat);
        snapshot.cpu_used_percent = ticks ? cpu_used_percent(previous_cpu_, ticks)
                                          : Reading<double>::invalid("unparseable /proc/stat");
    } else {
        snapshot.cpu_used_percent = Reading<double>::missing(no_procfs);
    }
    previous_cpu_ = ticks;

    const auto meminfo = read_file(proc_root_ / "meminfo");
    snapshot.memory_used_percent =
        meminfo ? parse_meminfo_used_percent(*meminfo) : Reading<double>::missing(no_procfs);

    const auto loadavg = read_file(proc_root_ / "loadavg");
    snapshot.load_average_1m =
        loadavg ? parse_loadavg_1m(*loadavg) : Reading<double>::missing(no_procfs);

    std::error_code ec;
    const auto space = std::filesystem::space(disk_path_, ec);
    snapshot.disk_used_percent =
        ec ? Reading<double>::missing("statvfs failed: " + ec.message()) : disk_used_percent(space);
    return snapshot;
}

}  // namespace faulttwin::collectors

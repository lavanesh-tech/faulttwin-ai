// faulttwin-monitor: collects PostgreSQL and host telemetry from a cluster with a read-only role.

#include <sys/resource.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#include "faulttwin/collectors/libpq_session.hpp"
#include "faulttwin/collectors/linux_resources.hpp"
#include "faulttwin/collectors/rates.hpp"
#include "faulttwin/collectors/topology.hpp"
#include "monitor_options.hpp"
#include "monitor_report.hpp"

namespace {

namespace col = faulttwin::collectors;
namespace mon = faulttwin::monitor;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitInternalError = 3;

double process_cpu_ms() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
    const auto to_ms = [](const timeval& tv) {
        return static_cast<double>(tv.tv_sec) * 1000.0 + static_cast<double>(tv.tv_usec) / 1000.0;
    };
    return to_ms(usage.ru_utime) + to_ms(usage.ru_stime);
}

int run(const mon::MonitorOptions& options) {
    std::vector<col::PgNodeCollector> collectors;
    collectors.reserve(options.nodes.size());
    for (const auto& endpoint : options.nodes) {
        collectors.emplace_back(endpoint, options.credentials, options.connect, options.policy);
    }
    col::LinuxResourceCollector resources;
    std::vector<std::optional<col::PgNodeSnapshot>> previous(options.nodes.size());
    mon::RunSummary summary;

    auto next_start = std::chrono::steady_clock::now();
    for (std::uint32_t cycle = 1; cycle <= options.cycles; ++cycle) {
        mon::CycleRecord record;
        record.cycle = cycle;
        const auto cycle_start = std::chrono::steady_clock::now();
        const double cpu_start = process_cpu_ms();

        for (auto& collector : collectors) {
            const auto start = std::chrono::steady_clock::now();
            record.nodes.push_back(collector.collect());
            record.node_wall_ms.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count());
        }
        if (options.resources) {
            record.resources = resources.collect(faulttwin::telemetry::Clock::now());
        }
        for (std::size_t i = 0; i < record.nodes.size(); ++i) {
            const auto& prev = previous[i];
            const auto& curr = record.nodes[i];
            const bool both_reachable = prev && prev->availability != col::Availability::kDown &&
                                        curr.availability != col::Availability::kDown;
            record.rates.push_back(both_reachable ? std::optional{col::compute_rates(*prev, curr)}
                                                  : std::nullopt);
        }
        record.topology = col::derive_topology(record.nodes);
        record.cycle_wall_ms = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - cycle_start)
                                   .count();
        record.client_cpu_ms = process_cpu_ms() - cpu_start;

        // Consumption point: freshness is measured here, and readings older than max_age
        // are marked stale before they are reported.
        const auto now = faulttwin::telemetry::Clock::now();
        for (std::size_t i = 0; i < record.nodes.size(); ++i) {
            record.freshness_ms.push_back(
                std::chrono::duration<double, std::milli>(now - record.nodes[i].collected_at)
                    .count());
            previous[i] = record.nodes[i];
            (void)col::mark_stale_if_older(record.nodes[i], now, options.max_age);
        }

        if (options.format == mon::OutputFormat::kJson) {
            mon::write_cycle_json(std::cout, record);
        } else {
            mon::write_cycle_text(std::cout, record);
        }
        std::cout.flush();
        mon::accumulate(summary, record);

        if (cycle < options.cycles) {
            next_start += options.interval;
            std::this_thread::sleep_until(next_start);
        }
    }
    if (options.format == mon::OutputFormat::kJson) {
        mon::write_summary_json(std::cout, summary);
    } else {
        mon::write_summary_text(std::cout, summary);
    }
    std::cout.flush();
    if (!std::cout) {
        std::cerr << "error: failed to write output\n";
        return kExitInternalError;
    }
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::vector<std::string_view> args(argv + 1, argv + argc);
        const std::string_view program = argc > 0 ? argv[0] : "faulttwin-monitor";
        const auto env = [](std::string_view name) -> std::optional<std::string> {
            const std::string key(name);
            // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread is started
            const char* value = std::getenv(key.c_str());
            return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
        };
        const auto parsed = mon::parse_monitor_options(args, env);
        if (std::holds_alternative<mon::HelpRequested>(parsed)) {
            std::cout << mon::monitor_usage(program);
            return kExitOk;
        }
        if (const auto* usage = std::get_if<mon::UsageError>(&parsed)) {
            std::cerr << "error: " << usage->message << "\n\n" << mon::monitor_usage(program);
            return kExitUsage;
        }
        return run(std::get<mon::MonitorOptions>(parsed));
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return kExitInternalError;
    } catch (...) {
        std::cerr << "fatal: unknown exception\n";
        return kExitInternalError;
    }
}

// Phase 2 benchmarks (docs/evaluation/experiment-protocols.md, Experiments 1-2).
//
// Run only from a Release build (the `release` preset). Use
// scripts/run-benchmarks.sh, which records the environment and writes JSON/CSV.
//
// Every benchmark processes a fixed workload per iteration (Iterations(1)) and
// is repeated; one untimed warm-up pass precedes each timed pass. Counters:
//   processed / rejected / dropped / abandoned / invalid  - event accounting
//   lat_p50_us / lat_p95_us / lat_p99_us / lat_max_us      - submit -> sink done
//   invariant_ok                                            - 1 if ATT/ACC invariants hold

#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "bench_support.hpp"
#include "faulttwin/concurrency/bounded_queue.hpp"
#include "faulttwin/concurrency/queue_types.hpp"
#include "faulttwin/concurrency/unbounded_queue.hpp"
#include "faulttwin/ingest/baseline.hpp"
#include "faulttwin/ingest/event_processor.hpp"
#include "faulttwin/ingest/pipeline.hpp"

namespace faulttwin::bench {
namespace {

using concurrency::OverloadPolicy;
using ingest::IngestEvent;
using ingest::ProcessedEvent;

constexpr std::size_t kWarmupEvents = 100'000;

// Extra per-event CPU work in the sink (simulated_work iterations). Light = none
// beyond validation and ordering; heavy = a few microseconds per event,
// standing in for enrichment/routing work that later phases add.
constexpr std::int64_t kWorkLight = 0;
constexpr std::int64_t kWorkHeavy = 4096;
constexpr auto kDrainTimeout = std::chrono::hours{1};

/// Per-worker latency buffers; each consumer writes only its own vector.
class LatencyRecorder {
  public:
    explicit LatencyRecorder(std::size_t workers, std::size_t expected) : per_worker_(workers) {
        for (auto& v : per_worker_) {
            v.reserve(expected / workers + 1024);
        }
    }

    void record(std::size_t worker, ingest::SteadyClock::time_point submitted) {
        per_worker_[worker].push_back((ingest::SteadyClock::now() - submitted).count());
    }

    std::vector<std::int64_t> merged() const {
        std::vector<std::int64_t> all;
        for (const auto& v : per_worker_) {
            all.insert(all.end(), v.begin(), v.end());
        }
        return all;
    }

  private:
    std::vector<std::vector<std::int64_t>> per_worker_;
};

ingest::EventSink make_sink(std::uint64_t work_iterations, LatencyRecorder& latencies) {
    return [work_iterations, &latencies](std::size_t worker, const ProcessedEvent& e) {
        simulated_work(work_iterations, e.event->sequence);
        latencies.record(worker, e.event->submitted_at);
    };
}

void report_latency(benchmark::State& state, std::vector<std::int64_t> ns) {
    constexpr double kNsPerUs = 1000.0;
    state.counters["lat_p50_us"] = nearest_rank(ns, 50.0) / kNsPerUs;
    state.counters["lat_p95_us"] = nearest_rank(ns, 95.0) / kNsPerUs;
    state.counters["lat_p99_us"] = nearest_rank(ns, 99.0) / kNsPerUs;
    state.counters["lat_max_us"] = nearest_rank(ns, 100.0) / kNsPerUs;
}

// ---- Experiment 1 baseline: single thread, same per-event work -------------

void BM_SingleThreadedBaseline(benchmark::State& state) {
    const auto work = static_cast<std::uint64_t>(state.range(0));
    ingest::ProcessingCounters counters;
    std::uint64_t events = 0;
    {
        LatencyRecorder warm(1, kWarmupEvents);
        ingest::EventProcessor processor({}, 1, workload_now, make_sink(work, warm));
        static_cast<void>(ingest::run_single_threaded_baseline(
            split_by_node(1, kWarmupEvents).front(), processor));
    }
    for (auto _ : state) {
        state.PauseTiming();
        std::vector<telemetry::RawTelemetrySample> input = workload();
        events = input.size();
        LatencyRecorder latencies(1, events);
        ingest::EventProcessor processor({}, 1, workload_now, make_sink(work, latencies));
        state.ResumeTiming();

        counters = ingest::run_single_threaded_baseline(std::move(input), processor);

        state.PauseTiming();
        // In the baseline every event is handled as soon as it is created, so this
        // "latency" is per-event service time, not comparable with queueing latency.
        report_latency(state, latencies.merged());
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(counters.processed()));
    state.counters["processed"] = static_cast<double>(counters.processed());
    state.counters["invalid"] =
        static_cast<double>(counters.count(ingest::Disposition::kRejectedInvalid));
    state.counters["rejected"] = 0;
    state.counters["dropped"] = 0;
    state.counters["invariant_ok"] = counters.processed() == events ? 1 : 0;
}

// ---- Experiment 1: concurrent pipeline --------------------------------------

struct PipelineRun {
    ingest::IngestCounters counters;
    std::uint64_t producer_refused{0};
};

PipelineRun run_pipeline(std::size_t producers, std::size_t consumers, std::size_t capacity,
                         std::size_t batch, OverloadPolicy policy,
                         std::vector<std::vector<telemetry::RawTelemetrySample>> parts,
                         const ingest::EventSink& sink) {
    ingest::PipelineConfig config{.queue_capacity = capacity,
                                  .overload = policy,
                                  .consumers = consumers,
                                  .consumer_batch = batch};
    ingest::IngestPipeline pipeline(config, workload_now, sink);
    std::atomic<std::uint64_t> refused{0};
    {
        std::vector<std::jthread> threads;
        threads.reserve(producers);
        for (std::size_t p = 0; p < producers; ++p) {
            threads.emplace_back([&pipeline, &refused, part = std::move(parts[p])]() mutable {
                for (auto& raw : part) {
                    if (!pipeline.submit(std::move(raw)).accepted()) {
                        refused.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
    }
    const ingest::ShutdownReport report = pipeline.shutdown(kDrainTimeout);
    return {report.counters, refused.load()};
}

void run_pipeline_benchmark(benchmark::State& state, std::size_t producers, std::size_t consumers,
                            std::size_t capacity, std::uint64_t work, std::size_t batch) {
    {
        LatencyRecorder warm(consumers, kWarmupEvents);
        static_cast<void>(
            run_pipeline(producers, consumers, capacity, batch, OverloadPolicy::kBlock,
                         split_by_node(producers, kWarmupEvents), make_sink(work, warm)));
    }
    PipelineRun run;
    for (auto _ : state) {
        state.PauseTiming();
        auto parts = split_by_node(producers, workload().size());
        LatencyRecorder latencies(consumers, workload().size());
        const ingest::EventSink sink = make_sink(work, latencies);
        state.ResumeTiming();

        run = run_pipeline(producers, consumers, capacity, batch, OverloadPolicy::kBlock,
                           std::move(parts), sink);

        state.PauseTiming();
        report_latency(state, latencies.merged());
        state.ResumeTiming();
    }
    const ingest::IngestCounters& c = run.counters;
    state.SetItemsProcessed(static_cast<std::int64_t>(c.processed()));
    state.counters["processed"] = static_cast<double>(c.processed());
    state.counters["invalid"] =
        static_cast<double>(c.processing.count(ingest::Disposition::kRejectedInvalid));
    state.counters["out_of_order"] =
        static_cast<double>(c.processing.count(ingest::Disposition::kDeliveredOutOfOrder));
    state.counters["rejected"] = static_cast<double>(c.rejected());
    state.counters["dropped"] = static_cast<double>(c.dropped_oldest);
    state.counters["abandoned"] = static_cast<double>(c.abandoned_at_shutdown);
    state.counters["queue_hwm"] = static_cast<double>(c.queue_high_watermark);
    state.counters["invariant_ok"] = (c.invariants_hold() && c.attempted == workload().size() &&
                                      c.in_queue == 0 && run.producer_refused == 0)
                                         ? 1
                                         : 0;
}

constexpr std::int64_t kDefaultBatch = 64;

void BM_Pipeline(benchmark::State& state) {
    run_pipeline_benchmark(
        state, static_cast<std::size_t>(state.range(0)), static_cast<std::size_t>(state.range(1)),
        static_cast<std::size_t>(state.range(2)), static_cast<std::uint64_t>(state.range(3)),
        static_cast<std::size_t>(kDefaultBatch));
}

/// Same pipeline with consumer batching on and off (design ablation, not part
/// of the Experiment 1 protocol grid).
void BM_PipelineBatchAblation(benchmark::State& state) {
    run_pipeline_benchmark(
        state, static_cast<std::size_t>(state.range(0)), static_cast<std::size_t>(state.range(1)),
        1024, static_cast<std::uint64_t>(state.range(2)), static_cast<std::size_t>(state.range(3)));
}

void pipeline_grid(benchmark::internal::Benchmark* b) {
    for (const std::int64_t work : {kWorkLight, kWorkHeavy}) {
        for (const std::int64_t capacity : {1024, 16384}) {
            for (const std::int64_t producers : {1, 2, 4}) {
                for (const std::int64_t consumers : {1, 2, 4, 8}) {
                    b->Args({producers, consumers, capacity, work});
                }
            }
        }
    }
}

// ---- Experiment 2 (reduced): overload, bounded policies vs unbounded --------

constexpr std::size_t kOverloadEvents = 200'000;
constexpr std::uint64_t kOverloadWork = 2'000;  // slow consumer: producer outruns it

template <typename Queue>
void run_overload(benchmark::State& state, Queue& queue) {
    // Generated here rather than taken from workload(), so that this process's
    // peak RSS (run it alone, see scripts/run-benchmarks.sh) reflects the queue
    // and not the 1,000,000-event Experiment 1 workload.
    telemetry::SyntheticConfig config = workload_config();
    config.samples_per_node = static_cast<std::uint32_t>(kOverloadEvents / kWorkloadNodes);
    std::vector<telemetry::RawTelemetrySample> input;
    input.reserve(kOverloadEvents);
    for (auto& sample : telemetry::generate_synthetic_samples(config)) {
        input.push_back(std::move(sample.raw));
    }
    std::uint64_t handed_back = 0;
    double producer_seconds = 0;
    std::uint64_t consumed = 0;
    for (auto _ : state) {
        const auto start = ingest::SteadyClock::now();
        std::jthread consumer([&queue, &consumed] {
            for (auto r = queue.pop(); r.status == concurrency::PopStatus::kItem; r = queue.pop()) {
                simulated_work(kOverloadWork, r.item->sequence);  // NOLINT(*-unchecked-optional-*)
                ++consumed;
            }
        });
        std::uint64_t seq = 0;
        for (auto& raw : input) {
            auto pushed = queue.push(
                IngestEvent{.sequence = seq++, .raw = std::move(raw), .submitted_at = start});
            if (pushed.item.has_value()) {
                ++handed_back;  // refused or displaced, returned to the producer
            }
        }
        producer_seconds =
            std::chrono::duration<double>(ingest::SteadyClock::now() - start).count();
        queue.close();
        consumer.join();
    }
    const concurrency::QueueStats s = queue.stats();
    state.SetItemsProcessed(static_cast<std::int64_t>(consumed));
    state.counters["processed"] = static_cast<double>(consumed);
    state.counters["rejected"] = static_cast<double>(s.rejected());
    state.counters["dropped"] = static_cast<double>(s.displaced_oldest);
    state.counters["handed_back"] = static_cast<double>(handed_back);
    state.counters["queue_hwm"] = static_cast<double>(s.high_watermark);
    state.counters["producer_s"] = producer_seconds;
    state.counters["peak_rss_mib"] = peak_rss_mib();
    state.counters["invariant_ok"] = (s.invariants_hold() && s.attempted == kOverloadEvents &&
                                      consumed + handed_back == kOverloadEvents)
                                         ? 1
                                         : 0;
}

void BM_OverloadBounded(benchmark::State& state) {
    const auto policy = static_cast<OverloadPolicy>(state.range(0));
    concurrency::BoundedQueue<IngestEvent> queue(1024, policy);
    state.SetLabel(std::string{concurrency::to_string(policy)} + " cap=1024");
    run_overload(state, queue);
}

void BM_OverloadUnbounded(benchmark::State& state) {
    concurrency::UnboundedQueue<IngestEvent> queue;
    state.SetLabel("unbounded reference");
    run_overload(state, queue);
}

// ---- Queue micro-benchmark: uncontended push + pop --------------------------

template <typename Queue>
void BM_QueuePushPopUncontended(benchmark::State& state, Queue& queue) {
    std::uint64_t value = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(queue.try_push(value++));
        benchmark::DoNotOptimize(queue.try_pop());
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_BoundedQueuePushPop(benchmark::State& state) {
    concurrency::BoundedQueue<std::uint64_t> queue(1024, OverloadPolicy::kBlock);
    BM_QueuePushPopUncontended(state, queue);
}

void BM_UnboundedQueuePushPop(benchmark::State& state) {
    concurrency::UnboundedQueue<std::uint64_t> queue;
    BM_QueuePushPopUncontended(state, queue);
}

constexpr int kRepetitions = 5;

BENCHMARK(BM_SingleThreadedBaseline)
    ->ArgName("work")
    ->Arg(kWorkLight)
    ->Arg(kWorkHeavy)
    ->Iterations(1)
    ->Repetitions(kRepetitions)
    ->UseRealTime()
    ->MeasureProcessCPUTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_Pipeline)
    ->ArgNames({"producers", "consumers", "capacity", "work"})
    ->Apply(pipeline_grid)
    ->Iterations(1)
    ->Repetitions(kRepetitions)
    ->UseRealTime()
    ->MeasureProcessCPUTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_PipelineBatchAblation)
    ->ArgNames({"producers", "consumers", "work", "batch"})
    ->Args({1, 1, kWorkLight, 1})
    ->Args({1, 1, kWorkLight, kDefaultBatch})
    ->Args({2, 4, kWorkLight, 1})
    ->Args({2, 4, kWorkLight, kDefaultBatch})
    ->Args({2, 4, kWorkHeavy, 1})
    ->Args({2, 4, kWorkHeavy, kDefaultBatch})
    ->Iterations(1)
    ->Repetitions(kRepetitions)
    ->UseRealTime()
    ->MeasureProcessCPUTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_OverloadBounded)
    ->ArgName("policy")
    ->Arg(static_cast<std::int64_t>(OverloadPolicy::kBlock))
    ->Arg(static_cast<std::int64_t>(OverloadPolicy::kRejectNewest))
    ->Arg(static_cast<std::int64_t>(OverloadPolicy::kDropOldest))
    ->Iterations(1)
    ->Repetitions(kRepetitions)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_OverloadUnbounded)
    ->Iterations(1)
    ->Repetitions(kRepetitions)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_BoundedQueuePushPop)->Repetitions(kRepetitions);
BENCHMARK(BM_UnboundedQueuePushPop)->Repetitions(kRepetitions);

}  // namespace
}  // namespace faulttwin::bench

BENCHMARK_MAIN();

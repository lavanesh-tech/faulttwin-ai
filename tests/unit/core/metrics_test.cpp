#include "faulttwin/observability/metrics.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace faulttwin::observability {
namespace {

TEST(Metrics, RendersCountersAndGaugesInTextFormat) {
    Registry r;
    r.counter("ft_requests_total", "Requests", {"route"}, {"/b"}).inc(2);
    r.counter("ft_requests_total", "Requests", {"route"}, {"/a"}).inc();
    r.gauge("ft_depth", "Queue depth").set(3.5);
    const std::string text = r.render();
    EXPECT_NE(text.find("# HELP ft_requests_total Requests\n# TYPE ft_requests_total counter\n"
                        "ft_requests_total{route=\"/a\"} 1\nft_requests_total{route=\"/b\"} 2\n"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("# TYPE ft_depth gauge\nft_depth 3.5\n"), std::string::npos) << text;
    EXPECT_NE(text.find("faulttwin_metrics_series_overflow_total 0\n"), std::string::npos);
}

TEST(Metrics, SameLabelsReturnTheSameSeries) {
    Registry r;
    auto& a = r.counter("ft_x_total", "x", {"k"}, {"v"});
    auto& b = r.counter("ft_x_total", "x", {"k"}, {"v"});
    EXPECT_EQ(&a, &b);
}

TEST(Metrics, HistogramBucketsAreCumulativeWithInf) {
    Registry r;
    auto& h = r.histogram("ft_latency_seconds", "Latency", {0.1, 1.0});
    h.observe(0.05);
    h.observe(0.1);  // le is inclusive
    h.observe(0.5);
    h.observe(5.0);
    const std::string text = r.render();
    EXPECT_NE(text.find("ft_latency_seconds_bucket{le=\"0.1\"} 2\n"
                        "ft_latency_seconds_bucket{le=\"1\"} 3\n"
                        "ft_latency_seconds_bucket{le=\"+Inf\"} 4\n"
                        "ft_latency_seconds_sum 5.65\n"
                        "ft_latency_seconds_count 4\n"),
              std::string::npos)
        << text;
}

TEST(Metrics, EscapesLabelValuesAndHelp) {
    Registry r;
    r.counter("ft_e_total", "a\\b\nc", {"v"}, {"q\"u\\o\nte"}).inc();
    const std::string text = r.render();
    EXPECT_NE(text.find("# HELP ft_e_total a\\\\b\\nc\n"), std::string::npos) << text;
    EXPECT_NE(text.find(R"(ft_e_total{v="q\"u\\o\nte"} 1)"), std::string::npos) << text;
}

TEST(Metrics, RejectsBadNamesAndShapeChanges) {
    Registry r;
    EXPECT_THROW((void)r.counter("1bad", "x"), std::invalid_argument);
    EXPECT_THROW((void)r.counter("ok_total", "x", {"__reserved"}, {"v"}), std::invalid_argument);
    EXPECT_THROW((void)r.counter("ok_total", "x", {"le"}, {"v"}), std::invalid_argument);
    EXPECT_THROW((void)r.counter("ok_total", "x", {"a"}, {}), std::invalid_argument);
    (void)r.counter("shape_total", "x", {"a"}, {"1"});
    EXPECT_THROW((void)r.gauge("shape_total", "x", {"a"}, {"1"}), std::invalid_argument);
    EXPECT_THROW((void)r.counter("shape_total", "x", {"b"}, {"1"}), std::invalid_argument);
    EXPECT_THROW(Histogram({1.0, 1.0}), std::invalid_argument);
    EXPECT_THROW(Histogram({std::numeric_limits<double>::infinity()}), std::invalid_argument);
}

TEST(Metrics, CardinalityIsBoundedAndOverflowIsCounted) {
    Registry r(2);
    r.counter("ft_c_total", "x", {"id"}, {"1"}).inc();
    r.counter("ft_c_total", "x", {"id"}, {"2"}).inc();
    auto& overflow_a = r.counter("ft_c_total", "x", {"id"}, {"3"});
    auto& overflow_b = r.counter("ft_c_total", "x", {"id"}, {"4"});
    EXPECT_EQ(&overflow_a, &overflow_b);
    EXPECT_EQ(r.overflow_count(), 2U);
    const std::string text = r.render();
    EXPECT_EQ(text.find("id=\"3\""), std::string::npos);
    EXPECT_NE(text.find("faulttwin_metrics_series_overflow_total 2\n"), std::string::npos);
}

TEST(Metrics, ConcurrentUpdatesAreNotLost) {
    Registry r;
    auto& c = r.counter("ft_conc_total", "x");
    auto& g = r.gauge("ft_conc_gauge", "x");
    auto& h = r.histogram("ft_conc_seconds", "x", {1.0});
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 10000; ++i) {
                    c.inc();
                    g.add(1.0);
                    h.observe(0.5);
                }
            });
        }
        // A scrape running concurrently with the writers (exercised under TSan).
        for (int i = 0; i < 20; ++i) {
            (void)r.render();
        }
    }
    EXPECT_EQ(c.value(), 40000U);
    EXPECT_DOUBLE_EQ(g.value(), 40000.0);
    EXPECT_EQ(h.snapshot().count, 40000U);
    EXPECT_DOUBLE_EQ(h.snapshot().sum, 20000.0);
}

}  // namespace
}  // namespace faulttwin::observability

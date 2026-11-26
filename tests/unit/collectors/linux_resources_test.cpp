#include "faulttwin/collectors/linux_resources.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace faulttwin::collectors {
namespace {

constexpr const char* kStat =
    "cpu  100 5 50 800 20 3 2 0 7 1\ncpu0 50 2 25 400 10 1 1 0 0 0\nintr 1 2 3\n";

TEST(LinuxResourcesTest, ParsesProcStatExcludingGuestTime) {
    const auto ticks = parse_proc_stat(kStat);
    ASSERT_TRUE(ticks.has_value());
    EXPECT_EQ(ticks->total, 100U + 5 + 50 + 800 + 20 + 3 + 2 + 0);
    EXPECT_EQ(ticks->idle, 820U);
}

TEST(LinuxResourcesTest, RejectsMalformedProcStat) {
    EXPECT_FALSE(parse_proc_stat("").has_value());
    EXPECT_FALSE(parse_proc_stat("cpu0 1 2 3 4\n").has_value());
    EXPECT_FALSE(parse_proc_stat("cpu 1 2 x 4\n").has_value());
    EXPECT_FALSE(parse_proc_stat("cpu 1 2\n").has_value());
}

TEST(LinuxResourcesTest, CpuPercentNeedsTwoSamplesAndForwardCounters) {
    const CpuTicks a{1000, 800};
    const CpuTicks b{1200, 850};
    EXPECT_EQ(cpu_used_percent(std::nullopt, b).quality(), Quality::kMissing);
    EXPECT_EQ(cpu_used_percent(a, std::nullopt).quality(), Quality::kMissing);
    EXPECT_DOUBLE_EQ(*cpu_used_percent(a, b).value(), 75.0);
    EXPECT_EQ(cpu_used_percent(b, a).quality(), Quality::kInvalid);
    EXPECT_EQ(cpu_used_percent(a, a).quality(), Quality::kInvalid);
}

TEST(LinuxResourcesTest, MemoryUsesMemAvailable) {
    const auto r = parse_meminfo_used_percent(
        "MemTotal:       1000 kB\nMemFree:          100 kB\nMemAvailable:     250 kB\n");
    ASSERT_TRUE(r.is_ok());
    EXPECT_DOUBLE_EQ(*r.value(), 75.0);
    EXPECT_EQ(parse_meminfo_used_percent("MemTotal: 1000 kB\n").quality(), Quality::kMissing);
    EXPECT_EQ(parse_meminfo_used_percent("MemTotal: 10 kB\nMemAvailable: 20 kB\n").quality(),
              Quality::kInvalid);
    // A key that is only a prefix of another key does not match.
    EXPECT_EQ(parse_meminfo_used_percent("MemTotalX: 1 kB\nMemAvailable: 1 kB\n").quality(),
              Quality::kMissing);
}

TEST(LinuxResourcesTest, LoadAverage) {
    EXPECT_DOUBLE_EQ(*parse_loadavg_1m("0.52 0.40 0.30 1/200 1234\n").value(), 0.52);
    EXPECT_EQ(parse_loadavg_1m("").quality(), Quality::kMissing);
    EXPECT_EQ(parse_loadavg_1m("abc 1 2").quality(), Quality::kInvalid);
    EXPECT_EQ(parse_loadavg_1m("-1 1 2").quality(), Quality::kInvalid);
}

TEST(LinuxResourcesTest, DiskUsesTheDfRule) {
    const std::filesystem::space_info space{1000, 400, 300};  // capacity, free, available
    EXPECT_NEAR(*disk_used_percent(space).value(), 100.0 * 600 / 900, 1e-9);
    EXPECT_EQ(disk_used_percent({0, 0, 0}).quality(), Quality::kInvalid);
    EXPECT_EQ(disk_used_percent({100, 200, 0}).quality(), Quality::kInvalid);
}

class FakeProcTest : public ::testing::Test {
  protected:
    void SetUp() override {
        root_ =
            std::filesystem::temp_directory_path() /
            ("faulttwin-proc-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::create_directories(root_);
    }
    void TearDown() override { std::filesystem::remove_all(root_); }
    void write(const char* name, const std::string& text) const {
        std::ofstream(root_ / name) << text;
    }
    std::filesystem::path root_;
};

TEST_F(FakeProcTest, CollectorReadsInjectedProcRoot) {
    write("stat", "cpu 100 0 100 800 0 0 0 0\n");
    write("meminfo", "MemTotal: 1000 kB\nMemAvailable: 500 kB\n");
    write("loadavg", "1.50 1.00 0.50 1/100 42\n");
    LinuxResourceCollector collector(root_, root_);
    const auto first = collector.collect({});
    EXPECT_EQ(first.cpu_used_percent.quality(), Quality::kMissing);  // needs two samples
    EXPECT_DOUBLE_EQ(*first.memory_used_percent.value(), 50.0);
    EXPECT_DOUBLE_EQ(*first.load_average_1m.value(), 1.5);
    EXPECT_TRUE(first.disk_used_percent.is_ok());

    write("stat", "cpu 200 0 100 900 0 0 0 0\n");
    const auto second = collector.collect({});
    EXPECT_DOUBLE_EQ(*second.cpu_used_percent.value(), 50.0);
}

TEST_F(FakeProcTest, MissingProcfsIsMissingNotZero) {
    LinuxResourceCollector collector(root_ / "does-not-exist", root_);
    const auto snap = collector.collect({});
    EXPECT_EQ(snap.cpu_used_percent.quality(), Quality::kMissing);
    EXPECT_EQ(snap.memory_used_percent.quality(), Quality::kMissing);
    EXPECT_EQ(snap.load_average_1m.quality(), Quality::kMissing);
    EXPECT_NE(snap.memory_used_percent.detail().find("procfs not available"), std::string::npos);
    EXPECT_TRUE(snap.disk_used_percent.is_ok());
}

TEST_F(FakeProcTest, CorruptProcStatIsInvalid) {
    write("stat", "garbage\n");
    LinuxResourceCollector collector(root_, root_);
    EXPECT_EQ(collector.collect({}).cpu_used_percent.quality(), Quality::kInvalid);
}

}  // namespace
}  // namespace faulttwin::collectors

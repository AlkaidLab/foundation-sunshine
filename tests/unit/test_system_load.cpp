/**
 * @file tests/unit/test_system_load.cpp
 * @brief CPU delta, GPU engine counter parsing and aggregation, and the JSON shape of the system load route.
 */
#include <cmath>
#include <cstdio>
#include <limits>

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include "src/system_load.h"

using system_load::cpu_percent_from_times;
using system_load::cpu_times_t;
using system_load::gpu_engine_sample_t;

TEST(SystemLoadCpu, NormalInterval) {
  // 1000 ticks total (kernel includes idle), 250 of them idle: 75 % busy.
  const auto percent = cpu_percent_from_times({ 100, 600, 400 }, { 350, 1100, 900 });
  ASSERT_TRUE(percent.has_value());
  EXPECT_DOUBLE_EQ(*percent, 75.0);
}

TEST(SystemLoadCpu, FullyBusy) {
  const auto percent = cpu_percent_from_times({ 10, 100, 100 }, { 10, 600, 600 });
  ASSERT_TRUE(percent.has_value());
  EXPECT_DOUBLE_EQ(*percent, 100.0);
}

TEST(SystemLoadCpu, FullyIdle) {
  const auto percent = cpu_percent_from_times({ 0, 0, 0 }, { 500, 500, 0 });
  ASSERT_TRUE(percent.has_value());
  EXPECT_DOUBLE_EQ(*percent, 0.0);
}

TEST(SystemLoadCpu, ZeroDeltaHasNoValue) {
  const cpu_times_t same { 5, 50, 50 };
  EXPECT_FALSE(cpu_percent_from_times(same, same).has_value());
}

TEST(SystemLoadCpu, CounterGoingBackwardsHasNoValue) {
  EXPECT_FALSE(cpu_percent_from_times({ 100, 1000, 1000 }, { 50, 2000, 2000 }).has_value());
  EXPECT_FALSE(cpu_percent_from_times({ 100, 1000, 1000 }, { 150, 900, 2000 }).has_value());
  EXPECT_FALSE(cpu_percent_from_times({ 100, 1000, 1000 }, { 150, 2000, 900 }).has_value());
}

TEST(SystemLoadCpu, IdleDeltaAboveTotalClampsToZero) {
  // Inconsistent sample: more idle ticks than kernel plus user ticks.
  const auto percent = cpu_percent_from_times({ 0, 0, 0 }, { 300, 100, 100 });
  ASSERT_TRUE(percent.has_value());
  EXPECT_DOUBLE_EQ(*percent, 0.0);
}

TEST(SystemLoadCpu, LargeCountersKeepPrecision) {
  const std::uint64_t base = 0x7000000000000000ULL;
  const auto percent = cpu_percent_from_times({ base, base, base }, { base + 500, base + 1000, base + 1000 });
  ASSERT_TRUE(percent.has_value());
  EXPECT_DOUBLE_EQ(*percent, 75.0);
}

TEST(SystemLoadGpuParse, ValidInstance) {
  const auto id = system_load::parse_gpu_engine_instance("pid_1234_luid_0x00000000_0x0000A1B2_phys_0_eng_0_engtype_3D");
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->luid_high, 0u);
  EXPECT_EQ(id->luid_low, 0xA1B2u);
  EXPECT_EQ(id->phys, 0u);
  EXPECT_EQ(id->engtype, "3D");
}

TEST(SystemLoadGpuParse, OtherEngineTypesAndIndices) {
  const auto encode = system_load::parse_gpu_engine_instance("pid_4_luid_0x00000001_0xDEADBEEF_phys_2_eng_7_engtype_VideoEncode");
  ASSERT_TRUE(encode.has_value());
  EXPECT_EQ(encode->luid_high, 1u);
  EXPECT_EQ(encode->luid_low, 0xDEADBEEFu);
  EXPECT_EQ(encode->phys, 2u);
  EXPECT_EQ(encode->engtype, "VideoEncode");

  const auto copy = system_load::parse_gpu_engine_instance("pid_0_luid_0x00000000_0x0000A1B2_phys_0_eng_3_engtype_Copy");
  ASSERT_TRUE(copy.has_value());
  EXPECT_EQ(copy->engtype, "Copy");

  const auto decode = system_load::parse_gpu_engine_instance("luid_0x00000000_0x0000A1B2_phys_0_eng_1_engtype_VideoDecode");
  ASSERT_TRUE(decode.has_value());
  EXPECT_EQ(decode->engtype, "VideoDecode");
}

TEST(SystemLoadGpuParse, MissingPartsHaveNoValue) {
  EXPECT_FALSE(system_load::parse_gpu_engine_instance("").has_value());
  EXPECT_FALSE(system_load::parse_gpu_engine_instance("pid_1234").has_value());
  EXPECT_FALSE(system_load::parse_gpu_engine_instance("pid_1_phys_0_eng_0_engtype_3D").has_value());
  EXPECT_FALSE(system_load::parse_gpu_engine_instance("pid_1_luid_0x00000000_0x0000A1B2_eng_0_engtype_3D").has_value());
  EXPECT_FALSE(system_load::parse_gpu_engine_instance("pid_1_luid_0x00000000_0x0000A1B2_phys_0_eng_0").has_value());
  EXPECT_FALSE(system_load::parse_gpu_engine_instance("pid_1_luid_0x00000000_0x0000A1B2_phys_0_eng_0_engtype_").has_value());
}

TEST(SystemLoadGpuParse, MalformedNumbersHaveNoValue) {
  EXPECT_FALSE(system_load::parse_gpu_engine_instance("pid_1_luid_0x0000ZZZZ_0x0000A1B2_phys_0_eng_0_engtype_3D").has_value());
  EXPECT_FALSE(system_load::parse_gpu_engine_instance("pid_1_luid_0x00000000_phys_0_eng_0_engtype_3D").has_value());
  EXPECT_FALSE(system_load::parse_gpu_engine_instance("pid_1_luid_0x00000000_0x0000A1B2_phys_x_eng_0_engtype_3D").has_value());
  EXPECT_FALSE(system_load::parse_gpu_engine_instance("pid_1_luid_0x00000000_0x0000A1B2_phys__eng_0_engtype_3D").has_value());
  // Parts out of order.
  EXPECT_FALSE(system_load::parse_gpu_engine_instance("engtype_3D_phys_0_luid_0x00000000_0x0000A1B2").has_value());
}

namespace {
  std::string
  engine(unsigned luid_low, unsigned phys, unsigned eng, const std::string &type, unsigned pid = 100) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "pid_%u_luid_0x00000000_0x%08X_phys_%u_eng_%u_engtype_%s", pid, luid_low, phys, eng, type.c_str());
    return buffer;
  }
}  // namespace

TEST(SystemLoadGpuAggregate, EmptyInputHasNoValue) {
  EXPECT_FALSE(system_load::aggregate_gpu_engines({}).has_value());
  EXPECT_FALSE(system_load::aggregate_gpu_adapters({}).has_value());
}

TEST(SystemLoadGpuAggregate, OnlyUnparsableNamesHaveNoValue) {
  EXPECT_FALSE(system_load::aggregate_gpu_engines({ { "nonsense", 50.0 } }).has_value());
}

TEST(SystemLoadGpuAggregate, TwoEnginesOfOneTypeAreNotAddedTogether) {
  // two video encoders at 40 % each are two busy engines, not one engine at 80 %
  const std::vector<system_load::gpu_engine_sample_t> samples {
    { "pid_1_luid_0x00000000_0x0000000A_phys_0_eng_3_engtype_VideoEncode", 40.0 },
    { "pid_1_luid_0x00000000_0x0000000A_phys_0_eng_4_engtype_VideoEncode", 40.0 },
  };
  const auto load = system_load::aggregate_gpu_engines(samples);
  ASSERT_TRUE(load.has_value());
  EXPECT_DOUBLE_EQ(*load, 40.0);
}

TEST(SystemLoadGpuAggregate, SumsPerEngineTypeAndTakesTheMaximum) {
  // 3D: 20 + 15 = 35 across two processes; VideoEncode: 30; Copy: 5. The adapter reports 35.
  const std::vector<gpu_engine_sample_t> samples {
    { engine(0xA1B2, 0, 0, "3D", 10), 20.0 },
    { engine(0xA1B2, 0, 0, "3D", 20), 15.0 },
    { engine(0xA1B2, 0, 1, "VideoEncode", 10), 30.0 },
    { engine(0xA1B2, 0, 2, "Copy", 10), 5.0 },
  };
  const auto percent = system_load::aggregate_gpu_engines(samples);
  ASSERT_TRUE(percent.has_value());
  EXPECT_DOUBLE_EQ(*percent, 35.0);
}

TEST(SystemLoadGpuAggregate, BusiestAdapterWins) {
  const std::vector<gpu_engine_sample_t> samples {
    { engine(0x1111, 0, 0, "3D"), 10.0 },
    { engine(0x2222, 0, 0, "3D"), 60.0 },
    { engine(0x2222, 0, 1, "Compute"), 20.0 },
    { engine(0x1111, 0, 1, "Copy"), 25.0 },
  };
  const auto load = system_load::aggregate_gpu_adapters(samples);
  ASSERT_TRUE(load.has_value());
  EXPECT_DOUBLE_EQ(load->percent, 60.0);
  EXPECT_EQ(load->luid_high, 0u);
  EXPECT_EQ(load->luid_low, 0x2222u);
}

TEST(SystemLoadGpuAggregate, PhysicalAdaptersAreSeparate) {
  // Same luid, different phys: 40 and 50 are two adapters, not one engine type summing to 90.
  const std::vector<gpu_engine_sample_t> samples {
    { engine(0x3333, 0, 0, "3D"), 40.0 },
    { engine(0x3333, 1, 0, "3D"), 50.0 },
  };
  const auto percent = system_load::aggregate_gpu_engines(samples);
  ASSERT_TRUE(percent.has_value());
  EXPECT_DOUBLE_EQ(*percent, 50.0);
}

TEST(SystemLoadGpuAggregate, ValuesAboveOneHundredClamp) {
  const std::vector<gpu_engine_sample_t> samples {
    { engine(0x4444, 0, 0, "3D", 1), 80.0 },
    { engine(0x4444, 0, 0, "3D", 2), 70.0 },
  };
  const auto percent = system_load::aggregate_gpu_engines(samples);
  ASSERT_TRUE(percent.has_value());
  EXPECT_DOUBLE_EQ(*percent, 100.0);
}

TEST(SystemLoadGpuAggregate, NegativeAndNonFiniteValuesAreIgnored) {
  const std::vector<gpu_engine_sample_t> samples {
    { engine(0x5555, 0, 0, "3D"), -5.0 },
    { engine(0x5555, 0, 1, "Copy"), std::numeric_limits<double>::quiet_NaN() },
    { engine(0x5555, 0, 2, "Compute"), std::numeric_limits<double>::infinity() },
    { engine(0x5555, 0, 3, "VideoDecode"), 12.5 },
  };
  const auto percent = system_load::aggregate_gpu_engines(samples);
  ASSERT_TRUE(percent.has_value());
  EXPECT_DOUBLE_EQ(*percent, 12.5);
}

TEST(SystemLoadGpuAggregate, IdleAdapterReportsZero) {
  const std::vector<gpu_engine_sample_t> samples { { engine(0x6666, 0, 0, "3D"), 0.0 } };
  const auto percent = system_load::aggregate_gpu_engines(samples);
  ASSERT_TRUE(percent.has_value());
  EXPECT_DOUBLE_EQ(*percent, 0.0);
}

TEST(SystemLoadMemory, PercentFromBytes) {
  const auto percent = system_load::memory_percent_from_bytes(16ULL << 30, 4ULL << 30);
  ASSERT_TRUE(percent.has_value());
  EXPECT_DOUBLE_EQ(*percent, 75.0);
  EXPECT_FALSE(system_load::memory_percent_from_bytes(0, 0).has_value());
  EXPECT_FALSE(system_load::memory_percent_from_bytes(100, 200).has_value());
}

TEST(SystemLoadJson, UnsupportedPlatformIsAllNulls) {
  const auto json = system_load::to_json(system_load::snapshot_t {});
  EXPECT_EQ(json.size(), 7u);
  EXPECT_TRUE(json.at("supported").is_boolean());
  EXPECT_FALSE(json.at("supported").get<bool>());
  for (const char *key : { "cpu_percent", "memory_total", "memory_available", "memory_percent", "gpu_percent", "gpu_name" }) {
    ASSERT_TRUE(json.contains(key)) << key;
    EXPECT_TRUE(json.at(key).is_null()) << key;
  }
}

TEST(SystemLoadJson, FullSnapshot) {
  system_load::snapshot_t snapshot;
  snapshot.supported = true;
  snapshot.cpu_percent = 12.5;
  snapshot.memory_total = 34359738368ULL;
  snapshot.memory_available = 17179869184ULL;
  snapshot.memory_percent = 50.0;
  snapshot.gpu_percent = 3.0;
  snapshot.gpu_name = "Test Adapter";

  const auto json = system_load::to_json(snapshot);
  EXPECT_TRUE(json.at("supported").get<bool>());
  EXPECT_DOUBLE_EQ(json.at("cpu_percent").get<double>(), 12.5);
  EXPECT_TRUE(json.at("memory_total").is_number_unsigned());
  EXPECT_EQ(json.at("memory_total").get<std::uint64_t>(), 34359738368ULL);
  EXPECT_EQ(json.at("memory_available").get<std::uint64_t>(), 17179869184ULL);
  EXPECT_DOUBLE_EQ(json.at("memory_percent").get<double>(), 50.0);
  EXPECT_DOUBLE_EQ(json.at("gpu_percent").get<double>(), 3.0);
  EXPECT_EQ(json.at("gpu_name").get<std::string>(), "Test Adapter");
}

TEST(SystemLoadJson, PartialSnapshotKeepsNullsForMissingValues) {
  // Windows host on the first call: memory known, rate-based values not yet.
  system_load::snapshot_t snapshot;
  snapshot.supported = true;
  snapshot.memory_total = 100;
  snapshot.memory_available = 40;
  snapshot.memory_percent = 60.0;

  const auto json = system_load::to_json(snapshot);
  EXPECT_TRUE(json.at("supported").get<bool>());
  EXPECT_TRUE(json.at("cpu_percent").is_null());
  EXPECT_TRUE(json.at("gpu_percent").is_null());
  EXPECT_TRUE(json.at("gpu_name").is_null());
  EXPECT_EQ(json.at("memory_total").get<std::uint64_t>(), 100u);
  EXPECT_EQ(json.at("memory_available").get<std::uint64_t>(), 40u);
}

TEST(SystemLoadSample, UnsupportedPlatformsReportNothing) {
#ifdef _WIN32
  GTEST_SKIP() << "Windows backend is not exercised by the unit tests";
#else
  const auto snapshot = system_load::sample();
  EXPECT_FALSE(snapshot.supported);
  EXPECT_FALSE(snapshot.cpu_percent.has_value());
  EXPECT_FALSE(snapshot.gpu_name.has_value());
#endif
}

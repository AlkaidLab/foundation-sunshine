/**
 * @file tests/unit/test_logging.cpp
 * @brief Test src/logging.*.
 */
#include <src/logging.h>

#include <array>
#include <fstream>
#include <gtest/gtest.h>
#include <random>

namespace {
  std::array log_levels = {
    std::tuple("Verbose", &verbose),
    std::tuple("Debug", &debug),
    std::tuple("Info", &info),
    std::tuple("Warning", &warning),
    std::tuple("Error", &error),
    std::tuple("Fatal", &fatal),
  };

  constexpr auto log_file = "test_sunshine.log";
}  // namespace

struct LogLevelsTest: testing::TestWithParam<decltype(log_levels)::value_type> {};

INSTANTIATE_TEST_SUITE_P(
  Logging,
  LogLevelsTest,
  testing::ValuesIn(log_levels),
  [](const auto &info) { return std::string(std::get<0>(info.param)); });

TEST_P(LogLevelsTest, FlushesMessageWithItsSeverity) {
  auto [label, plogger] = GetParam();
  auto &logger = *plogger;

  std::random_device rand_dev;
  std::mt19937_64 rand_gen(rand_dev());
  auto test_message = std::to_string(rand_gen()) + std::to_string(rand_gen());
  BOOST_LOG(logger) << test_message;

  logging::log_flush();
  std::ifstream input(log_file);
  ASSERT_TRUE(input.is_open());
  const auto expected = std::string(label) + ": " + test_message;
  for (std::string line; std::getline(input, line);) {
    if (line.ends_with(expected)) {
      return;
    }
  }
  FAIL() << "Missing flushed log record: " << expected;
}

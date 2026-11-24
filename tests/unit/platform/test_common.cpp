/**
 * @file tests/unit/platform/test_common.cpp
 * @brief Test src/platform/common.*.
 */
#include <src/platform/common.h>

#include <algorithm>
#include <boost/asio/ip/host_name.hpp>
#include <gtest/gtest.h>

struct EnvironmentTest: testing::Test {
protected:
  static constexpr auto name = "SUNSHINE_UNIT_TEST_ENV_VAR";

  void
  TearDown() override {
    platf::unset_env(name);
  }
};

TEST_F(EnvironmentTest, SetsAndOverwritesVariable) {
  for (const auto value : { "test_value_0", "test_value_1" }) {
    ASSERT_EQ(platf::set_env(name, value), 0);
    const char *env_value = std::getenv(name);
    ASSERT_NE(env_value, nullptr);
    EXPECT_EQ(std::string(env_value), value);
  }
}

TEST_F(EnvironmentTest, UnsetsExistingVariable) {
  ASSERT_EQ(platf::set_env(name, "test_value"), 0);
  ASSERT_NE(std::getenv(name), nullptr);
  EXPECT_EQ(platf::unset_env(name), 0);
  EXPECT_EQ(std::getenv(name), nullptr);
}

TEST(HostnameTests, MatchesSystemHostnameForAsciiNames) {
  // Windows uses GetHostNameW plus UTF-8 conversion, while Asio uses the narrow
  // system API. They provide independent paths for an ASCII hostname.
  const auto expected = boost::asio::ip::host_name();
  if (!std::all_of(expected.begin(), expected.end(), [](unsigned char c) { return c < 128; })) {
    GTEST_SKIP() << "The narrow system hostname is not ASCII";
  }
  EXPECT_EQ(platf::get_host_name(), expected);
}

/**
 * @file tests/unit/test_display_start_policy.cpp
 * @brief Tests for stream-start display result and retry policy.
 */
#include <src/display_start_policy.h>

#include <array>
#include <gtest/gtest.h>

namespace {

  using result_e = display_device::session_t::configure_result_t::result_e;
  using nvhttp::stream_start::policy::configure_outcome_e;
  using nvhttp::stream_start::policy::deferred_retry_action_e;

  TEST(DisplayStartPolicy, DeferredRetryProbesOnlyAfterSuccessfulConfiguration) {
    EXPECT_EQ(
      nvhttp::stream_start::policy::deferred_retry_action(result_e::deferred_retry),
      deferred_retry_action_e::retry_without_probe);
    EXPECT_EQ(
      nvhttp::stream_start::policy::deferred_retry_action(result_e::vdd_prepare_deferred),
      deferred_retry_action_e::retry_without_probe);
    EXPECT_EQ(
      nvhttp::stream_start::policy::deferred_retry_action(result_e::success),
      deferred_retry_action_e::probe);

    constexpr std::array failures {
      result_e::vdd_not_installed,
      result_e::vdd_unavailable,
      result_e::vdd_create_failed,
      result_e::parse_fail,
      result_e::topology_fail,
      result_e::primary_display_fail,
      result_e::modes_fail,
      result_e::hdr_states_fail,
      result_e::file_save_fail,
      result_e::revert_fail
    };
    for (const auto failure : failures) {
      EXPECT_EQ(
        nvhttp::stream_start::policy::deferred_retry_action(failure),
        deferred_retry_action_e::stop_without_probe);
    }
  }

  TEST(DisplayStartPolicy, RevertFailureKeepsMasterCurrentDisplayFallback) {
    EXPECT_EQ(
      nvhttp::stream_start::policy::classify_configure_result(result_e::revert_fail),
      configure_outcome_e::current_only);
  }

  TEST(DisplayStartPolicy, ParseAndVddFailuresRemainFatal) {
    EXPECT_EQ(
      nvhttp::stream_start::policy::classify_configure_result(result_e::parse_fail),
      configure_outcome_e::fatal);
    EXPECT_EQ(
      nvhttp::stream_start::policy::classify_configure_result(result_e::vdd_not_installed),
      configure_outcome_e::fatal);
    EXPECT_EQ(
      nvhttp::stream_start::policy::classify_configure_result(result_e::vdd_unavailable),
      configure_outcome_e::fatal);
    EXPECT_EQ(
      nvhttp::stream_start::policy::classify_configure_result(result_e::vdd_create_failed),
      configure_outcome_e::fatal);
  }

}  // namespace

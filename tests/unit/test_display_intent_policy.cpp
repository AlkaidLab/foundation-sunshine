/**
 * @file tests/unit/test_display_intent_policy.cpp
 * @brief Tests for requested-display resolution under CCD enumeration states.
 */
#include <src/display_device/display_intent_policy.h>

#include <gtest/gtest.h>
#include <utility>

namespace {

  using display_device::device_enumeration_result_t;
  using display_device::device_info_t;
  using display_device::device_state_e;
  using display_device::hdr_state_e;
  using display_device::requested_display_resolution_e;

  constexpr auto failed = device_enumeration_result_t::status_e::failed;
  constexpr auto success = device_enumeration_result_t::status_e::success;

  device_info_t
  device(std::string friendly_name) {
    return { R"(\\.\DISPLAY7)", std::move(friendly_name), device_state_e::primary, hdr_state_e::disabled };
  }

  TEST(DisplayIntentPolicy, QueryFailurePreservesClientPhysicalRequest) {
    const device_enumeration_result_t enumeration { failed, {} };
    EXPECT_EQ(
      display_device::classify_requested_display(enumeration, "physical-id", true, false, "Zako HDR"),
      requested_display_resolution_e::preserve_physical);
  }

  TEST(DisplayIntentPolicy, OnlySuccessfulEnumerationCanDriveVddPreparation) {
    EXPECT_FALSE(display_device::display_enumeration_is_reliable({ failed, {} }));
    // A successful empty snapshot proves a genuinely headless host and is safe
    // to use when deciding whether a first VDD monitor may be created.
    EXPECT_TRUE(display_device::display_enumeration_is_reliable({ success, {} }));
  }

  TEST(DisplayIntentPolicy, SuccessfulEmptyEnumerationRejectsMissingClientTarget) {
    const device_enumeration_result_t enumeration { success, {} };
    EXPECT_EQ(
      display_device::classify_requested_display(enumeration, "physical-id", true, false, "Zako HDR"),
      requested_display_resolution_e::unavailable);
  }

  TEST(DisplayIntentPolicy, SuccessfulEnumerationFallsBackForStaleHostTarget) {
    const device_enumeration_result_t enumeration { success, { { "other-id", device("Physical") } } };
    EXPECT_EQ(
      display_device::classify_requested_display(enumeration, "stale-id", false, false, "Zako HDR"),
      requested_display_resolution_e::fallback_primary);
  }

  TEST(DisplayIntentPolicy, FoundPhysicalTargetIsPreserved) {
    const device_enumeration_result_t enumeration { success, { { "physical-id", device("Physical") } } };
    EXPECT_EQ(
      display_device::classify_requested_display(enumeration, "physical-id", true, false, "Zako HDR"),
      requested_display_resolution_e::preserve_physical);
  }

  TEST(DisplayIntentPolicy, ExplicitAndEnumeratedVirtualDisplaysResolveToVdd) {
    const device_enumeration_result_t failed_enumeration { failed, {} };
    EXPECT_EQ(
      display_device::classify_requested_display(failed_enumeration, "ZakoHDR", true, true, "Zako HDR"),
      requested_display_resolution_e::vdd);

    const device_enumeration_result_t enumeration { success, { { "vdd-id", device("Zako HDR") } } };
    EXPECT_EQ(
      display_device::classify_requested_display(enumeration, "vdd-id", true, false, "Zako HDR"),
      requested_display_resolution_e::vdd);
  }

}  // namespace

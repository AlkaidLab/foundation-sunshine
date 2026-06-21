#include "../tests_common.h"

#ifdef _WIN32

#include <string>
#include <utility>

#include "src/display_device/vdd_utils.h"
#include "src/globals.h"

namespace {
  display_device::device_info_t
  make_device(std::string friendly_name, display_device::device_state_e state) {
    return {
      "\\\\.\\DISPLAY1",
      std::move(friendly_name),
      state,
      display_device::hdr_state_e::unknown
    };
  }
}  // namespace

TEST(VddPrepSafety, RequiresPreVddSnapshotForTopologyPrep) {
  using display_device::parsed_config_t;

  EXPECT_FALSE(display_device::vdd_utils::vdd_prep_requires_pre_vdd_snapshot(parsed_config_t::vdd_prep_e::no_operation));
  EXPECT_FALSE(display_device::vdd_utils::vdd_prep_requires_pre_vdd_snapshot(parsed_config_t::vdd_prep_e::vdd_as_primary));
  EXPECT_FALSE(display_device::vdd_utils::vdd_prep_requires_pre_vdd_snapshot(parsed_config_t::vdd_prep_e::vdd_as_secondary));
  EXPECT_TRUE(display_device::vdd_utils::vdd_prep_requires_pre_vdd_snapshot(parsed_config_t::vdd_prep_e::display_off));
}

TEST(VddPrepSafety, DetectsRestorablePhysicalDisplaySnapshot) {
  using namespace display_device;

  EXPECT_FALSE(vdd_utils::has_active_physical_display_snapshot({}));
  EXPECT_FALSE(vdd_utils::has_physical_display_snapshot({}));

  device_info_map_t vdd_only {
    { "vdd", make_device(ZAKO_NAME, device_state_e::primary) },
  };
  EXPECT_FALSE(vdd_utils::has_active_physical_display_snapshot(vdd_only));
  EXPECT_FALSE(vdd_utils::has_physical_display_snapshot(vdd_only));

  device_info_map_t inactive_physical {
    { "physical", make_device("F32D80U", device_state_e::inactive) },
  };
  EXPECT_FALSE(vdd_utils::has_active_physical_display_snapshot(inactive_physical));
  EXPECT_TRUE(vdd_utils::has_physical_display_snapshot(inactive_physical));

  device_info_map_t active_physical {
    { "physical", make_device("F32D80U", device_state_e::active) },
  };
  EXPECT_TRUE(vdd_utils::has_active_physical_display_snapshot(active_physical));
  EXPECT_TRUE(vdd_utils::has_physical_display_snapshot(active_physical));

  device_info_map_t primary_physical {
    { "physical", make_device("F32D80U", device_state_e::primary) },
  };
  EXPECT_TRUE(vdd_utils::has_active_physical_display_snapshot(primary_physical));
  EXPECT_TRUE(vdd_utils::has_physical_display_snapshot(primary_physical));
}

TEST(VddPrepSafety, BlocksOnlyUnsafeDisplayOffBaselines) {
  using namespace display_device;
  using result_e = vdd_utils::vdd_prep_result_e;

  EXPECT_EQ(
    vdd_utils::apply_vdd_prep("vdd", parsed_config_t::vdd_prep_e::display_off, {}, false),
    result_e::safety_blocked);

  device_info_map_t inactive_physical {
    { "physical", make_device("F32D80U", device_state_e::inactive) },
    { "vdd", make_device(ZAKO_NAME, device_state_e::primary) },
  };
  EXPECT_EQ(
    vdd_utils::apply_vdd_prep("vdd", parsed_config_t::vdd_prep_e::display_off, inactive_physical, true),
    result_e::safety_blocked);

  device_info_map_t vdd_only {
    { "vdd", make_device(ZAKO_NAME, device_state_e::primary) },
  };
  EXPECT_EQ(
    vdd_utils::apply_vdd_prep("vdd", parsed_config_t::vdd_prep_e::display_off, vdd_only, true),
    result_e::success);
}

TEST(VddRequestSafety, NoOperationBlocksOnlyAutomaticVddFallback) {
  using namespace display_device;
  using device_prep_e = parsed_config_t::device_prep_e;
  using decision_e = vdd_request_decision_e;

  display_request_t config_missing_display {
    "missing",
    display_request_t::source_e::config,
    false
  };
  EXPECT_FALSE(is_explicit_vdd_request(config_missing_display, false));
  EXPECT_EQ(resolve_vdd_request(config_missing_display, false, false, device_prep_e::no_operation), decision_e::blocked_automatic_fallback);
  EXPECT_EQ(resolve_vdd_request(config_missing_display, false, false, device_prep_e::ensure_active), decision_e::use_vdd);
  EXPECT_EQ(resolve_vdd_request(config_missing_display, true, false, device_prep_e::no_operation), decision_e::use_requested_display);

  display_request_t explicit_vdd {
    "missing",
    display_request_t::source_e::config,
    true
  };
  EXPECT_TRUE(is_explicit_vdd_request(explicit_vdd, false));
  EXPECT_EQ(resolve_vdd_request(explicit_vdd, false, false, device_prep_e::no_operation), decision_e::use_vdd);

  display_request_t client_physical_missing {
    "missing",
    display_request_t::source_e::client,
    false
  };
  EXPECT_FALSE(client_physical_missing.allows_vdd_fallback());
  EXPECT_EQ(resolve_vdd_request(client_physical_missing, false, false, device_prep_e::ensure_active), decision_e::use_requested_display);
}

#endif

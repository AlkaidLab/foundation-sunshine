#include "src/display_device/vdd_mode_list.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {
  using display_device::resolution_t;
  namespace mode_list = display_device::vdd_mode_list;
  using mode_list::vdd_mode_t;

  const std::vector<resolution_t> configured {
    { 1920, 1080 },
    { 2560, 1440 },
  };
  const std::vector<unsigned int> refresh_rates { 60, 120 };

  std::wstring
  command_for(const vdd_mode_t *requested,
    unsigned int session_hz = 120,
    const std::vector<resolution_t> &sizes = configured,
    const std::vector<unsigned int> &rates = refresh_rates,
    const mode_list::limits_t &limits = {}) {
    return mode_list::format_setmodes_command(mode_list::plan_modes(sizes, rates, session_hz, requested, limits).modes);
  }

  std::vector<resolution_t>
  many_sizes(unsigned int count) {
    std::vector<resolution_t> sizes;
    for (unsigned int i = 0; i < count; ++i) {
      sizes.push_back({ 1280 + 64 * i, 720 });
    }
    return sizes;
  }

  TEST(VddModeList, FormatsConfiguredProductThenRequestedMode) {
    const vdd_mode_t requested { { 3000, 2000 }, 120 };
    EXPECT_EQ(command_for(&requested),
      L"SETMODES 1920x1080x120,1920x1080x60,2560x1440x60,2560x1440x120,3000x2000x120");
    // A configured size at the session rate adds nothing.
    const vdd_mode_t configured_mode { { 2560, 1440 }, 120 };
    EXPECT_EQ(command_for(&configured_mode), L"SETMODES 1920x1080x120,1920x1080x60,2560x1440x60,2560x1440x120");
    EXPECT_TRUE(mode_list::format_setmodes_command({}).empty());
  }

  TEST(VddModeList, UnconfiguredSessionRateIsAppendedToTheProduct) {
    EXPECT_EQ(command_for(nullptr, 90),
      L"SETMODES 1920x1080x90,1920x1080x60,1920x1080x120,2560x1440x60,2560x1440x120,2560x1440x90");
  }

  TEST(VddModeList, RepeatedRequestIsByteIdentical) {
    const vdd_mode_t requested { { 3000, 2000 }, 120 };
    EXPECT_EQ(command_for(&requested), command_for(&requested));
  }

  TEST(VddModeList, RequestAlwaysFitsWhenConfiguredModesFillTheBudget) {
    // 16 sizes x 6 rates = 96 configured modes already fill the budget.
    const auto sizes = many_sizes(16);
    const std::vector<unsigned int> rates { 30, 60, 90, 120, 144, 165 };
    const vdd_mode_t requested { { 3088, 1880 }, 120 };

    const auto plan = mode_list::plan_modes(sizes, rates, 120, &requested);
    EXPECT_EQ(plan.modes.size(), mode_list::kMaxTotalModes);
    EXPECT_EQ(plan.configured_left_out, 1u);
    EXPECT_TRUE(mode_list::contains_mode(plan.modes, requested));
    // Every configured size stays available at the session rate.
    for (const auto &size : sizes) {
      EXPECT_TRUE(mode_list::contains_mode(plan.modes, { size, 120 })) << size.width;
    }
    // The configured mode that dropped out is the last one of the last
    // non-session rate, in configured order.
    EXPECT_FALSE(mode_list::contains_mode(plan.modes, { sizes.back(), 165 }));
    // The requested mode is emitted after the configured product.
    EXPECT_TRUE(mode_list::same_mode(plan.modes.back(), requested));
  }

  TEST(VddModeList, ConfiguredModesAtTheSessionRateWinOverOtherRates) {
    // 20 sizes x 6 rates = 120 configured modes: the session rate (60) and
    // the earliest other rates fill the budget first.
    const auto sizes = many_sizes(20);
    const std::vector<unsigned int> rates { 30, 60, 90, 120, 144, 165 };
    const vdd_mode_t requested { { 1280, 720 }, 60 };
    const auto plan = mode_list::plan_modes(sizes, rates, 60, &requested);
    ASSERT_EQ(plan.modes.size(), mode_list::kMaxTotalModes);
    EXPECT_EQ(plan.configured_left_out, 24u);
    for (const auto &size : sizes) {
      EXPECT_TRUE(mode_list::contains_mode(plan.modes, { size, 60 }));
      EXPECT_TRUE(mode_list::contains_mode(plan.modes, { size, 30 }));
      EXPECT_TRUE(mode_list::contains_mode(plan.modes, { size, 90 }));
      EXPECT_TRUE(mode_list::contains_mode(plan.modes, { size, 120 }));
      EXPECT_FALSE(mode_list::contains_mode(plan.modes, { size, 165 }));
    }
  }

  TEST(VddModeList, CommandLengthLimitLeavesOutConfiguredModes) {
    const vdd_mode_t requested { { 3000, 2000 }, 120 };
    mode_list::limits_t limits;
    // Room for the requested mode plus one configured mode only.
    limits.max_command_chars = std::wstring(L"SETMODES 1920x1080x120,3000x2000x120").size();
    const auto plan = mode_list::plan_modes(configured, refresh_rates, 120, &requested, limits);
    EXPECT_EQ(mode_list::format_setmodes_command(plan.modes), L"SETMODES 1920x1080x120,3000x2000x120");
    EXPECT_EQ(plan.configured_left_out, 3u);
  }

  TEST(VddModeList, SmallestModeLeadsTheList) {
    // The driver reports index 0 as the native mode; Windows scales any source
    // smaller than native in both dimensions, so the smallest size leads.
    const std::vector<resolution_t> sizes { { 1280, 720 }, { 1920, 1080 } };
    const std::vector<unsigned int> rates { 60, 120 };

    // 1458x540 (787,320 px) is smaller than 1280x720 (921,600 px).
    const vdd_mode_t wide { { 1458, 540 }, 120 };
    EXPECT_EQ(command_for(&wide, 120, sizes, rates),
      L"SETMODES 1458x540x120,1280x720x60,1280x720x120,1920x1080x60,1920x1080x120");

    // Without a smaller request, the smallest configured size leads at the session rate.
    const vdd_mode_t large { { 1920, 1080 }, 120 };
    EXPECT_EQ(command_for(&large, 120, sizes, rates),
      L"SETMODES 1280x720x120,1280x720x60,1920x1080x60,1920x1080x120");

    // No listed size is smaller than index 0 in both dimensions.
    const vdd_mode_t small { { 960, 540 }, 120 };
    const auto modes = mode_list::plan_modes(sizes, rates, 120, &small).modes;
    ASSERT_FALSE(modes.empty());
    EXPECT_TRUE(mode_list::same_mode(modes.front(), small));
    for (const auto &mode : modes) {
      EXPECT_FALSE(mode.resolution.width < modes.front().resolution.width && mode.resolution.height < modes.front().resolution.height);
    }
  }

  TEST(VddModeList, SmallestModeTieBreakIsDeterministic) {
    // Equal areas: the narrower size wins; same size prefers the session rate.
    const std::vector<vdd_mode_t> modes {
      { { 1200, 600 }, 60 },
      { { 800, 900 }, 60 },
      { { 800, 900 }, 120 },
    };
    EXPECT_EQ(mode_list::smallest_mode_index(modes, 120), 2u);
    EXPECT_EQ(mode_list::smallest_mode_index(modes, 90), 1u);
  }
}  // namespace

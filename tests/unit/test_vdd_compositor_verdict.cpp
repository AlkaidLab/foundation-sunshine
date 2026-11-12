/**
 * @file tests/unit/test_vdd_compositor_verdict.cpp
 * @brief Tests for the "may we conclude the compositor cannot use this
 *        virtual display" decision.
 * @details A status-forced connector takes seconds to surface, and the query
 *          that would confirm it stalls behind the compositor's own
 *          re-enumeration. Rang a stream down on a query that never answered
 *          and every slow compositor looks like an unusable one.
 */
// local includes
#include "src/display_device/vdd_utils.h"

// standard includes
#include <type_traits>

// third-party includes
#include <gtest/gtest.h>

namespace {
  using display_device::vdd_utils::compositor_verdict;
  using display_device::vdd_utils::compositor_verdict_e;
}  // namespace

TEST(VddCompositorVerdict, ListedConnectorIsLiveWhateverTheQueryHistoryWas) {
  EXPECT_EQ(compositor_verdict(true, true, true), compositor_verdict_e::live);
  // Even a connector that only answered late is live once it is listed.
  EXPECT_EQ(compositor_verdict(true, true, false), compositor_verdict_e::live);
  EXPECT_EQ(compositor_verdict(false, true, false), compositor_verdict_e::live);
}

TEST(VddCompositorVerdict, AnsweredAndAbsentIsTheOnlyReasonToRefuse) {
  EXPECT_EQ(compositor_verdict(true, false, true), compositor_verdict_e::refuse);
}

TEST(VddCompositorVerdict, AQueryThatNeverAnsweredIsNotEvidence) {
  // The regression this locks: one timed-out kscreen-doctor used to be enough
  // to report "never surfaced" and fail the stream.
  EXPECT_EQ(compositor_verdict(true, false, false), compositor_verdict_e::unverified);
}

TEST(VddCompositorVerdict, SessionsWithoutAnObservableCompositorAreNeverRefused) {
  // No compositor to ask (niri, wlroots without KScreen): the DRM-level state
  // is all there is, and it must not be second-guessed by this decision.
  EXPECT_EQ(compositor_verdict(false, false, false), compositor_verdict_e::unverified);
  EXPECT_EQ(compositor_verdict(false, false, true), compositor_verdict_e::unverified);
}

TEST(VddCompositorVerdict, TheSurfaceBudgetOutlastsACompositorReenumerating) {
  // Observed worst case on KWin 6.8 was 6.5-14s to surface a forced connector;
  // a shorter budget is the bug, not a tuning preference.
  static_assert(display_device::vdd_utils::kCompositorSurfaceBudget >= std::chrono::seconds { 15 },
    "the compositor surface budget must cover a compositor re-enumerating its DRM resources");
  static_assert(display_device::vdd_utils::kCompositorProbeInterval >= std::chrono::milliseconds { 200 },
    "each probe forks kscreen-doctor and must not hammer the compositor it waits for");
}

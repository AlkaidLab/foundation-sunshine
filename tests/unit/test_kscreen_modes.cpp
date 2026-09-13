/**
 * @file tests/unit/test_kscreen_modes.cpp
 * @brief Advertised-mode accessor used to verify VDD mode publication.
 *
 * The interesting part of this helper (parsing kscreen-doctor -o against a live
 * compositor) cannot be unit-tested; these pin the contract callers depend on:
 * an unknown or unqueryable output yields an empty list, which the VDD backend
 * reads as "unknown" and never as "not advertised".
 */
#include <gtest/gtest.h>

#include <string>

#include "src/platform/linux/kscreen_modes.h"

TEST(KscreenModes, UnknownOutputReportsNoModes) {
  EXPECT_TRUE(platf::kscreen::advertised_modes("SUNSHINE-NO-SUCH-OUTPUT").empty());
}

TEST(KscreenModes, EmptyNameReportsNoModes) {
  EXPECT_TRUE(platf::kscreen::advertised_modes("").empty());
}

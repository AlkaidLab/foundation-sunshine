#include "tools/installer_guide.h"

#include <gtest/gtest.h>

using installer_guide::read_host_guide;
using installer_guide::start_guide;

namespace {
  constexpr const char *kFinished = "sunshine_name = PC\nguide_done = intro,account\nguide_finished = true\nguide_closed = 2026.1116.1\n";
  constexpr const char *kClosedOnly = "guide_done = intro\nguide_finished = false\nguide_closed = 2026.1116.1\n";
  constexpr const char *kStarted = "guide_done = intro\nguide_finished = false\nguide_closed =\nguide_active = d1@1760000000\n";
}  // namespace

TEST(InstallerGuide, readsTheTwoKeysAndNothingElse) {
  EXPECT_TRUE(read_host_guide(kFinished).finished);
  EXPECT_TRUE(read_host_guide(kFinished).closed);
  EXPECT_FALSE(read_host_guide(kClosedOnly).finished);
  EXPECT_TRUE(read_host_guide(kClosedOnly).closed);
  EXPECT_FALSE(read_host_guide(kStarted).finished);
  EXPECT_FALSE(read_host_guide(kStarted).closed);
  EXPECT_FALSE(read_host_guide("").finished);
  EXPECT_FALSE(read_host_guide("guide_finished\nguide_closed").closed);
  // spaces, CRLF and the value 1 as the host writes them
  EXPECT_TRUE(read_host_guide("  guide_finished  =  1 \r\n").finished);
  EXPECT_TRUE(read_host_guide("guide_finished = true\r\n").finished);
  EXPECT_FALSE(read_host_guide("guide_finished = false\r\n").finished);
  EXPECT_FALSE(read_host_guide("xguide_finished = true\n").finished);
}

TEST(InstallerGuide, readsDuplicatesEmptyValuesAndCommentsLikeTheHost) {
  // config::parse_config keeps the first non-empty value of a key and skips a line without a value
  EXPECT_TRUE(read_host_guide("guide_finished = true\nguide_finished = false\n").finished);
  EXPECT_FALSE(read_host_guide("guide_finished = false\nguide_finished = true\n").finished);
  EXPECT_TRUE(read_host_guide("guide_finished =\nguide_finished = true\n").finished);
  EXPECT_FALSE(read_host_guide("guide_closed =\nguide_closed=\n").closed);
  EXPECT_TRUE(read_host_guide("guide_closed =\nguide_closed = 2026.1116\n").closed);
  // a trailing comment is not part of the value, a commented-out line is not a line
  EXPECT_TRUE(read_host_guide("guide_finished = true # done on the phone\n").finished);
  EXPECT_FALSE(read_host_guide("# guide_finished = true\n").finished);
}

TEST(InstallerGuide, aFreshInstallStartsTheGuideOnlyUntilItIsFinishedOrClosed) {
  EXPECT_TRUE(start_guide(false, false, ""));
  EXPECT_TRUE(start_guide(false, false, kStarted));
  EXPECT_FALSE(start_guide(false, false, kFinished));
  EXPECT_FALSE(start_guide(false, false, kClosedOnly));
}

TEST(InstallerGuide, anUpgradeOrMaintenanceRunNeverStartsTheFullGuide) {
  // the Panel decides: once per version, with what is new; so the flag is not passed whatever the host recorded
  for (const char *conf : {"", kStarted, kClosedOnly, kFinished}) {
    EXPECT_FALSE(start_guide(true, false, conf));
    EXPECT_FALSE(start_guide(false, true, conf));
    EXPECT_FALSE(start_guide(true, true, conf));
  }
}

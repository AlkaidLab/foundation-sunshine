#include <algorithm>

#include <gtest/gtest.h>

#include "src/logging_severity.h"
#include "src/platform/windows/display_device/settings_restore.h"

boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> warning;
boost::log::sources::severity_logger<int> error;
boost::log::sources::severity_logger<int> fatal;

namespace display_device {
  namespace {

    const display_mode_t mode { { 2560, 1440 }, { 60, 1 } };
    const active_topology_t dummy_only { { "dummy" } };
    const active_topology_t extended { { "dummy" }, { "monitor" } };
    const active_topology_t cloned { { "dummy", "monitor" } };
    const active_topology_t monitor_only { { "monitor" } };

    struct display_api_mock_t {
      active_topology_t current_topology;
      std::vector<active_topology_t> topology_writes;
      device_display_mode_map_t mode_writes;
      hdr_state_map_t hdr_writes;
      std::vector<std::string> primary_writes;
      bool modes_succeed = true;
    } display_api;

    class DisplaySettingsRestore: public testing::Test {
    protected:
      void
      SetUp() override {
        display_api = {};
      }
    };

  }  // namespace

  active_topology_t
  get_current_topology() {
    return display_api.current_topology;
  }

  bool
  is_topology_the_same(const active_topology_t &a, const active_topology_t &b) {
    return std::is_permutation(a.begin(), a.end(), b.begin(), b.end(), [](const auto &lhs, const auto &rhs) {
      return std::is_permutation(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
    });
  }

  bool
  is_topology_valid(const active_topology_t &topology) {
    return !topology.empty();
  }

  std::unordered_set<std::string>
  get_device_ids_from_topology(const active_topology_t &topology) {
    std::unordered_set<std::string> ids;
    for (const auto &group : topology) {
      ids.insert(group.begin(), group.end());
    }
    return ids;
  }

  std::unordered_set<std::string>
  get_newly_enabled_devices_from_topology(const active_topology_t &previous, const active_topology_t &next) {
    auto enabled = get_device_ids_from_topology(next);
    for (const auto &id : get_device_ids_from_topology(previous)) {
      enabled.erase(id);
    }
    return enabled;
  }

  bool
  set_topology(const active_topology_t &topology) {
    display_api.topology_writes.push_back(topology);
    display_api.current_topology = topology;
    return true;
  }

  bool
  set_display_modes(const device_display_mode_map_t &modes) {
    display_api.mode_writes = modes;
    return display_api.modes_succeed;
  }

  bool
  set_hdr_states(const hdr_state_map_t &states) {
    display_api.hdr_writes = states;
    return true;
  }

  bool
  set_as_primary_device(const std::string &device_id) {
    display_api.primary_writes.push_back(device_id);
    return true;
  }

  // googletest-primer-d72f9c8-20261121, Simple Tests and Test Fixtures.
  TEST_F(DisplaySettingsRestore, PhysicalDisplayReturnsWithoutReplayingSavedTopology) {
    display_api.current_topology = extended;
    display_restore_state_t saved { { dummy_only, dummy_only }, {}, { { "dummy", mode } }, { { "dummy", hdr_state_e::disabled } } };
    bool data_modified = false;

    EXPECT_TRUE(restore_display_settings(saved, saved.topology, data_modified).success);

    EXPECT_TRUE(display_api.topology_writes.empty());
    EXPECT_EQ(display_api.current_topology, extended);
    EXPECT_EQ(display_api.mode_writes.size(), 1u);
    EXPECT_TRUE(display_api.mode_writes.contains("dummy"));
    EXPECT_EQ(display_api.hdr_writes, (hdr_state_map_t { { "dummy", hdr_state_e::disabled } }));
    EXPECT_TRUE(saved.original_modes.empty());
    EXPECT_TRUE(saved.original_hdr_states.empty());
    EXPECT_TRUE(data_modified);
  }

  TEST_F(DisplaySettingsRestore, DoesNotRestoreModeAcrossChangedCloneMembership) {
    display_api.current_topology = cloned;
    display_restore_state_t saved { { extended, extended }, {}, { { "dummy", mode }, { "monitor", mode } }, {} };
    bool data_modified = false;

    EXPECT_TRUE(restore_display_settings(saved, saved.topology, data_modified).success);

    EXPECT_TRUE(display_api.topology_writes.empty());
    EXPECT_TRUE(display_api.mode_writes.empty());
    EXPECT_TRUE(saved.original_modes.empty());
    EXPECT_TRUE(data_modified);
  }

  TEST_F(DisplaySettingsRestore, LeavesInactiveTargetUnchanged) {
    display_api.current_topology = monitor_only;
    display_restore_state_t saved { { dummy_only, dummy_only }, "dummy", { { "dummy", mode } }, { { "dummy", hdr_state_e::disabled } } };
    bool data_modified = false;

    EXPECT_TRUE(restore_display_settings(saved, saved.topology, data_modified).success);

    EXPECT_TRUE(display_api.topology_writes.empty());
    EXPECT_TRUE(display_api.mode_writes.empty());
    EXPECT_TRUE(display_api.hdr_writes.empty());
    EXPECT_TRUE(display_api.primary_writes.empty());
    EXPECT_TRUE(data_modified);
  }

  TEST_F(DisplaySettingsRestore, UnavailableActivePathsKeepSnapshotForRetry) {
    display_restore_state_t saved { { dummy_only, dummy_only }, {}, { { "dummy", mode } }, {} };
    bool data_modified = false;

    EXPECT_FALSE(restore_display_settings(saved, saved.topology, data_modified).success);

    EXPECT_TRUE(display_api.topology_writes.empty());
    EXPECT_TRUE(display_api.mode_writes.empty());
    EXPECT_TRUE(saved.original_modes.contains("dummy"));
    EXPECT_FALSE(data_modified);
  }

  TEST_F(DisplaySettingsRestore, RetriesOnlyTheSettingThatFailed) {
    display_api.current_topology = extended;
    display_api.modes_succeed = false;
    display_restore_state_t saved { { dummy_only, dummy_only }, {}, { { "dummy", mode } }, { { "dummy", hdr_state_e::disabled } } };
    bool data_modified = false;

    EXPECT_FALSE(restore_display_settings(saved, saved.topology, data_modified).success);

    EXPECT_TRUE(display_api.topology_writes.empty());
    EXPECT_TRUE(saved.original_modes.contains("dummy"));
    EXPECT_TRUE(saved.original_hdr_states.empty());
    EXPECT_TRUE(data_modified);

    display_api.modes_succeed = true;
    display_api.hdr_writes.clear();
    EXPECT_TRUE(restore_display_settings(saved, saved.topology, data_modified).success);

    EXPECT_TRUE(display_api.topology_writes.empty());
    EXPECT_TRUE(display_api.hdr_writes.empty());
    EXPECT_TRUE(saved.original_modes.empty());
  }

  TEST_F(DisplaySettingsRestore, StillRestoresAfterVirtualDisplayIsRemovedFromWorkingPaths) {
    const active_topology_t with_virtual_display { { "dummy" }, { "virtual" } };
    display_api.current_topology = extended;
    display_restore_state_t saved { { dummy_only, with_virtual_display }, {}, {}, {} };
    const topology_pair_t after_virtual_display_removal { dummy_only, dummy_only };
    bool data_modified = false;

    EXPECT_TRUE(restore_display_settings(saved, after_virtual_display_removal, data_modified).success);

    EXPECT_EQ(display_api.topology_writes, (std::vector<active_topology_t> { dummy_only }));
    EXPECT_EQ(saved.topology.modified, with_virtual_display);
    EXPECT_TRUE(data_modified);
  }

  TEST_F(DisplaySettingsRestore, RevertsTopologyThatTheSessionActuallyChanged) {
    display_api.current_topology = monitor_only;
    display_restore_state_t saved { { extended, dummy_only }, {}, { { "dummy", mode } }, {} };
    bool data_modified = false;

    const auto result = restore_display_settings(saved, saved.topology, data_modified);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(display_api.topology_writes, (std::vector<active_topology_t> { dummy_only, extended }));
    EXPECT_EQ(result.active_topology, extended);
    EXPECT_TRUE(display_api.mode_writes.contains("dummy"));
    EXPECT_TRUE(data_modified);
  }

}  // namespace display_device

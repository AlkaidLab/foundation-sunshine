#pragma once

#include "settings_topology.h"

namespace display_device {

  struct display_restore_state_t {
    topology_pair_t topology;  // Original and session topologies, including any virtual displays.
    std::string original_primary_display;
    device_display_mode_map_t original_modes;
    hdr_state_map_t original_hdr_states;
  };

  struct display_restore_result_t {
    active_topology_t active_topology;
    std::unordered_set<std::string> newly_enabled_devices;
    bool success = true;
  };

  display_restore_result_t
  restore_display_settings(display_restore_state_t &data, const topology_pair_t &restore_topology, bool &data_modified);

}  // namespace display_device

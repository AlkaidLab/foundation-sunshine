// standard includes
#include <algorithm>

// local includes
#include "settings_restore.h"
#include "src/display_device/to_string.h"
#include "src/logging_severity.h"  // boost-log-sources-1.92-20261121, Logging sources.

namespace display_device {
  namespace {

    /**
     * @brief Keep snapshots that can be restored without changing the live topology.
     */
    void
    keep_live_settings(display_restore_state_t &data, const active_topology_t &current_topology, bool &data_modified) {
      const auto active_ids = get_device_ids_from_topology(current_topology);
      const bool removed_hdr = std::erase_if(data.original_hdr_states, [&](const auto &entry) {
        return !active_ids.contains(entry.first);
      }) != 0;

      // A clone group shares a source mode. Only restore a group's modes if its
      // membership is unchanged and we have a snapshot for every member.
      // ms-setdisplayconfig-20261121, Remarks (shared source modes).
      std::unordered_set<std::string> restorable_modes;
      for (const auto &group : current_topology) {
        const bool same_group = std::ranges::any_of(data.topology.modified, [&](const auto &saved_group) {
          return std::is_permutation(group.begin(), group.end(), saved_group.begin(), saved_group.end());
        });
        if (same_group && std::ranges::all_of(group, [&](const auto &id) { return data.original_modes.contains(id); })) {
          restorable_modes.insert(group.begin(), group.end());
        }
      }
      const bool removed_modes = std::erase_if(data.original_modes, [&](const auto &entry) {
        return !restorable_modes.contains(entry.first);
      }) != 0;

      const bool removed_primary = !data.original_primary_display.empty() && !active_ids.contains(data.original_primary_display);
      if (removed_primary) {
        data.original_primary_display.clear();
      }
      if (removed_hdr || removed_modes || removed_primary) {
        BOOST_LOG(info) << "Skipping saved display settings that do not match the active display paths";
        data_modified = true;
      }
    }

  }  // namespace

  display_restore_result_t
  restore_display_settings(display_restore_state_t &data, const topology_pair_t &restore_topology, bool &data_modified) {
    const bool topology_changed = !is_topology_the_same(data.topology.initial, data.topology.modified);
    const bool restore_settings = !data.original_primary_display.empty() || !data.original_modes.empty() || !data.original_hdr_states.empty();
    if (!topology_changed && !restore_settings) {
      return {};
    }

    display_restore_result_t result { get_current_topology(), {} };
    const auto switch_topology = [&](const active_topology_t &topology) {
      if (!set_topology(topology)) {
        return false;
      }
      result.newly_enabled_devices.merge(get_newly_enabled_devices_from_topology(result.active_topology, topology));
      result.active_topology = topology;
      return true;
    };

    if (restore_settings) {
      if (!topology_changed) {
        // The live CCD paths can change independently through hot plug (ms-ccd-20261121, user-mode CCD).
        // QueryDisplayConfig reports them separately from possible paths (ms-querydisplayconfig-20261121, Parameters).
        if (result.active_topology.empty()) {
          result.success = false;
          return result;
        }
        keep_live_settings(data, result.active_topology, data_modified);
      }
      else if (!is_topology_valid(restore_topology.modified)) {
        BOOST_LOG(warning) << "Modified topology invalid, skipping restoration of HDR, modes, and primary display";
        data.original_hdr_states.clear();
        data.original_modes.clear();
        data.original_primary_display.clear();
        data_modified = true;
      }
      else if (!switch_topology(restore_topology.modified)) {
        BOOST_LOG(error) << "Cannot switch to the topology to undo changes!";
        result.success = false;
      }

      const auto restore = [&](auto &saved, auto setter, const char *label) {
        if (saved.empty()) {
          return;
        }
        BOOST_LOG(info) << "Changing back " << label << " to: " << to_string(saved);
        if (setter(saved)) {
          saved.clear();
          data_modified = true;
        }
        else {
          result.success = false;
        }
      };

      if (result.success) {
        restore(data.original_hdr_states, set_hdr_states, "the HDR states");
        restore(data.original_modes, set_display_modes, "the display modes");
        if (!data.original_primary_display.empty()) {
          BOOST_LOG(info) << "Changing back the primary device to: " << data.original_primary_display;
          if (set_as_primary_device(data.original_primary_display)) {
            data.original_primary_display.clear();
            data_modified = true;
          }
          else {
            result.success = false;
          }
        }
      }
    }

    if (!topology_changed) {
      return result;
    }

    if (is_topology_valid(restore_topology.initial)) {
      BOOST_LOG(info) << "Changing display topology back to: " << to_string(restore_topology.initial);
      if (switch_topology(restore_topology.initial)) {
        data_modified = true;
      }
      else {
        BOOST_LOG(error) << "Failed to switch back to the initial topology!";
        result.success = false;
      }
    }
    else {
      BOOST_LOG(warning) << "Initial topology invalid (VDD may have been removed), keeping current topology";
    }

    return result;
  }

}  // namespace display_device

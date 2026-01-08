// local includes
#include "settings_topology.h"
#include "src/display_device/to_string.h"
#include "src/globals.h"
#include "src/logging.h"

namespace display_device {

  namespace {
    /**
     * @brief 基于初始拓扑，补全那些当前inactive但应该恢复的设备
     * @param base_topology 基础拓扑（通常是当前拓扑）
     * @param requested_device_id 请求的设备ID
     * @param initial_topology_devices 初始拓扑中的设备列表（只补全这些设备）
     * @return 补全后的拓扑
     */
    active_topology_t
    augment_topology_with_inactive_devices(
      const active_topology_t &base_topology,
      const std::string &requested_device_id,
      const boost::optional<std::unordered_set<std::string>> &initial_topology_devices = boost::none) {
      
      // 先拷贝一份作为候选结果
      active_topology_t augmented_topology { base_topology };

      // 收集当前拓扑中的设备 id，避免重复添加
      const auto existing_ids { get_device_ids_from_topology(augmented_topology) };

      const auto available_devices { enum_available_devices() };
      if (available_devices.empty()) {
        return base_topology;
      }

      // 如果提供了初始拓扑设备列表，只补全这些设备
      // 否则补全所有inactive设备（旧行为）
      if (initial_topology_devices && !initial_topology_devices->empty()) {
        BOOST_LOG(debug) << "Augmenting topology based on initial topology devices (respecting user's original configuration)";
        
        for (const auto &device_id : *initial_topology_devices) {
          // 已经在拓扑中的设备不需要再处理
          if (existing_ids.count(device_id) > 0) {
            continue;
          }

          // 检查设备是否可用且是inactive状态
          auto device_it = available_devices.find(device_id);
          if (device_it == available_devices.end()) {
            BOOST_LOG(debug) << "Device from initial topology not available: " << device_id;
            continue;
          }

          if (device_it->second.device_state != device_state_e::inactive) {
            // 设备已经是active或其他状态，不需要补全
            continue;
          }

          BOOST_LOG(debug) << "Augmenting topology with device from initial topology: " << device_id;
          augmented_topology.push_back({ device_id });
        }
      }
      else {
        // 没有初始拓扑约束的场景（非VDD）
        // augment_topology的真正作用应该是：把在final_topology中但因为某些原因变成inactive的设备重新激活
        // 但determine_final_topology已经决定了要激活哪些设备，我们不应该再自动添加新设备
        // 所以这里直接返回base_topology，不做任何补全
        BOOST_LOG(debug) << "No initial topology constraint, relying on determine_final_topology result without augmentation";
        return base_topology;
      }

      // 如果补全后的拓扑不合法，则保守地退回原始拓扑，避免把系统弄到奇怪状态
      if (!augmented_topology.empty() && !is_topology_valid(augmented_topology)) {
        BOOST_LOG(warning) << "Augmented display topology is invalid, falling back to original topology.";
        return base_topology;
      }

      return augmented_topology;
    }

    /**
     * @brief Get all device ids that belong in the same group as provided ids (duplicated displays).
     * @param device_id Device id to search for in the topology.
     * @param topology Topology to search.
     * @return A list of device ids, with the provided device id always at the front.
     *
     * EXAMPLES:
     * ```cpp
     * const auto duplicated_devices = get_duplicate_devices("MY_DEVICE_ID", get_current_topology());
     * ```
     */
    std::vector<std::string>
    get_duplicate_devices(const std::string &device_id, const active_topology_t &topology) {
      std::vector<std::string> duplicated_devices;

      duplicated_devices.clear();
      duplicated_devices.push_back(device_id);

      for (const auto &group : topology) {
        for (const auto &group_device_id : group) {
          if (device_id == group_device_id) {
            std::copy_if(std::begin(group), std::end(group), std::back_inserter(duplicated_devices), [&](const auto &id) {
              return id != device_id;
            });
            break;
          }
        }
      }

      return duplicated_devices;
    }

    /**
     * @brief Check if device id is found in the active topology.
     * @param device_id Device id to search for in the topology.
     * @param topology Topology to search.
     * @return True if device id is in the topology, false otherwise.
     *
     * EXAMPLES:
     * ```cpp
     * const bool is_in_topology = is_device_found_in_active_topology("MY_DEVICE_ID", get_current_topology());
     * ```
     */
    bool
    is_device_found_in_active_topology(const std::string &device_id, const active_topology_t &topology) {
      for (const auto &group : topology) {
        for (const auto &group_device_id : group) {
          if (device_id == group_device_id) {
            return true;
          }
        }
      }

      return false;
    }

    /**
     * @brief Compute the final topology based on the information we have.
     * @param device_prep The device preparation setting from user configuration.
     * @param primary_device_requested  Indicates that the user did NOT specify device id to be used.
     * @param duplicated_devices Devices that we need to handle.
     * @param topology The current topology that we are evaluating.
     * @return Topology that matches requirements and should be set.
     */
    active_topology_t
    determine_final_topology(parsed_config_t::device_prep_e device_prep, const bool primary_device_requested, const std::vector<std::string> &duplicated_devices, const active_topology_t &topology) {
      boost::optional<active_topology_t> final_topology;

      const bool topology_change_requested { device_prep != parsed_config_t::device_prep_e::no_operation };
      if (topology_change_requested) {
        if (device_prep == parsed_config_t::device_prep_e::ensure_only_display) {
          // Device needs to be the only one that's active or if it's a PRIMARY device,
          // only the whole PRIMARY group needs to be active (in case they are duplicated)

          if (primary_device_requested) {
            if (topology.size() > 1) {
              // There are other topology groups other than the primary devices,
              // so we need to change that
              final_topology = active_topology_t { { duplicated_devices } };
            }
            else {
              // Primary device group is the only one active, nothing to do
            }
          }
          else {
            // Since primary_device_requested == false, it means a device was specified via config by the user
            // and is the only device that needs to be enabled

            if (is_device_found_in_active_topology(duplicated_devices.front(), topology)) {
              // Device is currently active in the active topology group

              if (duplicated_devices.size() > 1 || topology.size() > 1) {
                // We have more than 1 device in the group, or we have more than 1 topology groups.
                // We need to disable all other devices
                final_topology = active_topology_t { { duplicated_devices.front() } };
              }
              else {
                // Our device is the only one that's active, nothing to do
              }
            }
            else {
              // Our device is not active, we need to activate it and ONLY it
              final_topology = active_topology_t { { duplicated_devices.front() } };
            }
          }
        }
        // device_prep_e::ensure_active || device_prep_e::ensure_primary
        else {
          //  The device needs to be active at least.

          if (primary_device_requested || is_device_found_in_active_topology(duplicated_devices.front(), topology)) {
            // Device is already active, nothing to do here
          }
          else {
            // Create the extended topology as it's probably what makes sense the most...
            final_topology = topology;
            final_topology->push_back({ duplicated_devices.front() });
          }
        }
      }

      return final_topology ? *final_topology : topology;
    }

  }  // namespace

  std::unordered_set<std::string>
  remove_vdd_from_topology(active_topology_t &topology) {
    std::unordered_set<std::string> removed_device_ids;
    
    // Get list of available devices (includes both active and inactive devices)
    // This ensures we don't remove inactive devices that can be re-enabled
    const auto available_devices = enum_available_devices();
    std::unordered_set<std::string> available_device_ids;
    for (const auto &[device_id, info] : available_devices) {
      // Include all devices (active, inactive, primary) - they all can potentially be used
      available_device_ids.insert(device_id);
    }

    for (auto &group : topology) {
      auto new_end = std::remove_if(group.begin(), group.end(),
        [&removed_device_ids, &available_device_ids](const std::string &device_id) {
          // First check if device exists in available devices
          // Note: available_devices includes inactive devices, so inactive devices will pass this check
          const bool device_exists = available_device_ids.count(device_id) > 0;
          
          if (!device_exists) {
            // Device doesn't exist in available devices at all - remove it
            // This means the device was truly destroyed (e.g., VDD uninstalled, physical display disconnected)
            // It's safe to remove as it cannot be re-enabled
            BOOST_LOG(debug) << "Removing non-existent device from topology: " << device_id;
            removed_device_ids.insert(device_id);  // Track removed ID
            return true;
          }
          
          // Device exists (could be active or inactive), check if it's VDD by friendly name
          // Only remove if it's VDD - inactive physical displays will be preserved
          const auto friendly_name = get_display_friendly_name(device_id);
          if (friendly_name == ZAKO_NAME) {
            BOOST_LOG(debug) << "Removing VDD device from topology: " << device_id;
            removed_device_ids.insert(device_id);  // Track removed ID
            return true;
          }
          
          // Device exists and is not VDD - preserve it (even if inactive, it can be re-enabled)
          return false;
        });
      group.erase(new_end, group.end());
    }

    // Remove empty groups
    topology.erase(
      std::remove_if(topology.begin(), topology.end(),
        [](const auto &group) { return group.empty(); }),
      topology.end());

    return removed_device_ids;
  }

  /**
   * @brief Enumerate and get one of the devices matching the id or
   *        any of the primary devices if id is unspecified.
   * @param device_id Id to find in enumerated devices.
   * @return Device id, or empty string if an error has occurred.
   *
   * EXAMPLES:
   * ```cpp
   * const std::string primary_device = find_one_of_the_available_devices("");
   * const std::string id_that_matches_provided_id = find_one_of_the_available_devices(primary_device);
   * ```
   */
  std::string
  find_one_of_the_available_devices(const std::string &device_id) {
    const auto devices { enum_available_devices() };
    if (devices.empty()) {
      BOOST_LOG(error) << "Display device list is empty!";
      return {};
    }
    BOOST_LOG(info) << "Available display devices: " << to_string(devices);

    const auto device_it { std::find_if(std::begin(devices), std::end(devices), [&device_id](const auto &entry) {
      return device_id.empty() ? entry.second.device_state == device_state_e::primary : entry.first == device_id;
    }) };
    if (device_it == std::end(devices)) {
      BOOST_LOG(error) << "Device " << (device_id.empty() ? "PRIMARY" : device_id) << " not found in the list of available devices!";
      return {};
    }

    return device_it->first;
  }

  std::unordered_set<std::string>
  get_device_ids_from_topology(const active_topology_t &topology) {
    std::unordered_set<std::string> device_ids;
    for (const auto &group : topology) {
      for (const auto &device_id : group) {
        device_ids.insert(device_id);
      }
    }

    return device_ids;
  }

  std::unordered_set<std::string>
  get_newly_enabled_devices_from_topology(const active_topology_t &previous_topology, const active_topology_t &new_topology) {
    const auto prev_ids { get_device_ids_from_topology(previous_topology) };
    auto new_ids { get_device_ids_from_topology(new_topology) };

    for (auto &id : prev_ids) {
      new_ids.erase(id);
    }

    return new_ids;
  }

  boost::optional<handled_topology_result_t>
  handle_device_topology_configuration(
    const parsed_config_t &config,
    const boost::optional<topology_pair_t> &previously_configured_topology,
    const std::function<bool()> &revert_settings,
    const boost::optional<active_topology_t> &pre_saved_initial_topology) {
    const bool primary_device_requested { config.device_id.empty() };
    const std::string requested_device_id { find_one_of_the_available_devices(config.device_id) };
    if (requested_device_id.empty()) {
      // Error already logged
      return boost::none;
    }

    // If we still have a previously configured topology, we could potentially skip making any changes to the topology.
    // However, it could also mean that we need to revert any previous changes in case the final topology has changed somehow.
    if (previously_configured_topology) {
      // Here we are pretending to be in an initial topology and want to perform reevaluation in case the
      // user has changed the settings while the stream was paused. For the proper "evaluation" order,
      // see logic outside this conditional.
      const auto prev_duplicated_devices { get_duplicate_devices(requested_device_id, previously_configured_topology->initial) };
      auto prev_final_topology { determine_final_topology(config.device_prep, primary_device_requested, prev_duplicated_devices, previously_configured_topology->initial) };

      // 与当前实现保持一致：在非「仅启用」模式下，也对“历史期望拓扑”做一次补全，
      // 这样在比较是否需要回滚时，不会因为我们额外补上的 inactive 设备导致无意义的回滚与再次切换。
      if (config.device_prep != parsed_config_t::device_prep_e::ensure_only_display) {
        prev_final_topology = augment_topology_with_inactive_devices(prev_final_topology, requested_device_id);
      }

      // There is also an edge case where we can have a different number of primary duplicated devices, which wasn't the case
      // during the initial topology configuration. If the user requested to use the primary device,
      // the prev_final_topology would not reflect that change in primary duplicated devices. Therefore, we also need
      // to evaluate current topology (which would have the new state of primary devices) and arrive at the
      // same final topology as the prev_final_topology.
      const auto current_topology { get_current_topology() };
      const auto duplicated_devices { get_duplicate_devices(requested_device_id, current_topology) };
      auto final_topology { determine_final_topology(config.device_prep, primary_device_requested, duplicated_devices, current_topology) };

      if (config.device_prep != parsed_config_t::device_prep_e::ensure_only_display) {
        final_topology = augment_topology_with_inactive_devices(final_topology, requested_device_id);
      }

      // If the topology we are switching to is the same as the final topology we had before, that means
      // user did not change anything, and we don't need to revert changes.
      if (!is_topology_the_same(previously_configured_topology->modified, prev_final_topology) ||
          !is_topology_the_same(previously_configured_topology->modified, final_topology)) {
        BOOST_LOG(warning) << "Previous topology does not match the new one. Reverting previous changes!";
        if (!revert_settings()) {
          return boost::none;
        }
      }
    }

    // Regardless of whether the user has made any changes to the user configuration or not, we always
    // need to evaluate the current topology and perform the switch if needed as the user might
    // have been playing around with active displays while the stream was paused.

    const auto current_topology { get_current_topology() };
    if (!is_topology_valid(current_topology)) {
      BOOST_LOG(error) << "Display topology is invalid!";
      return boost::none;
    }

    // When dealing with the "requested device" here and in other functions we need to keep
    // in mind that it could belong to a duplicated display and thus all of them
    // need to be taken into account, which complicates everything...
    
    // 在VDD场景下，使用真实初始拓扑来计算duplicated_devices和final_topology
    // 这样可以基于用户串流前的真实状态来构建目标拓扑
    const auto &topology_for_calculation = pre_saved_initial_topology ? *pre_saved_initial_topology : current_topology;
    
    auto duplicated_devices { get_duplicate_devices(requested_device_id, topology_for_calculation) };
    auto final_topology { determine_final_topology(config.device_prep, primary_device_requested, duplicated_devices, topology_for_calculation) };

    // 只在特定模式下才调用augment_topology
    // no_operation模式：不调整任何内容，跳过
    // ensure_only_display模式：只启用指定设备，不补全，跳过
    if (config.device_prep != parsed_config_t::device_prep_e::ensure_only_display &&
        config.device_prep != parsed_config_t::device_prep_e::no_operation) {
      // 如果有预保存的初始拓扑（VDD场景），只补全在初始拓扑中的设备
      // 这样可以尊重用户的原始配置（不会打开用户手动关闭的显示器）
      if (pre_saved_initial_topology) {
        const auto initial_devices = get_device_ids_from_topology(*pre_saved_initial_topology);
        BOOST_LOG(debug) << "Augmenting topology with constraints from initial topology (VDD scenario)";
        final_topology = augment_topology_with_inactive_devices(final_topology, requested_device_id, initial_devices);
      }
    }

    BOOST_LOG(debug) << "Current display topology: " << to_string(current_topology);
    if (!is_topology_the_same(current_topology, final_topology)) {
      BOOST_LOG(info) << "Changing display topology to: " << to_string(final_topology);
      if (!set_topology(final_topology)) {
        // Error already logged.
        return boost::none;
      }

      // It is possible that we no longer have duplicate displays, so we need to update the list
      duplicated_devices = get_duplicate_devices(requested_device_id, final_topology);
    }

    // This check is mainly to cover the case for "config.device_prep == no_operation" as we at least
    // have to validate that the device exists, but it doesn't hurt to double-check it in all cases.
    if (!is_device_found_in_active_topology(requested_device_id, final_topology)) {
      BOOST_LOG(error) << "Device " << requested_device_id << " is not active!";
      return boost::none;
    }

    // 如果有预保存的初始拓扑（在VDD创建前保存的），使用它作为真实初始拓扑
    // 否则使用当前拓扑（可能已被VDD破坏）
    const auto real_initial_topology = pre_saved_initial_topology ? *pre_saved_initial_topology : current_topology;
    
    return handled_topology_result_t {
      topology_pair_t {
        real_initial_topology,  // 使用真实的初始拓扑
        final_topology },
      topology_metadata_t {
        final_topology,
        get_newly_enabled_devices_from_topology(current_topology, final_topology),
        primary_device_requested,
        duplicated_devices }
    };
  }

}  // namespace display_device

/**
 * @file src/platform/linux/vdd_instances.cpp
 * @brief Attribution bookkeeping for the Linux virtual displays.
 */
#include "vdd_instances.h"

#include <algorithm>

#include "src/vdd_client_identity.h"

namespace platf::vdd_instances {
  std::size_t
  find(const std::vector<record_t> &records, std::string_view identifier, std::size_t selected) {
    const auto serial = vdd_identity::serial_for_identifier(identifier);
    if (serial == 0) {
      // Unidentified caller: the historical single-slot behaviour, i.e. whatever
      // is selected (a tray-created display keeps being the one it created).
      if (selected < records.size()) {
        return selected;
      }
      // Without a selection, an unidentified caller may still take a legacy
      // record that no client owns.
      const auto legacy = std::find_if(records.begin(), records.end(), [](const record_t &record) {
        return record.serial == 0;
      });
      return legacy == records.end() ? npos : static_cast<std::size_t>(legacy - records.begin());
    }

    const auto owned = std::find_if(records.begin(), records.end(), [serial](const record_t &record) {
      return record.serial == serial;
    });
    return owned == records.end() ? npos : static_cast<std::size_t>(owned - records.begin());
  }

  std::size_t
  live_count(const std::vector<record_t> &records) {
    return static_cast<std::size_t>(std::count_if(records.begin(), records.end(), [](const record_t &record) {
      return record.live;
    }));
  }

  std::vector<std::string>
  used_connectors(const std::vector<record_t> &records) {
    std::vector<std::string> connectors;
    connectors.reserve(records.size());
    for (const auto &record : records) {
      if (!record.connector.empty()) {
        connectors.push_back(record.connector);
      }
    }
    return connectors;
  }

  std::string
  pick_connector(const std::vector<std::string> &candidates, const std::vector<std::string> &unavailable) {
    for (const auto &candidate : candidates) {
      if (candidate.empty()) {
        continue;
      }
      if (std::find(unavailable.begin(), unavailable.end(), candidate) == unavailable.end()) {
        return candidate;
      }
    }
    return {};
  }
}  // namespace platf::vdd_instances

/**
 * @file src/platform/linux/vdd_instances.h
 * @brief Attribution bookkeeping for the Linux virtual displays.
 *
 * The Windows virtual display is a driver object created per client GUID, so
 * two clients naturally get two monitors. Linux has no such driver: a virtual
 * display is a physical connector hijacked with an EDID override, and the only
 * per-client slot the EDID can carry is the serial number. This module holds
 * the pure part of the bookkeeping — which record a request belongs to, and
 * which connector a new record may take — so it can be tested without DRM.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace platf::vdd_instances {
  /// One virtual display: a connector carrying our EDID override.
  struct record_t {
    std::string client_identifier;  ///< Client it was created for ("" when unidentified).
    std::uint32_t serial { 0 };     ///< EDID serial number: the client's identity on the wire.
    std::string connector;          ///< e.g. "DP-1".
    bool live { false };            ///< Whether the connector currently reports "connected".
  };

  constexpr std::size_t npos = static_cast<std::size_t>(-1);

  /**
   * @brief Find the record a request belongs to.
   * @details Two rules, in order:
   *          1. When the requester has an identity, the record whose EDID serial
   *             matches it — including a display adopted from a previous run,
   *             whose original client name is no longer known.
   *          2. An unidentified request (empty identifier: tray or headless
   *             creation) keeps the historical single-slot behaviour and
   *             belongs to the selected record, if any.
   * @param records Known records.
   * @param identifier Requesting client, empty when unidentified.
   * @param selected Index of the record the shared session fields describe.
   * @return Index of the matching record, or npos.
   */
  std::size_t find(const std::vector<record_t> &records, std::string_view identifier,
                   std::size_t selected = npos);

  /// Records still reporting a live connector.
  std::size_t live_count(const std::vector<record_t> &records);

  /// Connectors of the records, in table order.
  std::vector<std::string> used_connectors(const std::vector<record_t> &records);

  /**
   * @brief Choose a connector for a new record.
   * @param candidates Connector names in preference order.
   * @param unavailable Connectors that must not be taken (records in use,
   *        physical outputs an exclusive session switched off).
   * @return The first free candidate, or an empty string.
   */
  std::string pick_connector(const std::vector<std::string> &candidates,
                             const std::vector<std::string> &unavailable);
}  // namespace platf::vdd_instances

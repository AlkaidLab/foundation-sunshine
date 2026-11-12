/**
 * @file src/vdd_client_identity.cpp
 * @brief Canonical per-client identity for the virtual display.
 */

#include "vdd_client_identity.h"

#include <boost/uuid/name_generator_sha1.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace vdd_identity {
  namespace {
    /**
     * @brief The name-based UUID namespace used for every client identifier.
     * @details The nil UUID. Frozen: changing it would rename every existing
     *          client's virtual display.
     */
    constexpr boost::uuids::uuid kClientNamespace {};

    boost::uuids::uuid
    name_uuid(std::string_view identifier) {
      return boost::uuids::name_generator_sha1 { kClientNamespace }(
        reinterpret_cast<const unsigned char *>(identifier.data()), identifier.size());
    }
  }  // namespace

  std::string
  guid_for_identifier(std::string_view identifier) {
    if (identifier.empty()) {
      return {};
    }

    return "{" + boost::uuids::to_string(name_uuid(identifier)) + "}";
  }

  std::uint32_t
  serial_for_identifier(std::string_view identifier) {
    if (identifier.empty()) {
      return 0;
    }

    const auto uuid = name_uuid(identifier);
    const std::uint32_t serial = (static_cast<std::uint32_t>(uuid.data[0]) << 24) |
                                 (static_cast<std::uint32_t>(uuid.data[1]) << 16) |
                                 (static_cast<std::uint32_t>(uuid.data[2]) << 8) |
                                 static_cast<std::uint32_t>(uuid.data[3]);

    // Serial zero is the "no serial" encoding in several consumer tools; the
    // derivation must never produce it for a real client.
    return serial == 0 ? 1u : serial;
  }

}  // namespace vdd_identity

/**
 * @file src/vdd_client_identity.h
 * @brief Canonical per-client identity for the virtual display.
 * @details Windows hands the per-client GUID to the ZakoVDD driver
 *          (`CREATEMONITOR {GUID}:...`); Linux has no driver to hand it to and
 *          hijacks a physical connector with an EDID override instead. Both
 *          platforms must still derive *the same* identity for the same client
 *          identifier, because that identity is what tells two clients' virtual
 *          displays apart. Keeping the derivation here — one implementation,
 *          shared by the Windows and Linux halves — is what stops the two
 *          platforms from silently drifting into different GUIDs for one
 *          client, which is exactly the bug that made Linux share a single
 *          virtual display between all clients.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vdd_identity {

  /**
   * @brief The client's canonical GUID, or an empty string without an identifier.
   * @details RFC 4122 version-5 (SHA-1, name-based) UUID over the UTF-8
   *          identifier with the nil UUID as namespace — the same derivation
   *          `boost::uuids::name_generator_sha1` performs, which is what the
   *          Windows side has always sent to the driver. Formatted the way the
   *          Windows payload spells it: braces and lowercase hex.
   */
  [[nodiscard]] std::string
  guid_for_identifier(std::string_view identifier);

  /**
   * @brief A stable 32-bit serial for the identifier, never zero.
   * @details The EDID serial-number field is the only per-client slot an EDID
   *          override can carry that the compositor treats as display identity
   *          (the product name has to stay `ZAKO_NAME`, and every descriptor
   *          slot in the base block is already taken). Taken from the leading
   *          bits of the same name-based derivation as the GUID, so a connector
   *          can be attributed to its client without storing a guessable
   *          mapping, and two clients never collide in practice.
   */
  [[nodiscard]] std::uint32_t
  serial_for_identifier(std::string_view identifier);

}  // namespace vdd_identity

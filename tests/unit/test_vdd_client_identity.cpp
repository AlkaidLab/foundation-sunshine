/**
 * @file tests/unit/test_vdd_client_identity.cpp
 * @brief Test the canonical per-client virtual display identity.
 *
 * The expected GUIDs are the RFC 4122 version-5 (SHA-1, nil namespace) values
 * for the same identifiers — the derivation Windows has always sent to
 * ZakoVDD, independently reproduced with `uuid.uuid5(uuid.UUID(int=0), name)`.
 * Pinning them here is what keeps the Windows and Linux halves from drifting
 * apart: if this test changes, every existing client's virtual display has been
 * renamed.
 */
#include "src/vdd_client_identity.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>

namespace {
  constexpr const char *kLivingRoomTv = "living-room-tv";
  constexpr const char *kBedroomDeck = "Bedroom Deck";
}  // namespace

TEST(VddClientIdentity, GuidMatchesTheRfc4122DerivationForTheSameIdentifier) {
  EXPECT_EQ(vdd_identity::guid_for_identifier(kLivingRoomTv), "{fed83b8c-4d8b-5380-93f9-2613daf4e420}");
  EXPECT_EQ(vdd_identity::guid_for_identifier(kBedroomDeck), "{30aef30b-b34f-58e1-b0bd-4ec855bc6e04}");
}

TEST(VddClientIdentity, GuidIsEmptyWithoutAnIdentifier) {
  // No identifier means no client attribution: the Windows payload omits the
  // GUID and Linux keeps the legacy shared-serial EDID.
  EXPECT_TRUE(vdd_identity::guid_for_identifier("").empty());
  EXPECT_EQ(vdd_identity::serial_for_identifier(""), 0u);
}

TEST(VddClientIdentity, GuidIsStableAndDistinct) {
  EXPECT_EQ(vdd_identity::guid_for_identifier(kLivingRoomTv), vdd_identity::guid_for_identifier(kLivingRoomTv));

  std::unordered_set<std::string> guids;
  for (int i = 0; i < 64; ++i) {
    guids.insert(vdd_identity::guid_for_identifier("client-" + std::to_string(i)));
  }
  EXPECT_EQ(guids.size(), 64u);
  EXPECT_EQ(guids.count(vdd_identity::guid_for_identifier(kLivingRoomTv)), 0u);
}

TEST(VddClientIdentity, SerialIsTheLeadingGuidBitsAndNeverZero) {
  // "{fed83b8c-..." -> 0xFED83B8C: the serial and the GUID must agree, because
  // the EDID serial is the only half the compositor and adoption can read back.
  EXPECT_EQ(vdd_identity::serial_for_identifier(kLivingRoomTv), 0xFED83B8Cu);
  EXPECT_EQ(vdd_identity::serial_for_identifier(kBedroomDeck), 0x30AEF30Bu);

  for (int i = 0; i < 256; ++i) {
    EXPECT_NE(vdd_identity::serial_for_identifier("client-" + std::to_string(i)), 0u);
  }
}

TEST(VddClientIdentity, SerialIsStableAcrossCalls) {
  EXPECT_EQ(vdd_identity::serial_for_identifier(kLivingRoomTv), vdd_identity::serial_for_identifier(kLivingRoomTv));
  EXPECT_NE(vdd_identity::serial_for_identifier(kLivingRoomTv), vdd_identity::serial_for_identifier(kBedroomDeck));
}

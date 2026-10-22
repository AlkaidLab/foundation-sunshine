/**
 * @file tests/unit/test_vdd_instances.cpp
 * @brief Tests for the pure attribution rules behind per-client virtual displays.
 */
#include <gtest/gtest.h>

#include "src/platform/linux/vdd_instances.h"
#include "src/vdd_client_identity.h"

namespace {
  using platf::vdd_instances::find;
  using platf::vdd_instances::live_count;
  using platf::vdd_instances::npos;
  using platf::vdd_instances::pick_connector;
  using platf::vdd_instances::record_t;
  using platf::vdd_instances::used_connectors;

  /// A record whose serial is the one the identity rules derive for the name,
  /// exactly as the backend stores it.
  record_t
  record(std::string identifier, std::string connector, bool live = true) {
    return record_t {
      .client_identifier = identifier,
      .serial = vdd_identity::serial_for_identifier(identifier),
      .connector = std::move(connector),
      .live = live,
    };
  }
}  // namespace

TEST(VddInstances, AnIdentifiedClientOwnsTheRecordWithItsSerial) {
  const std::vector<record_t> records {
    record("alpha", "DP-1"),
    record("bravo", "DP-2"),
  };

  EXPECT_EQ(find(records, "alpha", 0), 0u);
  EXPECT_EQ(find(records, "bravo", 0), 1u);
}

TEST(VddInstances, AnAdoptedDisplayMatchesBySerialEvenWithoutItsName) {
  // A display adopted after a restart keeps its EDID, so the serial survives
  // even though the client name that produced it does not.
  record_t adopted = record("bravo", "DP-2");
  adopted.client_identifier.clear();
  const std::vector<record_t> records { adopted };

  EXPECT_EQ(find(records, "bravo", npos), 0u);
  EXPECT_EQ(find(records, "alpha", npos), npos);
}

TEST(VddInstances, AnUnknownClientDoesNotInheritAnotherClientsDisplay) {
  // The regression this module exists for: a second client used to be handed
  // the record of the first one, which tore the first client's display down.
  const std::vector<record_t> records { record("alpha", "DP-1") };

  EXPECT_EQ(find(records, "bravo", 0), npos);
  EXPECT_EQ(find(records, "charlie", 0), npos);
}

TEST(VddInstances, AnUnidentifiedRequestKeepsTheSingleSlotBehaviour) {
  const std::vector<record_t> records { record("alpha", "DP-1") };

  EXPECT_NE(vdd_identity::serial_for_identifier("alpha"), 0u);

  EXPECT_EQ(find(records, "", 0), 0u);
  EXPECT_EQ(find(records, "", npos), npos);
  EXPECT_EQ(find(records, "", 7), npos) << "a stale selection must not resolve";
}

TEST(VddInstances, LiveCountCountsOnlyConnectedRecords) {
  const std::vector<record_t> records {
    record("alpha", "DP-1"),
    record("bravo", "DP-2", false),
    record("charlie", "HDMI-A-1"),
  };

  EXPECT_EQ(live_count(records), 2u);
  EXPECT_EQ(live_count({}), 0u);
}

TEST(VddInstances, UsedConnectorsAreReportedInTableOrder) {
  const std::vector<record_t> records {
    record("alpha", "DP-1"),
    record("bravo", "HDMI-A-1"),
  };

  EXPECT_EQ(used_connectors(records), (std::vector<std::string> { "DP-1", "HDMI-A-1" }));
}

TEST(VddInstances, ANewDisplayTakesTheFirstFreeConnector) {
  const std::vector<std::string> candidates { "DP-1", "DP-2", "HDMI-A-1" };

  EXPECT_EQ(pick_connector(candidates, {}), "DP-1");
  EXPECT_EQ(pick_connector(candidates, { "DP-1" }), "DP-2");
  EXPECT_EQ(pick_connector(candidates, { "DP-1", "DP-2", "HDMI-A-1" }), "");
}

TEST(VddInstances, OfflinedPhysicalOutputsAreSkippedLikeUsedOnes) {
  // A physical screen an exclusive session switched off reads as plain
  // "disconnected" in sysfs, so it has to be named explicitly.
  const std::vector<std::string> candidates { "DP-1", "DP-2" };

  EXPECT_EQ(pick_connector(candidates, { "DP-1" }), "DP-2");
}

TEST(VddInstances, TwoClientsEndUpOnDifferentConnectors) {
  std::vector<record_t> records;

  for (const std::string identifier : { "alpha", "bravo" }) {
    const std::vector<std::string> candidates { "DP-1", "DP-2", "DP-3" };
    const auto connector = pick_connector(candidates, used_connectors(records));
    ASSERT_FALSE(connector.empty()) << identifier;
    records.push_back(record(identifier, connector));
  }

  EXPECT_EQ(find(records, "alpha", npos), 0u);
  EXPECT_EQ(find(records, "bravo", npos), 1u);
  EXPECT_EQ(live_count(records), 2u);
}

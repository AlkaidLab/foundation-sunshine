#include "src/transport_policy_json.h"

#include <gtest/gtest.h>
#include <limits>

namespace {
  transport::network_statistics_t
  network_statistics() {
    transport::network_statistics_t statistics;
    statistics.window = {std::numeric_limits<uint64_t>::max(),1,1300000,0,1100000,4,1,0,true,false};
    statistics.cumulative.negotiated = true;
    statistics.cumulative.last_new_feedback_us = 1200000;
    statistics.cumulative.latest_covered_send_us = 1050000;
    return statistics;
  }

  TEST(NetworkStatisticsJson, MatureCompleteSuccessfulSetHasExplicitRatesAndExactCounters) {
    auto statistics = network_statistics();
    statistics.cumulative.ledger.committed_packets = std::numeric_limits<uint64_t>::max();
    statistics.cumulative.ledger.committed_ip_bytes = 9007199254740993ull;
    const auto json = transport::network_statistics_json(statistics);
    EXPECT_EQ(json["connectionEpoch"],"18446744073709551615");
    EXPECT_EQ(json["committedPackets"],"18446744073709551615");
    EXPECT_EQ(json["committedIpBytes"],"9007199254740993");
    EXPECT_EQ(json["sampledPackets"],"5");
    EXPECT_EQ(json["reason"],"valid");
    EXPECT_EQ(json["rawLossPercent"],20.0);
    EXPECT_EQ(json["coveragePercent"],100.0);
  }

  TEST(NetworkStatisticsJson, EveryUnavailableStateKeepsLossNullInsteadOfFalseZero) {
    for (int condition=0; condition<7; ++condition) {
      auto statistics = network_statistics();
      statistics.window.missing = 0;
      if (condition == 0) statistics.cumulative.negotiated = false;
      if (condition == 1) statistics.window.valid = false;
      if (condition == 2) statistics.window.received = 0;
      if (condition == 3) statistics.cumulative.last_new_feedback_us = 1;
      if (condition == 4) statistics.cumulative.latest_covered_send_us = 1;
      if (condition == 5) statistics.window.unknown = 1;
      if (condition == 6) statistics.window.history_truncated = true;
      const auto json = transport::network_statistics_json(statistics);
      EXPECT_TRUE(json["rawLossPercent"].is_null()) << condition;
      EXPECT_NE(json["reason"],"valid") << condition;
      if (condition == 2 || condition == 6) {
        EXPECT_TRUE(json["coveragePercent"].is_null());
      }
    }
  }

  TEST(NetworkStatisticsJson, PartialCoverageAndClockResetStayDistinctFromValidZero) {
    auto statistics = network_statistics();
    statistics.window.received = 1;
    statistics.window.missing = 0;
    statistics.window.unknown = 1;
    auto json = transport::network_statistics_json(statistics);
    EXPECT_TRUE(json["rawLossPercent"].is_null());
    EXPECT_EQ(json["coveragePercent"],50.0);
    EXPECT_EQ(json["reason"],"coverage_incomplete");
    statistics.window.unknown = 0;
    json = transport::network_statistics_json(statistics);
    EXPECT_EQ(json["rawLossPercent"],0.0);
    statistics.window.receiver_clock_epoch = 0;
    EXPECT_TRUE(transport::network_statistics_json(statistics)["rawLossPercent"].is_null());
  }

  TEST(NetworkStatisticsJson, LifetimeUsesTheOlderFreshnessWatermarkAndNeverRenewsAtTheReader) {
    auto statistics = network_statistics();
    EXPECT_EQ(transport::network_statistics_json(statistics)["freshnessRemainingUs"], "750000");
    statistics.cumulative.last_new_feedback_us = 400000;
    EXPECT_EQ(transport::network_statistics_json(statistics)["freshnessRemainingUs"], "100000");
    statistics.window.sampled_at_us = 1400000;
    EXPECT_EQ(transport::network_statistics_json(statistics)["freshnessRemainingUs"], "0");
    statistics.window.sampled_at_us = 1400001;
    const auto expired = transport::network_statistics_json(statistics);
    EXPECT_EQ(expired["freshnessRemainingUs"], "0");
    EXPECT_FALSE(expired["fresh"]);
    EXPECT_TRUE(expired["rawLossPercent"].is_null());
    statistics.window.sampled_at_us = std::numeric_limits<int64_t>::max();
    EXPECT_EQ(transport::network_statistics_json(statistics)["freshnessRemainingUs"], "0");
  }

  nlohmann::json
  request() {
    return { { "version", 2 }, { "sessionId", "100" }, { "connectionEpoch", "18446744073709551615" },
      { "controlEpoch", "1" }, { "expectedRevision", "1" }, { "requestId", "test-1" },
      { "budget", { { "totalKbps", 40000 }, { "otherTrafficKbps", 1000 }, { "repairReserveKbps", 0 },
                    { "probeReserveKbps", 0 }, { "videoOverheadKbps", 1000 } } },
      { "fec", { { "base", 10 }, { "key", 40 }, { "recovery", 20 } } } };
  }
}  // namespace

TEST(LegacyControlScope, OmittedPairIsTheOnlyUnscopedForm) {
  EXPECT_FALSE(transport::parse_legacy_scope(std::nullopt, std::nullopt));
  EXPECT_THROW(transport::parse_legacy_scope("1", std::nullopt), std::invalid_argument);
  EXPECT_THROW(transport::parse_legacy_scope(std::nullopt, "2"), std::invalid_argument);
  for (const auto bad : { "", "0", "01", "+1", "-1", " 1", "1 ", "1x", "18446744073709551616" }) {
    EXPECT_THROW(transport::parse_legacy_scope("1", bad), std::invalid_argument) << bad;
    EXPECT_THROW(transport::parse_legacy_scope(bad, "1"), std::invalid_argument) << bad;
  }
  EXPECT_THROW(transport::parse_legacy_scope("4294967296", "1"), std::invalid_argument);
  const auto maximum = transport::parse_legacy_scope("4294967295", "18446744073709551615");
  ASSERT_TRUE(maximum);
  EXPECT_EQ(maximum->session_id, std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(maximum->connection_epoch, std::numeric_limits<uint64_t>::max());
}

TEST(LegacyControlScope, DuplicateOrMistypedJsonDoesNotDowngrade) {
  EXPECT_FALSE(transport::parse_legacy_scope(transport::parse_legacy_control_body(R"({"enabled":false})")));
  for (const auto bad : {
    R"({"enabled":true,"sessionId":"1"})",
    R"({"sessionId":1,"connectionEpoch":"2"})",
    R"({"sessionId":"1","connectionEpoch":null})",
    R"({"sessionId":"1","connectionEpoch":"2","connectionEpoch":"3"})",
    R"({"sessionId":"1","connectionEpoch":"2","enabled":false,"enabled":true})", "[]" })
    EXPECT_THROW(transport::parse_legacy_control_body(bad), std::invalid_argument) << bad;
  const auto body = transport::parse_legacy_control_body(R"({"sessionId":"1","connectionEpoch":"2","enabled":true})");
  ASSERT_TRUE(transport::parse_legacy_scope(body));
  EXPECT_EQ(transport::parse_legacy_scope(body)->connection_epoch, 2);
}

TEST(LegacyControlScope, WorkerKeysSeparateOwnerSessionAndReconnection) {
  const auto key = transport::legacy_scope_key("alice", { 1, 2 });
  EXPECT_NE(key, transport::legacy_scope_key("bob", { 1, 2 }));
  EXPECT_NE(key, transport::legacy_scope_key("alice", { 2, 2 }));
  EXPECT_NE(key, transport::legacy_scope_key("alice", { 1, 3 }));
  EXPECT_NE(transport::legacy_scope_key("a:1", { 2, 3 }), transport::legacy_scope_key("a", { 1, 23 }));
}

TEST(TransportPolicyJson, LiveControlIsStrictAndSeparateFromManualPolicyFields) {
  auto j = request();
  j.erase("budget");
  j.erase("fec");
  j["automaticBitrate"] = true;
  j["automaticFec"] = false;
  j["maximumTotalKbps"] = 40000;
  const auto parsed = transport::parse_control_update(j.dump());
  EXPECT_EQ(parsed.connection_epoch, std::numeric_limits<uint64_t>::max());
  EXPECT_TRUE(parsed.automatic_bitrate);
  EXPECT_FALSE(parsed.automatic_fec);
  EXPECT_EQ(parsed.maximum_total_kbps, 40000);
  EXPECT_THROW(transport::parse_policy_update(j.dump()), std::invalid_argument);
  EXPECT_THROW(transport::parse_control_update(request().dump()), std::invalid_argument);
  for (const auto key : { "automaticBitrate", "automaticFec" }) {
    for (const nlohmann::json &bad : { nlohmann::json(1), nlohmann::json("true"), nlohmann::json(nullptr) }) {
      auto invalid = j;
      invalid[key] = bad;
      EXPECT_THROW(transport::parse_control_update(invalid.dump()), std::invalid_argument);
    }
  }
  for (const nlohmann::json &bad : { nlohmann::json(0), nlohmann::json(-1), nlohmann::json(800001), nlohmann::json(1.5), nlohmann::json(true) }) {
    auto invalid = j;
    invalid["maximumTotalKbps"] = bad;
    EXPECT_THROW(transport::parse_control_update(invalid.dump()), std::invalid_argument);
  }
  auto duplicate = j.dump();
  duplicate.insert(1, "\"automaticBitrate\":false,");
  EXPECT_THROW(transport::parse_control_update(duplicate), std::invalid_argument);
  j["activationEpoch"] = "2";
  EXPECT_THROW(transport::parse_control_update(j.dump()), std::invalid_argument);
}

TEST(TransportPolicyJson, AutomaticIntentDoesNotFabricateConfirmedRuntimeOrEncoderState) {
  transport::frame_policy_t initial;
  initial.connection_epoch = 42;
  initial.budget.total_kbps = 40000;
  initial.encoder_kbps = 32000;
  transport::policy_state_t state(initial, 50000, true, true);
  const auto manual = state.request_normalized(initial.budget, 20, 30, 20, 1, 1).policy;
  const auto armed = state.request_automatic_control(true, false, 50000, manual->revision, manual->control_epoch, "enable").policy;
  ASSERT_TRUE(armed);
  const auto status = transport::policy_status_json(state.snapshot(), 100);
  EXPECT_EQ(status["accepted"]["automaticControl"]["maximumTotalKbps"], 50000);
  EXPECT_EQ(status["accepted"]["automaticControl"]["activationEpoch"], std::to_string(armed->control_epoch));
  EXPECT_EQ(status["accepted"]["wireBudgetKbps"], 40000);
  EXPECT_TRUE(status["confirmed"].is_null());
  EXPECT_FALSE(status["experimentalControllerActive"].get<bool>());
  EXPECT_TRUE(status["experimentalLiveControlAvailable"].get<bool>());
  EXPECT_FALSE(status["wireBudgetEnforced"].get<bool>());
  EXPECT_FALSE(status["receipts"].back()["encoderApplied"].get<bool>());
}

TEST(TransportPolicyJson, CanonicalIdentitiesPreserveAll64Bits) {
  EXPECT_EQ(transport::parse_policy_identity("18446744073709551615"), std::numeric_limits<uint64_t>::max());
  for (const auto bad : { "", "-1", "+1", " 1", "1 ", "01", "1e2", "1.0", "18446744073709551616" }) {
    EXPECT_THROW(transport::parse_policy_identity(bad), std::invalid_argument) << bad;
  }
  const auto parsed = transport::parse_policy_update(request().dump());
  EXPECT_EQ(parsed.session_id, 100u);
  EXPECT_EQ(parsed.connection_epoch, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(parsed.fec_base, 10u);
  EXPECT_EQ(parsed.fec_key, 40u);
  EXPECT_EQ(parsed.fec_recovery, 20u);
}

TEST(TransportPolicyJson, EncoderCeilingIsReadOnlyAndDoesNotClaimApplication) {
  auto body = request();
  body["encoderCeilingKbps"] = 1500;
  EXPECT_THROW(transport::parse_policy_update(body.dump()), std::invalid_argument);
  transport::frame_policy_t initial;
  initial.connection_epoch = 42;
  initial.budget.total_kbps = 40000;
  initial.encoder_kbps = 32000;
  transport::policy_state_t state(initial, 50000);
  const auto manual = state.request_normalized(initial.budget, 20, 30, 20, 1, 1).policy;
  const auto granted = state.transfer_control({ 42, manual->control_epoch, transport::control_source_e::manual },
                              transport::control_source_e::googcc, manual->revision)
                         .policy;
  const auto reduced = state.request_controller_update({ 42, granted->control_epoch, transport::control_source_e::googcc },
    granted->budget, 20, 30, 20, granted->revision, {}, 1500);
  ASSERT_EQ(reduced.result, transport::policy_request_result_e::accepted);
  const auto status = transport::policy_status_json(state.snapshot(), 100);
  EXPECT_EQ(status["accepted"]["encoderCeilingKbps"], 1500);
  EXPECT_EQ(status["accepted"]["encoderKbps"], 1500);
  EXPECT_EQ(status["accepted"]["wireBudgetKbps"], 40000);
  EXPECT_TRUE(status["confirmed"].is_null());
  EXPECT_TRUE(status["receipts"].back()["encoderCeilingKbps"] == 1500);
  EXPECT_FALSE(status["receipts"].back()["encoderApplied"].get<bool>());
}

TEST(TransportPolicyJson, IntegerAndBudgetValidationRejectTruncationOrExhaustion) {
  for (const nlohmann::json &bad : { nlohmann::json(-1), nlohmann::json(0), nlohmann::json(1.5),
         nlohmann::json("40000"), nlohmann::json(true), nlohmann::json(800001), nlohmann::json(std::numeric_limits<uint64_t>::max()) }) {
    auto j = request();
    j["budget"]["totalKbps"] = bad;
    EXPECT_THROW(transport::parse_policy_update(j.dump()), std::invalid_argument);
  }
  auto j = request();
  j["budget"]["probeReserveKbps"] = 40000;
  EXPECT_THROW(transport::parse_policy_update(j.dump()), std::invalid_argument);
  j = request();
  j["fec"]["base"] = 101;
  EXPECT_THROW(transport::parse_policy_update(j.dump()), std::invalid_argument);
}

TEST(TransportPolicyJson, RejectsUnknownMissingDuplicateAndNestedPayloads) {
  auto j = request();
  j["autoFec"] = true;
  EXPECT_THROW(transport::parse_policy_update(j.dump()), std::invalid_argument);
  j = request();
  j.erase("controlEpoch");
  EXPECT_THROW(transport::parse_policy_update(j.dump()), std::invalid_argument);
  auto duplicate = request().dump();
  duplicate.insert(1, "\"version\":2,");
  EXPECT_THROW(transport::parse_policy_update(duplicate), std::invalid_argument);
  std::string nested(40, '[');
  nested += "0";
  nested += std::string(40, ']');
  EXPECT_THROW(transport::parse_policy_update(nested), std::invalid_argument);
  EXPECT_THROW(transport::parse_policy_update(std::string(8193, ' ')), std::invalid_argument);
  EXPECT_THROW(transport::parse_policy_update("{"), nlohmann::json::exception);
}

TEST(TransportPolicyJson, ValidatesRequestIdentityAndSessionWidth) {
  auto j = request();
  j["sessionId"] = "4294967296";
  EXPECT_THROW(transport::parse_policy_update(j.dump()), std::invalid_argument);
  j = request();
  j["connectionEpoch"] = 1;
  EXPECT_THROW(transport::parse_policy_update(j.dump()), std::invalid_argument);
  for (const auto key : { "connectionEpoch", "controlEpoch", "expectedRevision" }) {
    j = request();
    j[key] = "0";
    EXPECT_THROW(transport::parse_policy_update(j.dump()), std::invalid_argument);
  }
  for (const auto id : { "", "../x", "a b", "test\n1" }) {
    j = request();
    j["requestId"] = id;
    EXPECT_THROW(transport::parse_policy_update(j.dump()), std::invalid_argument);
  }
}

TEST(TransportPolicyJson, StatusSeparatesAcceptedAppliedAndSuccessfulSend) {
  const auto update = transport::parse_policy_update(request().dump());
  transport::frame_policy_t initial;
  initial.connection_epoch = update.connection_epoch;
  initial.budget.total_kbps = 40000;
  initial.encoder_kbps = 32000;
  initial.fec_base = initial.fec_key = initial.fec_recovery = 20;
  transport::policy_state_t state(initial, 50000);
  auto status = transport::policy_status_json(state.snapshot(), 100);
  EXPECT_EQ(status["controlOwner"], "server");
  EXPECT_EQ(status["controlSource"], "legacy");
  EXPECT_TRUE(status["encoderAppliedRevision"].is_null());
  EXPECT_TRUE(status["confirmed"].is_null());
  EXPECT_TRUE(status["receipts"][0]["firstSentFrame"].is_null());
  auto p = state.begin_encoder_initialization();
  ASSERT_TRUE(state.acknowledge_encoder(p, transport::policy_failure_e::none));
  auto r = state.request_normalized(update.budget, update.fec_base, update.fec_key, update.fec_recovery, 1, 1, update.request_id);
  status = transport::policy_status_json(state.snapshot(), 100);
  EXPECT_EQ(status["acceptedRevision"], "2");
  EXPECT_EQ(status["controlEpoch"], "2");
  EXPECT_EQ(status["controlSource"], "manual");
  EXPECT_EQ(status["accepted"]["controlEpoch"], "2");
  EXPECT_EQ(status["confirmed"]["controlEpoch"], "1");
  EXPECT_EQ(status["confirmed"]["controlSource"], "legacy");
  EXPECT_EQ(status["encoderAppliedRevision"], "1");
  EXPECT_EQ(status["confirmed"]["encoderKbps"], 32000);
  EXPECT_TRUE(status["pending"].get<bool>());
  ASSERT_EQ(state.acquire_pending(), r.policy);
  ASSERT_TRUE(state.acknowledge_encoder(r.policy, transport::policy_failure_e::none));
  ASSERT_TRUE(state.acknowledge_first_sent(r.policy, std::numeric_limits<uint64_t>::max()));
  status = transport::policy_status_json(state.snapshot(), 100);
  EXPECT_EQ(status["connectionEpoch"], "18446744073709551615");
  EXPECT_EQ(status["encoderAppliedRevision"], "2");
  EXPECT_EQ(status["receipts"][1]["firstSentFrame"], "18446744073709551615");
  EXPECT_FALSE(status["wireBudgetEnforced"].get<bool>());
  EXPECT_FALSE(status["packetControlAvailable"].get<bool>());
}

TEST(TransportPolicyJson, BackendFailureAndStopDoNotAdvertiseReadiness) {
  transport::frame_policy_t initial;
  initial.connection_epoch = 42;
  initial.budget.total_kbps = initial.encoder_kbps = 10000;
  transport::policy_state_t state(initial, 50000);
  ASSERT_TRUE(state.acknowledge_encoder(state.begin_encoder_initialization(), transport::policy_failure_e::none));
  state.request_legacy_change(20000, 20);
  ASSERT_TRUE(state.acknowledge_encoder(state.acquire_pending(), transport::policy_failure_e::backend_failure));
  auto status = transport::policy_status_json(state.snapshot(), 100);
  EXPECT_FALSE(status["encoderReady"].get<bool>());
  EXPECT_FALSE(status["pending"].get<bool>());
  EXPECT_EQ(status["encoderAppliedRevision"], "1");
  EXPECT_EQ(status["receipts"][1]["failure"], "backend_failure");
  state.stop();
  EXPECT_TRUE(transport::policy_status_json(state.snapshot(), 100)["stopped"].get<bool>());
}

TEST(TransportPolicyJson, ControlOwnerSourceAndGenerationDoNotInventRuntimeCapability) {
  transport::frame_policy_t initial;
  initial.connection_epoch = 42;
  initial.budget.total_kbps = initial.encoder_kbps = 10000;
  transport::policy_state_t state(initial, 50000);
  const auto update = transport::parse_policy_update(request().dump());
  const auto manual = state.request_normalized(update.budget, 10, 40, 20, 1, 1).policy;
  ASSERT_TRUE(manual);
  const auto grant = state.transfer_control({ 42, manual->control_epoch, transport::control_source_e::manual },
    transport::control_source_e::local, manual->revision);
  ASSERT_EQ(grant.result, transport::policy_request_result_e::accepted);
  const auto status = transport::policy_status_json(state.snapshot(), 100);
  EXPECT_EQ(status["controlOwner"], "local");
  EXPECT_EQ(status["controlSource"], "local");
  EXPECT_EQ(status["controlEpoch"], "3");
  EXPECT_EQ(status["accepted"]["controlSource"], "local");
  EXPECT_EQ(status["receipts"][1]["controlSource"], "manual");
  EXPECT_FALSE(status["packetControlAvailable"].get<bool>());
  EXPECT_FALSE(status["pacerAvailable"].get<bool>());
  EXPECT_FALSE(status["wireBudgetEnforced"].get<bool>());
}

TEST(TransportPolicyJson, ExperimentalNegotiationAndActivationDoNotClaimAggregateReadiness) {
  transport::frame_policy_t initial;
  initial.connection_epoch = 42;
  initial.budget.total_kbps = initial.encoder_kbps = 10000;
  EXPECT_THROW(transport::policy_state_t(initial, 50000, true, false), std::invalid_argument);
  transport::policy_state_t state(initial, 50000, true, true);
  auto status = transport::policy_status_json(state.snapshot(), 100);
  EXPECT_TRUE(status["experimentalPacketControlNegotiated"].get<bool>());
  EXPECT_TRUE(status["experimentalVideoPacerEnabled"].get<bool>());
  EXPECT_FALSE(status["experimentalControllerActive"].get<bool>());
  const auto prepared = state.prepare_normalized_controller({ 42, 1, transport::control_source_e::legacy }, initial.budget, 10, 20, 20, 1);
  ASSERT_EQ(prepared.result, transport::policy_request_result_e::accepted);
  ASSERT_EQ(state.transfer_control({ 42, 1, transport::control_source_e::legacy }, transport::control_source_e::googcc,
                   prepared.policy->revision)
              .result,
    transport::policy_request_result_e::accepted);
  status = transport::policy_status_json(state.snapshot(), 100);
  EXPECT_TRUE(status["experimentalControllerActive"].get<bool>());
  EXPECT_FALSE(status["packetControlAvailable"].get<bool>());
  EXPECT_FALSE(status["pacerAvailable"].get<bool>());
  EXPECT_FALSE(status["wireBudgetEnforced"].get<bool>());
  state.stop();
  status = transport::policy_status_json(state.snapshot(), 100);
  EXPECT_FALSE(status["experimentalPacketControlNegotiated"].get<bool>());
  EXPECT_FALSE(status["experimentalControllerActive"].get<bool>());
}

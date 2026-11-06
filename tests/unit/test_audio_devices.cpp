/**
 * @file tests/unit/test_audio_devices.cpp
 * @brief The shaping of GET /api/audio/devices: which endpoints are known virtual devices, what VB-CABLE's state is,
 *        and how the lists are ordered. No audio API is touched.
 */
#include <gtest/gtest.h>

#include "src/audio_devices.h"

namespace {
  using namespace audio_devices;

  endpoint_t
  make(flow_e flow, std::string id, std::string name, std::string adapter, std::string state = "active", bool is_default = false) {
    endpoint_t endpoint;
    endpoint.flow = flow;
    endpoint.id = std::move(id);
    endpoint.name = std::move(name);
    endpoint.adapter = std::move(adapter);
    endpoint.state = std::move(state);
    endpoint.is_default = is_default;
    return endpoint;
  }

  endpoint_t
  cable_input(std::string state = "active") {
    return make(flow_e::render, "{0.0.0.00000000}.{cable-in}", "CABLE Input (VB-Audio Virtual Cable)", "VB-Audio Virtual Cable", std::move(state));
  }

  endpoint_t
  cable_output(std::string state = "active") {
    return make(flow_e::capture, "{0.0.1.00000000}.{cable-out}", "CABLE Output (VB-Audio Virtual Cable)", "VB-Audio Virtual Cable", std::move(state));
  }

  endpoint_t
  speakers(bool is_default = true) {
    return make(flow_e::render, "{0.0.0.00000000}.{realtek}", "Speakers (Realtek(R) Audio)", "Realtek(R) Audio", "active", is_default);
  }
}  // namespace

TEST(AudioDevices, FormFactorWords) {
  EXPECT_EQ(form_factor_name(0), "remote_network");
  EXPECT_EQ(form_factor_name(1), "speakers");
  EXPECT_EQ(form_factor_name(4), "microphone");
  EXPECT_EQ(form_factor_name(9), "digital_display");
  EXPECT_EQ(form_factor_name(10), "unknown");
  EXPECT_EQ(form_factor_name(-1), "unknown");
}

TEST(AudioDevices, StateWords) {
  EXPECT_EQ(state_name(0x1), "active");
  EXPECT_EQ(state_name(0x2), "disabled");
  EXPECT_EQ(state_name(0x8), "unplugged");
  EXPECT_EQ(state_name(0x4), "not_present");
  EXPECT_EQ(state_name(0), "");
  EXPECT_EQ(state_name(0x3), "");
}

TEST(AudioDevices, VbCableIsRecognisedByAdapterName) {
  EXPECT_TRUE(is_vb_cable(cable_input()));
  EXPECT_TRUE(is_vb_cable(cable_output()));
  EXPECT_EQ(classify_virtual(cable_input()), virtual_kind::vb_cable);
  auto lower = cable_input();
  lower.adapter = "vb-audio virtual cable";
  EXPECT_TRUE(is_vb_cable(lower));
}

TEST(AudioDevices, VbCableIsRecognisedByFriendlyNameWhenTheAdapterIsUnknown) {
  auto endpoint = cable_input();
  endpoint.adapter.clear();
  EXPECT_TRUE(is_vb_cable(endpoint));
}

TEST(AudioDevices, OtherVbAudioProductsAreNotTheMicrophoneCable) {
  const auto cable_a = make(flow_e::render, "a", "CABLE-A Input (VB-Audio Cable A)", "VB-Audio Cable A");
  EXPECT_FALSE(is_vb_cable(cable_a));
  EXPECT_EQ(classify_virtual(cable_a), virtual_kind::vb_audio);
  const auto voicemeeter = make(flow_e::render, "v", "VoiceMeeter Input (VB-Audio VoiceMeeter VAIO)", "VB-Audio VoiceMeeter VAIO");
  EXPECT_EQ(classify_virtual(voicemeeter), virtual_kind::vb_audio);
  EXPECT_FALSE(summarize_vb_cable({ cable_a, voicemeeter }).installed);
}

TEST(AudioDevices, SteamAndOwnMicrophoneAreFlagged) {
  EXPECT_EQ(classify_virtual(make(flow_e::render, "s", "Speakers (Steam Streaming Speakers)", "Steam Streaming Speakers")), virtual_kind::steam_streaming);
  EXPECT_EQ(classify_virtual(make(flow_e::capture, "m", "Microphone (Steam Streaming Microphone)", "Steam Streaming Microphone")), virtual_kind::steam_streaming);
  EXPECT_EQ(classify_virtual(make(flow_e::capture, "u", "Microphone (Sunshine Virtual Microphone)", "USB Audio Device")), virtual_kind::virtual_microphone);
  EXPECT_EQ(classify_virtual(speakers()), "");
}

TEST(AudioDevices, VbCableStateMissingWhenNoEndpoint) {
  const auto status = summarize_vb_cable({ speakers() });
  EXPECT_FALSE(status.installed);
  EXPECT_EQ(status.state, "missing");
  EXPECT_TRUE(status.render_id.empty());
  EXPECT_TRUE(status.capture_id.empty());
}

TEST(AudioDevices, VbCableStateReadyWhenBothSidesAreActive) {
  const auto status = summarize_vb_cable({ speakers(), cable_input(), cable_output() });
  EXPECT_TRUE(status.installed);
  EXPECT_EQ(status.state, "ready");
  EXPECT_EQ(status.render_id, "{0.0.0.00000000}.{cable-in}");
  EXPECT_EQ(status.capture_id, "{0.0.1.00000000}.{cable-out}");
  EXPECT_EQ(status.render_name, "CABLE Input (VB-Audio Virtual Cable)");
}

TEST(AudioDevices, VbCableStatePartialWhenOneSideIsUsable) {
  EXPECT_EQ(summarize_vb_cable({ cable_input() }).state, "partial");
  EXPECT_EQ(summarize_vb_cable({ cable_input(), cable_output("disabled") }).state, "partial");
  EXPECT_TRUE(summarize_vb_cable({ cable_input(), cable_output("disabled") }).installed);
}

TEST(AudioDevices, VbCableStateUnavailableWhenNothingIsUsable) {
  const auto status = summarize_vb_cable({ cable_input("disabled"), cable_output("unplugged") });
  EXPECT_TRUE(status.installed);
  EXPECT_EQ(status.state, "unavailable");
}

TEST(AudioDevices, RememberedVbCableEndpointsAreUnavailableNotInstalled) {
  // The Windows test host case: the driver's device is disabled (Code 22), Windows lists the endpoints as not present.
  const auto status = summarize_vb_cable({ speakers(), cable_input("not_present"), cable_output("not_present") });
  EXPECT_FALSE(status.installed);
  EXPECT_EQ(status.state, "unavailable");
  const auto json = to_json({ speakers(), cable_input("not_present"), cable_output("not_present") });
  EXPECT_EQ(json["render"].size(), 1u);  // remembered endpoints are not offered in a picker
  EXPECT_TRUE(json["capture"].empty());
  EXPECT_EQ(json["vb_cable"]["state"], "unavailable");
}

namespace {
  driver_t
  cable_driver(bool disabled, long problem = -2) {
    driver_t driver;
    driver.name = "VB-Audio Virtual Cable";
    driver.disabled = disabled;
    driver.problem = problem == -2 ? (disabled ? 22 : 0) : problem;
    return driver;
  }
}  // namespace

TEST(AudioDevices, DriverNodeIsRecognisedByItsName) {
  EXPECT_TRUE(is_vb_cable_driver(cable_driver(false)));
  driver_t other;
  other.name = "VB-Audio Cable A";
  EXPECT_FALSE(is_vb_cable_driver(other));
  other.name = "Steam Streaming Speakers";
  EXPECT_FALSE(is_vb_cable_driver(other));
}

TEST(AudioDevices, InstalledButDisabledDriverIsDisabledNotMissing) {
  // The Windows test host case: driver 1.0.3.5 in the device tree, CM_PROB_DISABLED, the endpoints are only remembered.
  const std::vector<driver_t> drivers { cable_driver(true) };
  const auto status = summarize_vb_cable({ speakers(), cable_input("not_present"), cable_output("not_present") }, &drivers);
  EXPECT_EQ(status.state, "disabled");
  EXPECT_TRUE(status.installed);
  EXPECT_TRUE(status.driver_present);
  EXPECT_TRUE(status.driver_disabled);
  const auto json = to_json({ speakers(), cable_input("not_present"), cable_output("not_present") }, &drivers);
  EXPECT_EQ(json["vb_cable"]["state"], "disabled");
  EXPECT_TRUE(json["vb_cable"]["installed"].get<bool>());
  EXPECT_TRUE(json["vb_cable"]["driver_disabled"].get<bool>());
  EXPECT_EQ(json["render"].size(), 1u);  // still no remembered endpoint in a picker
}

TEST(AudioDevices, DisabledDriverWithoutAnyEndpointIsStillDisabled) {
  const std::vector<driver_t> drivers { cable_driver(true) };
  EXPECT_EQ(summarize_vb_cable({ speakers() }, &drivers).state, "disabled");
}

TEST(AudioDevices, NoDriverInTheDeviceTreeMeansNotInstalledEvenWithRememberedEndpoints) {
  const std::vector<driver_t> none;
  const auto status = summarize_vb_cable({ speakers(), cable_input("not_present"), cable_output("not_present") }, &none);
  EXPECT_EQ(status.state, "missing");
  EXPECT_FALSE(status.installed);
  EXPECT_FALSE(status.driver_present);
  // without the device tree the endpoints decide alone, as before
  EXPECT_EQ(summarize_vb_cable({ speakers(), cable_input("not_present"), cable_output("not_present") }, nullptr).state, "unavailable");
  EXPECT_EQ(summarize_vb_cable({ speakers() }, &none).state, "missing");
}

TEST(AudioDevices, EnabledDriverWithNothingUsableIsUnavailable) {
  const std::vector<driver_t> drivers { cable_driver(false) };
  const auto status = summarize_vb_cable({ speakers(), cable_input("not_present"), cable_output("not_present") }, &drivers);
  EXPECT_EQ(status.state, "unavailable");
  EXPECT_TRUE(status.installed);
  EXPECT_FALSE(status.driver_disabled);
  const std::vector<driver_t> with_problem { cable_driver(false, 52) };
  EXPECT_EQ(summarize_vb_cable({ speakers() }, &with_problem).state, "unavailable");
}

TEST(AudioDevices, UsableEndpointsDecideEvenWhenTheDriverReadsDisabled) {
  const std::vector<driver_t> drivers { cable_driver(true) };
  EXPECT_EQ(summarize_vb_cable({ cable_input(), cable_output() }, &drivers).state, "ready");
  EXPECT_EQ(summarize_vb_cable({ cable_input() }, &drivers).state, "partial");
}

TEST(AudioDevices, ReadyDriverAndEndpointsAreReady) {
  const std::vector<driver_t> drivers { cable_driver(false) };
  const auto status = summarize_vb_cable({ cable_input(), cable_output() }, &drivers);
  EXPECT_EQ(status.state, "ready");
  EXPECT_TRUE(status.driver_present);
}

TEST(AudioDevices, VbCablePrefersTheActiveEndpointWhenASideIsListedTwice) {
  auto stale = cable_input("disabled");
  stale.id = "{0.0.0.00000000}.{stale}";
  const auto status = summarize_vb_cable({ stale, cable_input(), cable_output() });
  EXPECT_EQ(status.state, "ready");
  EXPECT_EQ(status.render_id, "{0.0.0.00000000}.{cable-in}");
}

TEST(AudioDevices, JsonSplitsDirectionsAndPutsTheDefaultFirst) {
  auto zeta = make(flow_e::render, "z", "Zeta", "Z");
  auto alpha = make(flow_e::render, "a", "Alpha", "A");
  auto disabled = make(flow_e::render, "d", "Aardvark", "D", "disabled");
  const auto json = to_json({ zeta, disabled, alpha, speakers(true), cable_output() });
  ASSERT_TRUE(json["success"].get<bool>());
  EXPECT_EQ(json["version"], JSON_VERSION);
  EXPECT_TRUE(json["supported"].get<bool>());
  const auto &render = json["render"];
  ASSERT_EQ(render.size(), 4u);
  EXPECT_EQ(render[0]["id"], "{0.0.0.00000000}.{realtek}");  // the default
  EXPECT_TRUE(render[0]["default"].get<bool>());
  EXPECT_EQ(render[1]["id"], "a");  // then the usable ones by name
  EXPECT_EQ(render[2]["id"], "z");
  EXPECT_EQ(render[3]["id"], "d");  // the disabled one last
  ASSERT_EQ(json["capture"].size(), 1u);
  EXPECT_EQ(json["capture"][0]["virtual"], "vb_cable");
  EXPECT_TRUE(render[0]["virtual"].is_null());
}

TEST(AudioDevices, JsonCarriesTheVbCableSummary) {
  const auto json = to_json({ speakers(), cable_input(), cable_output() });
  EXPECT_TRUE(json["vb_cable"]["installed"].get<bool>());
  EXPECT_EQ(json["vb_cable"]["state"], "ready");
  EXPECT_EQ(json["vb_cable"]["capture_name"], "CABLE Output (VB-Audio Virtual Cable)");
  const auto missing = to_json({ speakers() });
  EXPECT_FALSE(missing["vb_cable"]["installed"].get<bool>());
  EXPECT_EQ(missing["vb_cable"]["state"], "missing");
}

TEST(AudioDevices, JsonKeepsNamesAsGiven) {
  auto endpoint = make(flow_e::render, "id", "扬声器 (Realtek(R) Audio)", "Realtek(R) Audio");
  endpoint.description = "扬声器";
  const auto json = to_json({ endpoint });
  EXPECT_EQ(json["render"][0]["name"], "扬声器 (Realtek(R) Audio)");
  EXPECT_EQ(json["render"][0]["description"], "扬声器");
}

TEST(AudioDevices, UnsupportedPlatformIsAnAnswerNotAnError) {
  const auto json = unsupported_json();
  EXPECT_TRUE(json["success"].get<bool>());
  EXPECT_FALSE(json["supported"].get<bool>());
  EXPECT_TRUE(json["render"].empty());
  EXPECT_EQ(json["vb_cable"]["state"], "unsupported");
}

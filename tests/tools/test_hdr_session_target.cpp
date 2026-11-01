/** Exercise the real session resolver without a live display or user-profile writes. */
#include "src/config.h"
#include "src/hdr/session_target.h"
#include "src/logging_severity.h"
#include "src/platform/windows/display_device/color_profile.h"
#include "src/rtsp.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <map>
#include <tuple>
#include <vector>

boost::log::sources::severity_logger<int> warning;
boost::log::sources::severity_logger<int> info;

namespace {
  std::string clients;
  std::string current_user;
  std::map<std::string, hdr::calibration_profile_t> installed;
  std::vector<std::string> reads;
  std::vector<std::pair<std::string, std::string>> applied;
}  // namespace

namespace config {
  video_t video {};
  std::string
  get_clients_config() {
    return clients;
  }
}  // namespace config

namespace display_device::vdd_utils {
  std::string
  generate_client_guid(const std::string &uuid) {
    return "vdd-" + uuid;
  }
}  // namespace display_device::vdd_utils

namespace display_device::win_color_profile {
  bool
  apply_hdr_calibration(const std::string &device_id, const hdr::calibration_binding_t &binding) {
    applied.emplace_back(device_id, binding.profile.profile_name);
    return true;
  }
  calibration_result_t
  read_hdr_calibration(const std::string &name) {
    reads.push_back(name);
    calibration_result_t result;
    result.user_sid = current_user;
    if (const auto entry = installed.find(name); entry != installed.end()) {
      result.profile = entry->second;
      result.state = calibration_result_t::state_e::detected;
    }
    return result;
  }
}  // namespace display_device::win_color_profile

namespace rtsp_stream {
  void
  launch_session_t::set_hdr_target(const hdr::client_display_capabilities_t &value, hdr::target_source_e source) {
    hdr_capabilities = value;
    hdr_target_source = source;
  }
}  // namespace rtsp_stream

namespace {
  class HdrSessionTarget: public testing::Test {
  protected:
    void
    SetUp() override {
      current_user = "user";
      config::video.vdd_reuse = false;
      installed = {
        { "A.icc", { "A.icc", std::string(64, 'a'), { true, 800, 0.001f, 700 } } },
        { "B.icc", { "B.icc", std::string(64, 'b'), { true, 1600, 0.001f, 1200 } } }
      };
      clients = nlohmann::json::array({ { { "uuid", "A" }, { "hdrCalibration", hdr::calibration_binding_t { "user", "vdd-A", installed.at("A.icc") } } },
                                        { { "uuid", "B" }, { "hdrCalibration", hdr::calibration_binding_t { "user", "vdd-B", installed.at("B.icc") } } } })
                  .dump();
      reads.clear();
      applied.clear();
    }

    rtsp_stream::launch_session_t
    session() {
      rtsp_stream::launch_session_t value {};
      value.client_cert_uuid = "B";
      value.enable_hdr = true;
      value.use_vdd = true;
      return value;
    }
  };
}  // namespace

TEST_F(HdrSessionTarget, ResolvesEachClientBeforeDisplayCreationAcrossABAReconnects) {
  for (const auto uuid : { "A", "B", "A" }) {
    auto value = session();
    value.client_cert_uuid = uuid;
    hdr::resolve_session_target(value);
    EXPECT_EQ(value.hdr_target_source, hdr::target_source_e::windows_hdr_calibration);
    EXPECT_FLOAT_EQ(value.hdr_capabilities.max_nits, std::string(uuid) == "A" ? 800 : 1600);
    EXPECT_FLOAT_EQ(value.hdr_capabilities.max_full_frame_nits, std::string(uuid) == "A" ? 700 : 1200);
    EXPECT_EQ(reads.back(), std::string(uuid) + ".icc");
  }
}

TEST_F(HdrSessionTarget, RejectsAnotherUserOrChangedProfile) {
  current_user = "another-user";
  auto value = session();
  hdr::resolve_session_target(value);
  EXPECT_EQ(value.hdr_target_source, hdr::target_source_e::safe_defaults);
  current_user = "user";
  installed.at("B.icc").fingerprint = std::string(64, 'c');
  value = session();
  hdr::resolve_session_target(value);
  EXPECT_EQ(value.hdr_target_source, hdr::target_source_e::safe_defaults);
}

TEST_F(HdrSessionTarget, PreservesClientAndManualBrightnessPriority) {
  auto value = session();
  value.reported_hdr_capabilities = { true, 1200, 0.005f, 600 };
  hdr::resolve_session_target(value);
  EXPECT_EQ(value.hdr_target_source, hdr::target_source_e::client_report);
  EXPECT_FLOAT_EQ(value.hdr_capabilities.max_nits, 1200);
  auto settings = nlohmann::json::parse(clients);
  settings[1].update({ { "hdrBrightnessMode", "manual" }, { "hdrBrightnessMaxNits", 500 },
    { "hdrBrightnessMinNits", 0.01 }, { "hdrBrightnessMaxFullFrameNits", 400 } });
  clients = settings.dump();
  hdr::resolve_session_target(value);
  EXPECT_EQ(value.hdr_target_source, hdr::target_source_e::manual_override);
  EXPECT_FLOAT_EQ(value.hdr_capabilities.max_nits, 500);
  EXPECT_TRUE(reads.empty());
}

TEST_F(HdrSessionTarget, DoesNotAdoptCalibrationForSharedPhysicalOrSdrSessions) {
  for (const auto [shared, vdd, enable_hdr] : { std::tuple { true, true, true }, { false, false, true }, { false, true, false } }) {
    config::video.vdd_reuse = shared;
    auto value = session();
    value.use_vdd = vdd;
    value.enable_hdr = enable_hdr;
    hdr::resolve_session_target(value);
    EXPECT_EQ(value.hdr_target_source, hdr::target_source_e::safe_defaults);
    EXPECT_TRUE(hdr::apply_vdd_calibration(value, "vdd"));
  }
  EXPECT_TRUE(reads.empty());
  EXPECT_TRUE(applied.empty());
}

TEST_F(HdrSessionTarget, RestoresOnlyTheClientsVerifiedProfileAndLeavesUnknownDefaultsAlone) {
  auto value = session();
  hdr::resolve_session_target(value);
  ASSERT_TRUE(hdr::apply_vdd_calibration(value, "prepared-B"));
  ASSERT_EQ(applied.size(), 1);
  EXPECT_EQ(applied.front(), (std::pair<std::string, std::string> { "prepared-B", "B.icc" }));
  clients = "[]";
  value = session();
  hdr::resolve_session_target(value);
  EXPECT_TRUE(hdr::apply_vdd_calibration(value, "prepared-B"));
  EXPECT_EQ(applied.size(), 1);
}

TEST_F(HdrSessionTarget, UnlockCannotApplyLuminanceDifferentFromThePreparedVdd) {
  auto value = session();
  hdr::resolve_session_target(value);
  installed.at("B.icc").luminance.max_nits = 1700;
  EXPECT_FALSE(hdr::apply_vdd_calibration(value, "prepared-B"));
  value.hdr_target_source = hdr::target_source_e::safe_defaults;
  EXPECT_FALSE(hdr::apply_vdd_calibration(value, "prepared-B"));
  EXPECT_TRUE(applied.empty());
  // Manual/client-report metadata takes priority independently of ICC selection.
  value.hdr_target_source = hdr::target_source_e::client_report;
  EXPECT_TRUE(hdr::apply_vdd_calibration(value, "prepared-B"));
  installed.clear();
  value.hdr_target_source = hdr::target_source_e::windows_hdr_calibration;
  EXPECT_FALSE(hdr::apply_vdd_calibration(value, "prepared-B"));
}

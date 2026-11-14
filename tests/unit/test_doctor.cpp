/**
 * @file tests/unit/test_doctor.cpp
 * @brief The health check: evaluators from made-up facts, the JSON contract, redaction, the runner's timeout and ?only=.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/doctor/doctor.h"
#include "src/doctor/doctor_os.h"

namespace {
  using namespace doctor;
  using namespace std::literals;

  const item_t *
  find(const std::vector<item_t> &items, std::string_view id) {
    const auto it = std::ranges::find_if(items, [&](const auto &item) { return item.id == id; });
    return it == items.end() ? nullptr : &*it;
  }

  /** Every non-pass item carries a fix text; a skip says why it was not checked. */
  void
  expect_contract(const std::vector<item_t> &items, lang_e lang = lang_e::zh) {
    std::set<std::string> ids;
    for (const auto &item : items) {
      EXPECT_TRUE(ids.insert(item.id).second) << "duplicate id " << item.id;
      EXPECT_FALSE(item.title.empty()) << item.id;
      EXPECT_TRUE(item.id.starts_with(item.group + ".")) << item.id;
      if (item.status == status_e::warn || item.status == status_e::fail) {
        EXPECT_FALSE(item.fix.text.empty()) << item.id << " has no fix";
      }
      if (item.status == status_e::skip) {
        EXPECT_TRUE(item.detail.starts_with(lang == lang_e::zh ? "未检查" : "Not checked")) << item.id << ": " << item.detail;
      }
    }
  }
}  // namespace

TEST(DoctorVersions, NvidiaWindowsDriverStringReadsAsPublicVersion) {
  EXPECT_EQ(nvidia_driver_version("32.0.15.6094", true), "560.94");
  EXPECT_EQ(nvidia_driver_version("31.0.15.2225", true), "522.25");
  EXPECT_EQ(nvidia_driver_version("27.21.14.5671", true), "456.71");
  EXPECT_EQ(nvidia_driver_version("570.86.16", false), "570.86.16");
  EXPECT_FALSE(nvidia_driver_version("", true));
  EXPECT_FALSE(nvidia_driver_version("unknown", true));
}

TEST(DoctorVersions, CompareIsNumericPerField) {
  EXPECT_EQ(compare_versions("522.25", "456.71"), 1);
  EXPECT_EQ(compare_versions("520.56.06", "520.56.6"), 0);
  EXPECT_EQ(compare_versions("570.0", "570"), 0);
  EXPECT_EQ(compare_versions("99.9", "100.0"), -1);
}

TEST(DoctorGpu, OldNvidiaDriverFailsWithAnUpdateLink) {
  gpu_facts_t facts;
  facts.windows = true;
  facts.gpus.push_back({ "NVIDIA GeForce RTX 3080", "nvidia", "27.21.14.4573", true, false });
  const auto items = evaluate_gpu(facts, lang_e::zh);
  const auto *driver = find(items, "gpu.driver.0");
  ASSERT_TRUE(driver);
  EXPECT_EQ(driver->status, status_e::fail);
  EXPECT_NE(driver->detail.find("445.73"), std::string::npos);
  EXPECT_NE(driver->detail.find("456.71"), std::string::npos);
  ASSERT_TRUE(driver->fix.action);
  EXPECT_EQ(driver->fix.action->kind, "link");
  expect_contract(items);
}

TEST(DoctorGpu, DriverWithoutAv1TierWarnsAndNamesTheMinimum) {
  gpu_facts_t facts;
  facts.windows = false;
  facts.gpus.push_back({ "NVIDIA GeForce RTX 4070", "nvidia", "515.65.01", true, false });
  const auto items = evaluate_gpu(facts, lang_e::en);
  const auto *driver = find(items, "gpu.driver.0");
  ASSERT_TRUE(driver);
  EXPECT_EQ(driver->status, status_e::warn);
  EXPECT_NE(driver->detail.find("520.56.06"), std::string::npos);
  expect_contract(items, lang_e::en);
}

TEST(DoctorGpu, CurrentDriverPassesWithItsSdkTier) {
  gpu_facts_t facts;
  facts.windows = true;
  facts.gpus.push_back({ "NVIDIA GeForce RTX 4090", "nvidia", "32.0.15.7283", true, false });
  const auto items = evaluate_gpu(facts, lang_e::zh);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].status, status_e::pass);
  EXPECT_NE(items[0].detail.find("572.83"), std::string::npos);
  EXPECT_NE(items[0].detail.find("13.0"), std::string::npos);
}

TEST(DoctorGpu, HybridWithoutAdapterChoiceWarnsAndOpensTheSetting) {
  gpu_facts_t facts;
  facts.windows = true;
  facts.gpus.push_back({ "Intel(R) UHD Graphics 770", "intel", "31.0.101.4502", true, true });
  facts.gpus.push_back({ "NVIDIA GeForce RTX 4060 Laptop GPU", "nvidia", "32.0.15.6094", false, false });
  const auto items = evaluate_gpu(facts, lang_e::zh);
  const auto *hybrid = find(items, "gpu.hybrid");
  ASSERT_TRUE(hybrid);
  EXPECT_EQ(hybrid->status, status_e::warn);
  ASSERT_TRUE(hybrid->fix.action);
  EXPECT_EQ(hybrid->fix.action->target, "setting:adapter_name");
  expect_contract(items);
}

TEST(DoctorGpu, ConfiguredAdapterThatMatchesNothingFails) {
  gpu_facts_t facts;
  facts.configured_adapter = "Radeon RX 9999";
  facts.gpus.push_back({ "NVIDIA GeForce RTX 4090", "nvidia", "570.86.16", false, false });
  const auto items = evaluate_gpu(facts, lang_e::en);
  const auto *adapter = find(items, "gpu.adapter");
  ASSERT_TRUE(adapter);
  EXPECT_EQ(adapter->status, status_e::fail);
}

TEST(DoctorGpu, NoAdapterFails) {
  const auto items = evaluate_gpu({}, lang_e::zh);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].status, status_e::fail);
  expect_contract(items);
}

TEST(DoctorEncoders, ForeignBackendFailingIsNotAProblemButTheConfiguredOneIs) {
  encoder_facts_t facts;
  facts.active = "nvenc";
  encoder_fact_t nvenc { .name = "nvenc", .ran = true, .usable = true, .active = true };
  nvenc.h264 = { true, true, false, false };
  nvenc.hevc = { true, true, true, true };
  nvenc.av1 = { true, false, false, false };
  encoder_fact_t qsv { .name = "quicksync", .ran = true, .usable = false, .failure = "unsupported_gpu" };
  facts.encoders = { nvenc, qsv };
  auto items = evaluate_encoders(facts, lang_e::zh);
  EXPECT_EQ(find(items, "encoder.nvenc")->status, status_e::pass);
  EXPECT_NE(find(items, "encoder.nvenc")->detail.find("HEVC ✓ HDR 4:4:4"), std::string::npos);
  EXPECT_NE(find(items, "encoder.nvenc")->detail.find("AV1 ✗"), std::string::npos);
  EXPECT_EQ(find(items, "encoder.quicksync")->status, status_e::pass);
  expect_contract(items);

  facts.configured = "quicksync";
  facts.encoders[1].configured = true;
  items = evaluate_encoders(facts, lang_e::zh);
  EXPECT_EQ(find(items, "encoder.quicksync")->status, status_e::fail);
  EXPECT_EQ(find(items, "encoder.quicksync")->fix.action->target, "setting:encoder");
  expect_contract(items);
}

TEST(DoctorEncoders, NothingUsableFailsAndStreamingSkips) {
  encoder_facts_t facts;
  facts.encoders = { { .name = "nvenc", .ran = true, .usable = false, .failure = "h264_session" } };
  auto items = evaluate_encoders(facts, lang_e::zh);
  ASSERT_TRUE(find(items, "encoder.none"));
  EXPECT_EQ(find(items, "encoder.none")->status, status_e::fail);

  facts.encoders = { { .name = "nvenc", .ran = false, .skipped_reason = "streaming" } };
  items = evaluate_encoders(facts, lang_e::zh);
  EXPECT_EQ(find(items, "encoder.nvenc")->status, status_e::skip);
  EXPECT_FALSE(find(items, "encoder.none"));
  expect_contract(items);
}

TEST(DoctorEncoders, ForcedHevcThatTheActiveEncoderCannotDoWarns) {
  encoder_facts_t facts;
  facts.hevc_mode = 2;
  encoder_fact_t vaapi { .name = "vaapi", .ran = true, .usable = true, .active = true };
  vaapi.h264 = { true, true };
  vaapi.hevc = { true, false };
  facts.encoders = { vaapi };
  const auto items = evaluate_encoders(facts, lang_e::en);
  EXPECT_EQ(find(items, "encoder.vaapi")->status, status_e::warn);
  EXPECT_EQ(find(items, "encoder.vaapi")->fix.action->target, "setting:hevc_mode");
}

TEST(DoctorEncoders, ABackendThatDoesNotFinishIsItsOwnWarning) {
  encoder_facts_t facts;
  facts.encoders = {
    { .name = "nvenc", .ran = false, .skipped_reason = "timeout" },
    { .name = "software", .ran = true, .usable = true },
  };
  facts.encoders[1].h264 = { true, true, false, false };
  const auto items = evaluate_encoders(facts, lang_e::zh);
  EXPECT_EQ(find(items, "encoder.nvenc")->status, status_e::warn);
  EXPECT_NE(find(items, "encoder.nvenc")->detail.find("15"), std::string::npos);
  EXPECT_EQ(find(items, "encoder.software")->status, status_e::pass);
  EXPECT_FALSE(find(items, "encoder.none"));
  expect_contract(items);
}

TEST(DoctorCapture, EveryOutcomeHasItsStatus) {
  capture_fact_t facts { .ran = true, .ok = true, .method = "ddx", .display = "\\\\.\\DISPLAY1", .width = 2560, .height = 1440, .first_frame_ms = 41 };
  EXPECT_EQ(evaluate_capture(facts, lang_e::zh)[0].status, status_e::pass);
  EXPECT_NE(evaluate_capture(facts, lang_e::zh)[0].detail.find("2560×1440"), std::string::npos);
  facts.ok = false;
  facts.failure = "no_frame";
  EXPECT_EQ(evaluate_capture(facts, lang_e::zh)[0].status, status_e::warn);
  facts.failure = "open";
  EXPECT_EQ(evaluate_capture(facts, lang_e::zh)[0].status, status_e::fail);
  EXPECT_EQ(evaluate_capture(facts, lang_e::zh)[0].fix.action->target, "setting:output_name");
  capture_fact_t skipped { .ran = false, .skipped_reason = "streaming" };
  const auto items = evaluate_capture(skipped, lang_e::zh);
  EXPECT_EQ(items[0].status, status_e::skip);
  expect_contract(items);
}

TEST(DoctorVdd, StatesMapToStatuses) {
  vdd_fact_t facts { .supported_platform = false };
  EXPECT_EQ(evaluate_vdd(facts, lang_e::zh)[0].status, status_e::skip);

  facts = { .supported_platform = true, .installed = false, .capture = "vdd" };
  EXPECT_EQ(evaluate_vdd(facts, lang_e::zh)[0].status, status_e::fail);
  facts.capture = "ddx";
  EXPECT_EQ(evaluate_vdd(facts, lang_e::zh)[0].status, status_e::warn);

  facts = { .supported_platform = true, .installed = true, .running = false, .problem_code_valid = true, .problem_code = 14, .os_build = 22631 };
  auto items = evaluate_vdd(facts, lang_e::zh);
  EXPECT_EQ(find(items, "vdd.driver")->status, status_e::warn);
  EXPECT_FALSE(find(items, "vdd.control"));
  expect_contract(items);

  facts = { .supported_platform = true, .installed = true, .running = true, .control_available = true, .problem_code_valid = true, .driver_version = "1.2.3.4", .os_build = 19045 };
  items = evaluate_vdd(facts, lang_e::zh);
  EXPECT_EQ(find(items, "vdd.driver")->status, status_e::pass);
  EXPECT_NE(find(items, "vdd.driver")->detail.find("1.2.3.4"), std::string::npos);
  EXPECT_EQ(find(items, "vdd.control")->status, status_e::pass);
  EXPECT_EQ(find(items, "vdd.hdr")->status, status_e::warn);
  EXPECT_EQ(find(items, "vdd.refresh")->status, status_e::skip);
  expect_contract(items);
}

TEST(DoctorAudio, MissingSinkFailsMissingVirtualWarnsMicOffSkips) {
  audio_fact_t facts { .checked = true, .sink = "Speakers (USB)", .sink_found = false, .virtual_checked = true, .virtual_found = false, .mic_checked = true, .mic_state = "disabled" };
  const auto items = evaluate_audio(facts, lang_e::zh);
  EXPECT_EQ(find(items, "audio.sink")->status, status_e::fail);
  EXPECT_EQ(find(items, "audio.sink")->fix.action->target, "setting:audio_sink");
  EXPECT_EQ(find(items, "audio.virtual")->status, status_e::warn);
  EXPECT_EQ(find(items, "audio.mic")->status, status_e::skip);
  expect_contract(items);
}

TEST(DoctorInput, WindowsWithoutVigemFailsAndLinuxUinputIsChecked) {
  input_fact_t windows { .windows = true, .gamepad = "auto", .vigem_checked = true, .vigem_ok = false, .vigem_error = "bus not found", .usbip_supported = true, .usbip_tool_found = true, .usbip_driver_checked = true, .usbip_driver_found = true, .usbip_driver = "usbip2_ude" };
  auto items = evaluate_input(windows, lang_e::zh);
  EXPECT_EQ(find(items, "input.vigem")->status, status_e::fail);
  EXPECT_EQ(find(items, "input.ds5")->status, status_e::pass);
  EXPECT_EQ(find(items, "input.usb")->status, status_e::pass);
  expect_contract(items);

  input_fact_t linux_facts { .gamepad = "auto", .uinput_checked = true, .uinput_ok = false, .uinput_error = "Permission denied", .uhid_checked = true, .uhid_ok = true };
  items = evaluate_input(linux_facts, lang_e::zh);
  EXPECT_EQ(find(items, "input.uinput")->status, status_e::fail);
  EXPECT_EQ(find(items, "input.ds5")->status, status_e::pass);
  EXPECT_EQ(find(items, "input.usb")->status, status_e::skip);
  expect_contract(items);
}

TEST(DoctorNetwork, ClosedPortBlockedFirewallAndVerdicts) {
  network_fact_t facts;
  facts.checked = true;
  facts.ports = {
    { "tcp", 47984, "https", true, false, "allowed" },
    { "udp", 47998, "video", false, true, "allowed" },
    { "tcp", 47990, "web", false, false, "blocked" },
  };
  facts.upnp_enabled = true;
  facts.router_protocol = "none";
  facts.public_verdict = "unreachable";
  facts.wan_ip_class = "cgnat";
  facts.mesh_verdict = "none";
  facts.lan_listening = true;
  facts.lan_address = "*";
  const auto items = evaluate_network(facts, lang_e::zh);
  EXPECT_EQ(find(items, "network.ports")->status, status_e::fail);
  EXPECT_NE(find(items, "network.ports")->detail.find("47990"), std::string::npos);
  EXPECT_EQ(find(items, "network.ports")->detail.find("47998"), std::string::npos) << "on-demand ports are not closed";
  EXPECT_EQ(find(items, "network.firewall")->status, status_e::fail);
  EXPECT_EQ(find(items, "network.upnp")->status, status_e::warn);
  EXPECT_EQ(find(items, "network.public")->status, status_e::warn);
  EXPECT_EQ(find(items, "network.mesh")->status, status_e::skip);
  EXPECT_EQ(find(items, "network.lan")->status, status_e::pass);
  EXPECT_EQ(find(items, "network.lan")->detail, "在所有地址上监听");
  expect_contract(items);
}

TEST(DoctorSystem, LowDiskPendingRebootOldWindows) {
  system_fact_t facts;
  facts.windows = true;
  facts.disks = { { "logs", "C:\\Program Files\\Sunshine\\config", true, 512ull << 20, 256ull << 30 }, { "config", "C:\\", true, 3ull << 30, 256ull << 30 } };
  facts.time_checked = true;
  facts.time_synced = false;
  facts.account_checked = true;
  facts.account = "admin";
  facts.reboot_checked = true;
  facts.reboot_reasons = { "Windows 更新" };
  facts.os_name = "Windows 10";
  facts.os_build = 19044;
  const auto items = evaluate_system(facts, lang_e::zh);
  EXPECT_EQ(find(items, "system.disk.logs")->status, status_e::fail);
  EXPECT_EQ(find(items, "system.disk.config")->status, status_e::warn);
  EXPECT_EQ(find(items, "system.time")->status, status_e::warn);
  EXPECT_EQ(find(items, "system.account")->status, status_e::warn);
  EXPECT_EQ(find(items, "system.reboot")->status, status_e::warn);
  EXPECT_EQ(find(items, "system.os")->status, status_e::fail);
  expect_contract(items);
}

TEST(DoctorHost, ChannelCrashesAndConfigProblems) {
  EXPECT_EQ(channel_of("2026.1104.121813", ""), "stable");
  EXPECT_EQ(channel_of("2026.1104.121813.96a1e2b6", ""), "dev");
  // ".杂鱼" ends every CI release: not a channel and not a dirty tree
  EXPECT_EQ(channel_of("2026.1104.121813.杂鱼", ""), "stable");
  EXPECT_EQ(channel_of("v2026.1103.21122.杂鱼", ""), "stable");
  EXPECT_EQ(channel_of("2026.1104.121813.96a1e2b6.杂鱼", ""), "stable");
  EXPECT_EQ(channel_of("2026.11.0-beta.2", ""), "beta");

  host_fact_t facts { .version = "2026.1104.121813.杂鱼", .uptime_s = 90061, .crashes_checked = true, .crash_source = "WER", .crashes = { "3 小时前" }, .config_problems = { "Unrecognized configurable option [encodr]" }, .fatal_lines = 2, .log_checked = true };
  const auto items = evaluate_host(facts, lang_e::zh);
  // no build record: the version string cannot say dirty, so ".杂鱼" alone is a pass
  EXPECT_EQ(find(items, "host.version")->status, status_e::pass);
  EXPECT_EQ(find(items, "host.version")->detail.find("未提交"), std::string::npos);
  EXPECT_EQ(find(items, "host.uptime")->detail, "1 天 1 小时 1 分钟");
  EXPECT_EQ(find(items, "host.crashes")->status, status_e::warn);
  EXPECT_EQ(find(items, "host.config")->status, status_e::warn);
  EXPECT_NE(find(items, "host.config")->detail.find("encodr"), std::string::npos);
  EXPECT_EQ(find(items, "host.log")->status, status_e::warn);
  expect_contract(items);
}

TEST(DoctorRedact, AddressesMacsAndHomeFoldersAreMasked) {
  EXPECT_EQ(redact("Listening on 192.0.2.133"), "Listening on 192.0.2.0/24");
  EXPECT_EQ(redact("peer 2001:db8:85a3:1:2:3:4:5 up"), "peer 2001:0db8:85a3::/64 up");
  EXPECT_EQ(redact("mac 3c:7c:3f:12:34:56"), "mac 3c:7c:3f:…");
  EXPECT_EQ(redact("C:\\Users\\test-user\\AppData\\x.log"), "C:\\Users\\…\\AppData\\x.log");
  EXPECT_EQ(redact("/home/test-user/.config/sunshine"), "/home/…/.config/sunshine");
  // Driver versions are not addresses.
  EXPECT_EQ(redact("Driver 32.0.15.6094"), "Driver 32.0.15.6094");
  EXPECT_EQ(redact("SDK 12.0 · 560.94"), "SDK 12.0 · 560.94");
}

TEST(DoctorText, ChinesePunctuationBesideChineseText) {
  EXPECT_EQ(zh_punctuation("未检查:正在串流,探测会打断画面"), "未检查：正在串流，探测会打断画面");
  EXPECT_EQ(zh_punctuation("已安装;需要重启 Windows 才能启用(代码 14)"), "已安装；需要重启 Windows 才能启用（代码 14）");
  EXPECT_EQ(zh_punctuation("没有监听:tcp 47990 (web)"), "没有监听：tcp 47990 (web)");
  EXPECT_EQ(zh_punctuation("Driver 560.94: NVENC SDK 12.0, AV1"), "Driver 560.94: NVENC SDK 12.0, AV1");
  EXPECT_EQ(zh_punctuation("重启 Sunshine?"), "重启 Sunshine?") << "a mark after Latin text stays";
  EXPECT_EQ(zh_punctuation("驱动 31.0.101.4502;主机没有表"), "驱动 31.0.101.4502；主机没有表");
}

TEST(DoctorFix, LevelsFromTheKindAndEffectsBeforeTheyRun) {
  item_t restart { "network.ports", "network", status_e::fail, "端口", "", { "重启", action_t { "restart", "sunshine" } } };
  complete_action(restart, lang_e::zh);
  EXPECT_EQ(restart.fix.action->level, 2);
  EXPECT_TRUE(restart.fix.action->confirm);
  EXPECT_NE(restart.fix.action->effect.find("串流会断开"), std::string::npos);
  item_t open_item { "audio.sink", "audio", status_e::fail, "x", "", { "x", action_t { "open", "setting:audio_sink" } } };
  complete_action(open_item, lang_e::zh);
  EXPECT_EQ(open_item.fix.action->level, 1);
  EXPECT_FALSE(open_item.fix.action->confirm);

  input_fact_t windows { .windows = true, .gamepad = "auto", .vigem_checked = true, .vigem_ok = false, .usbip_supported = true };
  const auto items = evaluate_input(windows, lang_e::en);
  const auto &vigem = find(items, "input.vigem")->fix.action;
  ASSERT_TRUE(vigem);
  EXPECT_EQ(vigem->kind, "repair");
  EXPECT_EQ(vigem->target, "driver:vigem");
  EXPECT_EQ(vigem->level, 3);
  EXPECT_NE(vigem->effect.find("DS4Windows"), std::string::npos);
  EXPECT_EQ(find(items, "input.usb")->fix.action->target, "driver:usbip");
  vdd_fact_t vdd { .supported_platform = true, .installed = false, .capture = "vdd" };
  EXPECT_EQ(evaluate_vdd(vdd, lang_e::en)[0].fix.action->target, "driver:vdd");
}

TEST(DoctorNetwork, NatIpv6AndTheExternalProbe) {
  network_fact_t facts;
  facts.checked = true;
  facts.ports = { { "tcp", 47984, "https", true, false, "allowed" } };
  facts.lan_listening = true;
  facts.lan_address = "*";
  facts.mesh_verdict = "none";
  facts.router_protocol = "upnp-igd";
  facts.upnp_enabled = true;
  facts.public_verdict = "unreachable";
  facts.wan_ip_class = "private";
  auto items = evaluate_network(facts, lang_e::zh);
  EXPECT_EQ(find(items, "network.nat.double")->status, status_e::warn);
  EXPECT_FALSE(find(items, "network.nat.cgnat"));
  EXPECT_EQ(find(items, "network.nat.mapping")->status, status_e::skip);
  EXPECT_EQ(find(items, "network.v6.global")->status, status_e::warn) << "no IPv6 while IPv4 is unreachable";
  EXPECT_EQ(find(items, "network.pcp")->status, status_e::skip);
  expect_contract(items);

  facts.wan_ip_class = "cgnat";
  facts.ipv6_global = { "2408:8207:1a2b::/64" };
  facts.evidence_kinds = { "external-probe-v6" };
  facts.nat_mapping = "dependent";
  items = evaluate_network(facts, lang_e::zh);
  EXPECT_EQ(find(items, "network.nat.cgnat")->status, status_e::warn);
  EXPECT_EQ(find(items, "network.public")->status, status_e::pass);
  EXPECT_NE(find(items, "network.public")->detail.find("IPv6"), std::string::npos);
  EXPECT_EQ(find(items, "network.nat.mapping")->status, status_e::warn);
  EXPECT_EQ(find(items, "network.v6.global")->status, status_e::pass);
  EXPECT_EQ(find(items, "network.v6.stable")->status, status_e::skip);
  expect_contract(items);

  facts.wan_ip_class = "public";
  facts.evidence_kinds.clear();
  facts.public_verdict = "likely";
  items = evaluate_network(facts, lang_e::en);
  EXPECT_EQ(find(items, "network.nat.cgnat")->status, status_e::pass);
  EXPECT_NE(find(items, "network.public")->detail.find("not tested from outside"), std::string::npos);
}

TEST(DoctorNetwork, MissingPublicAddressIsAHintNotAFailure) {
  network_fact_t facts;
  facts.checked = true;
  facts.ports = { { "tcp", 47989, "http", true, false, "allowed" }, { "tcp", 47990, "web", true, false, "allowed" } };
  facts.lan_listening = true;
  facts.lan_address = "*";
  facts.mesh_verdict = "none";
  facts.public_verdict = "unknown";
  facts.public_address_unset = true;
  facts.public_reasons = { "unset", "private_wan" };
  const auto items = evaluate_network(facts, lang_e::zh);
  const auto *item = find(items, "network.public");
  ASSERT_TRUE(item);
  EXPECT_EQ(item->status, status_e::skip);
  ASSERT_TRUE(item->fix.action);
  EXPECT_EQ(item->fix.action->target, "setting:external_endpoints");
  EXPECT_NE(item->detail.find("没有设置公网地址"), std::string::npos);
  expect_contract(items);
}

TEST(DoctorNetwork, AClosedPortKeepsTheRealFailureWithAnUnsetAddress) {
  network_fact_t facts;
  facts.checked = true;
  facts.ports = { { "tcp", 47989, "http", false, false, "allowed" }, { "tcp", 47990, "web", true, false, "allowed" } };
  facts.lan_listening = true;
  facts.lan_address = "*";
  facts.mesh_verdict = "none";
  facts.public_verdict = "unreachable";
  facts.public_address_unset = true;
  facts.public_reasons = { "unset", "not_listening:tcp/47989" };
  const auto items = evaluate_network(facts, lang_e::zh);
  EXPECT_EQ(find(items, "network.public")->status, status_e::warn);
  EXPECT_NE(find(items, "network.public")->detail.find("连不上"), std::string::npos);
  expect_contract(items);
}

TEST(DoctorHost, TheBuildIdentityReplacesTheGuess) {
  host_fact_t facts { .version = "2026.1104.121813", .log_checked = true };
  auto items = evaluate_host(facts, lang_e::zh);
  EXPECT_NE(find(items, "host.version")->detail.find("由版本号推断"), std::string::npos);
  facts.build = build_info_t { .version = "2026.1104.121813", .commit = "96a1e2b6c0ffee00112233445566778899aabbcc", .channel = "beta", .build_time_utc = "2026-11-04T04:18:13Z", .variant = "product-zero" };
  items = evaluate_host(facts, lang_e::zh);
  const auto &detail = find(items, "host.version")->detail;
  EXPECT_NE(detail.find("beta"), std::string::npos);
  EXPECT_NE(detail.find("96a1e2b6c0"), std::string::npos);
  EXPECT_EQ(detail.find("推断"), std::string::npos);
#if !__has_include("src/build_info.h")
  EXPECT_FALSE(current_build_info()) << "a stub until feat/build-identity is merged";
#endif
}

TEST(DoctorJson, ContractHasGroupsItemsSummaryAndActions) {
  report_t report;
  report.checked_at = "2026-11-04T00:00:00Z";
  group_result_t group { "network", "网络", status_e::fail, 12, false, {} };
  group.items.push_back({ "network.ports", "network", status_e::fail, "端口", "没有监听", { "重启", action_t { "restart", "sunshine", true } } });
  group.items.push_back({ "network.lan", "network", status_e::pass, "局域网", "", {} });
  report.groups.push_back(group);
  const auto json = to_json(report);
  EXPECT_EQ(json["version"], 1);
  EXPECT_EQ(json["summary"]["fail"], 1);
  EXPECT_EQ(json["summary"]["pass"], 1);
  EXPECT_EQ(json["groups"][0]["items"][0], "network.ports");
  EXPECT_EQ(json["items"][0]["fix"]["action"]["kind"], "restart");
  EXPECT_EQ(json["items"][0]["fix"]["action"]["confirm"], true);
  EXPECT_EQ(json["source"], "host");
  EXPECT_EQ(json["streaming"], false);
  EXPECT_TRUE(json["items"][0]["fix"]["action"].contains("level"));
  EXPECT_FALSE(json["items"][1]["fix"].contains("action"));
}

TEST(DoctorRunner, RunsInParallelTimesOutAndCatches) {
  std::atomic<int> running { 0 };
  std::atomic<int> peak { 0 };
  auto slow = [&](lang_e) {
    peak = std::max(peak.load(), ++running);
    std::this_thread::sleep_for(150ms);
    --running;
    return std::vector<item_t> { { "gpu.a", "gpu", status_e::pass, "a", "", {} } };
  };
  std::vector<check_t> checks {
    { "gpu", 2000ms, slow },
    { "audio", 2000ms, [&](lang_e lang) { return slow(lang); } },
    { "host", 50ms, [](lang_e) { std::this_thread::sleep_for(400ms); return std::vector<item_t> {}; } },
    { "system", 2000ms, [](lang_e) -> std::vector<item_t> { throw std::runtime_error("boom"); } },
  };
  const auto started = std::chrono::steady_clock::now();
  const auto report = run(checks, {}, lang_e::zh);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_LT(elapsed, 380ms) << "checks ran one after another, or the runner waited for the hung check";
  EXPECT_EQ(peak.load(), 2);
  ASSERT_EQ(report.groups.size(), 4u);
  EXPECT_EQ(report.groups[0].id, "gpu") << "groups keep display order";
  const auto host = std::ranges::find_if(report.groups, [](const auto &g) { return g.id == "host"; });
  EXPECT_TRUE(host->timed_out);
  EXPECT_EQ(host->items[0].status, status_e::warn);
  ASSERT_TRUE(host->items[0].fix.action);
  EXPECT_EQ(host->items[0].fix.action->kind, "rerun");
  const auto system = std::ranges::find_if(report.groups, [](const auto &g) { return g.id == "system"; });
  EXPECT_EQ(system->items[0].status, status_e::fail);
  EXPECT_EQ(system->items[0].detail, "boom");
  std::this_thread::sleep_for(450ms);  // let the detached check finish before the test exits
}

TEST(DoctorRunner, OnlySelectsGroups) {
  std::vector<check_t> checks {
    { "gpu", 1000ms, [](lang_e) { return std::vector<item_t> { { "gpu.a", "gpu", status_e::pass, "a", "", {} } }; } },
    { "network", 1000ms, [](lang_e) { return std::vector<item_t> { { "network.a", "network", status_e::warn, "a", "", { "x" } } }; } },
  };
  const auto report = run(checks, { "network" }, lang_e::en);
  ASSERT_EQ(report.groups.size(), 1u);
  EXPECT_EQ(report.groups[0].id, "network");
  EXPECT_EQ(report.groups[0].status, status_e::warn);
  EXPECT_EQ(report.groups[0].title, "Network");
}

TEST(DoctorRoute, ParseOnly) {
  std::vector<std::string> unknown;
  EXPECT_EQ(parse_groups("encoder, network,encoder", unknown), (std::vector<std::string> { "encoder", "network" }));
  EXPECT_TRUE(unknown.empty());
  EXPECT_TRUE(parse_groups("gpus", unknown).empty());
  EXPECT_EQ(unknown, (std::vector<std::string> { "gpus" }));
}

TEST(DoctorOs, AudioEndpointMatchesAnyFieldLikeTheHost) {
  const os::audio_endpoint_t speakers { "{0.0.0.00000000}.{abc}", "Speakers (Realtek(R) Audio)", "Speakers", "Realtek(R) Audio", true };
  EXPECT_TRUE(os::endpoint_matches(speakers, "speakers (realtek(r) audio)"));
  EXPECT_TRUE(os::endpoint_matches(speakers, "{0.0.0.00000000}.{abc}"));
  EXPECT_TRUE(os::endpoint_matches(speakers, "Realtek(R) Audio"));
  EXPECT_FALSE(os::endpoint_matches(speakers, "Speakers (USB)"));
  EXPECT_FALSE(os::endpoint_matches(speakers, ""));
}

TEST(DoctorOs, ReadOnlyProbesAnswerOnThisMachine) {
  // They read; on the build machine they only have to answer without throwing.
  const auto time = os::time_sync();
  const auto reboot = os::pending_reboot();
  const auto crashes = os::recent_crashes(7 * 24 * 3600);
  EXPECT_FALSE(os::account_kind().empty());
  EXPECT_TRUE(os::writable("/").checked);
  EXPECT_FALSE(os::writable("/nonexistent/doctor").ok);
#ifndef _WIN32
  EXPECT_FALSE(os::usbip().supported);
  EXPECT_FALSE(os::windows_build());
#endif
  (void) time;
  (void) reboot;
  (void) crashes;
}

TEST(DoctorGroups, TitlesInBothLanguages) {
  for (const auto id : group_ids()) {
    EXPECT_NE(group_title(id, lang_e::zh), std::string { id });
    EXPECT_NE(group_title(id, lang_e::en), std::string { id });
  }
  EXPECT_EQ(group_title("vdd", lang_e::zh), "虚拟显示器");
}

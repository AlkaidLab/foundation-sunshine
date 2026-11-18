/**
 * @file tests/unit/test_system_info.cpp
 * @brief Fixture tests for the inventory parsers and the JSON contract.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/system_info.h"

namespace {
  using namespace system_info;

  const std::string LINUX_CPUINFO =
    "processor\t: 0\n"
    "vendor_id\t: GenuineIntel\n"
    "model name\t: Example CPU @ 3.40GHz\n"
    "cpu cores\t: 2\n"
    "siblings\t: 4\n"
    "cpu MHz\t\t: 1200.000\n"
    "\n"
    "processor\t: 1\n"
    "vendor_id\t: GenuineIntel\n"
    "model name\t: Example CPU @ 3.40GHz\n"
    "cpu cores\t: 2\n"
    "siblings\t: 4\n";
}  // namespace

TEST(SystemInfoParsing, LinuxCpuinfoIdentityAndTopology) {
  const auto cpu = parse_linux_cpuinfo(LINUX_CPUINFO);
  EXPECT_EQ(cpu.model, "Example CPU @ 3.40GHz");
  EXPECT_EQ(cpu.vendor, "GenuineIntel");
  EXPECT_EQ(cpu.cores, 2);
  EXPECT_EQ(cpu.threads, 4);
  EXPECT_FALSE(cpu.base_mhz.has_value());
}

TEST(SystemInfoParsing, LinuxCpuinfoFallsBackToArchitectureFields) {
  const auto cpu = parse_linux_cpuinfo(
    "processor\t: 0\n"
    "Processor\t: ARMv8 Processor rev 1 (v8l)\n"
    "Hardware\t: Example Board\n"
    "CPU implementer\t: 0x41\n");
  EXPECT_EQ(cpu.model, "ARMv8 Processor rev 1 (v8l)");
  EXPECT_EQ(cpu.vendor, "0x41");
  EXPECT_EQ(cpu.cores, 1);
  EXPECT_EQ(cpu.threads, 1);
}

TEST(SystemInfoParsing, OsReleaseUsesNameAndVersionId) {
  const auto os = parse_os_release(
    "NAME=\"Example Linux\"\n"
    "VERSION_ID=\"24.04\"\n"
    "PRETTY_NAME=\"Example Linux 24.04 LTS\"\n",
    "6.8.0-45-generic");
  EXPECT_EQ(os.name, "Example Linux");
  EXPECT_EQ(os.version, "24.04");
  EXPECT_EQ(os.build, "6.8.0-45-generic");
}

TEST(SystemInfoParsing, OsReleaseFallsBackToPrettyName) {
  const auto os = parse_os_release("PRETTY_NAME=Minimal Linux\n", "6.1.0");
  EXPECT_EQ(os.name, "Minimal Linux");
  EXPECT_TRUE(os.version.empty());
  EXPECT_EQ(os.build, "6.1.0");
}

TEST(SystemInfoParsing, ParseMhzAcceptsDecimalsAndSuffixes) {
  EXPECT_EQ(parse_mhz("3400.000"), 3400);
  EXPECT_EQ(parse_mhz(" 3400 MHz"), 3400);
  EXPECT_FALSE(parse_mhz("").has_value());
  EXPECT_FALSE(parse_mhz("unknown").has_value());
}

TEST(SystemInfoPrivacy, MacIsMaskedToTheOui) {
  EXPECT_EQ(mask_mac("aa:bb:cc:dd:ee:ff"), "aa:bb:cc");
  EXPECT_EQ(mask_mac("AA-BB-CC-DD-EE-FF"), "aa:bb:cc");
  EXPECT_EQ(mask_mac("aabbccddeeff"), "aa:bb:cc");
  EXPECT_EQ(mask_mac("aabb"), "");
  EXPECT_EQ(mask_mac("not-a-mac"), "");
}

TEST(SystemInfoPrivacy, SsidKeepsAtMostTwoCodePoints) {
  EXPECT_EQ(mask_ssid("MyHomeNetwork"), "My***");
  EXPECT_EQ(mask_ssid("a"), "a***");
  EXPECT_EQ(mask_ssid(""), "");
  EXPECT_EQ(mask_ssid("\xe5\xae\xb6\xe7\xbd\x91\xe7\xbb\x9c"), "\xe5\xae\xb6\xe7\xbd\x91***");
}

TEST(SystemInfoClassification, AdapterTypes) {
  EXPECT_EQ(classify_nic_type("Ethernet", "Intel(R) Ethernet Controller", false, false, false), nic_type_e::ethernet);
  EXPECT_EQ(classify_nic_type("Wi-Fi", "Intel(R) Wi-Fi 6E AX211", true, false, false), nic_type_e::wifi);
  EXPECT_EQ(classify_nic_type("tailscale0", "", false, true, false), nic_type_e::mesh);
  EXPECT_EQ(classify_nic_type("docker0", "", false, false, false), nic_type_e::virtual_interface);
  EXPECT_EQ(classify_nic_type("vEthernet (WSL)", "Hyper-V Virtual Ethernet Adapter", false, false, false), nic_type_e::virtual_interface);
  EXPECT_EQ(classify_nic_type("utun3", "", false, false, true), nic_type_e::other);
}

TEST(SystemInfoClassification, StreamingAdapterSelection) {
  EXPECT_TRUE(is_streaming_adapter("", { "GPU A" }, 0));
  EXPECT_FALSE(is_streaming_adapter("", { "GPU A" }, 1));
  EXPECT_TRUE(is_streaming_adapter("NVIDIA GeForce RTX 4080", { "NVIDIA GeForce RTX 4080" }, 1));
  EXPECT_TRUE(is_streaming_adapter("nvidia geforce rtx 4080", { "NVIDIA GeForce RTX 4080" }, 1));
  EXPECT_TRUE(is_streaming_adapter("/dev/dri/renderD129", { "card1", "renderD129" }, 1));
  EXPECT_FALSE(is_streaming_adapter("renderD128", { "card1", "renderD129" }, 1));
}

TEST(SystemInfoJson, ContractShapeIsStable) {
  snapshot_t snapshot;
  snapshot.checked_at = "2026-11-03T00:00:00Z";
  snapshot.cpu.model = "Example CPU";
  snapshot.cpu.vendor = "intel";
  snapshot.cpu.cores = 8;
  snapshot.cpu.threads = 16;
  snapshot.cpu.base_mhz = 3400;
  snapshot.gpus.push_back({ "Example GPU", "nvidia", "31.0.15.5222", 16384, true, {} });
  snapshot.gpus.push_back({ "Other GPU", "amd", "", std::nullopt, false, {} });
  nic_info_t nic;
  nic.name = "Ethernet";
  nic.description = "Example adapter";
  nic.type = nic_type_e::ethernet;
  nic.up = true;
  nic.link_speed_mbps = 2500;
  nic.ipv4 = { "10.0.1.10" };
  nic.ipv6 = { "2001:db8::10" };
  nic.mac = "aa:bb:cc";
  nic.is_default_route = true;
  snapshot.nics.push_back(nic);
  snapshot.os = { "Example Linux", "24.04", "6.8.0-45-generic" };
  snapshot.host = "example-host";

  const auto json = to_json(snapshot);
  EXPECT_EQ(json.at("version"), 1);
  EXPECT_EQ(json.at("checked_at"), "2026-11-03T00:00:00Z");
  EXPECT_EQ(json.at("cpu").at("model"), "Example CPU");
  EXPECT_EQ(json.at("cpu").at("cores"), 8);
  EXPECT_EQ(json.at("cpu").at("threads"), 16);
  EXPECT_EQ(json.at("cpu").at("base_mhz"), 3400);
  ASSERT_EQ(json.at("gpus").size(), 2U);
  EXPECT_EQ(json.at("gpus").at(0).at("vram_mb"), 16384);
  EXPECT_EQ(json.at("gpus").at(0).at("is_streaming_adapter"), true);
  EXPECT_TRUE(json.at("gpus").at(1).at("vram_mb").is_null());
  ASSERT_EQ(json.at("nics").size(), 1U);
  EXPECT_EQ(json.at("nics").at(0).at("type"), "ethernet");
  EXPECT_EQ(json.at("nics").at(0).at("link_speed_mbps"), 2500);
  EXPECT_EQ(json.at("nics").at(0).at("mac"), "aa:bb:cc");
  EXPECT_EQ(json.at("nics").at(0).at("is_default_route"), true);
  EXPECT_TRUE(json.at("nics").at(0).at("wifi").at("signal").is_null());
  EXPECT_EQ(json.at("os").at("build"), "6.8.0-45-generic");
  EXPECT_EQ(json.at("host").at("name"), "example-host");
}

#if !defined(_WIN32)
TEST(SystemInfoProbe, CollectsThisHost) {
  const auto snapshot = collect("");
  EXPECT_FALSE(snapshot.host.empty());
  EXPECT_GT(snapshot.cpu.threads, 0);
  EXPECT_FALSE(snapshot.os.name.empty());
  for (const auto &nic : snapshot.nics) {
    EXPECT_TRUE(nic.mac.empty() || nic.mac.size() == 8) << nic.name;
    for (const auto &address : nic.ipv6) {
      EXPECT_NE(address.rfind("fe80", 0), 0U) << nic.name << ' ' << address;
      EXPECT_NE(address.rfind("FE80", 0), 0U) << nic.name << ' ' << address;
    }
  }
}
#endif

/**
 * @file component_manifest.h
 * @brief Read-only validation of the active Sidecar package before execution.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <boost/property_tree/json_parser.hpp>

#include "src/platform/windows/virtual_device_host/protocol.h"

namespace platf::ds5 {
  /** Validate the installer-written manifest and both binaries at fixed paths.
   * The caller enforces the active directory's location. Hash returns an optional
   * SHA-256 string, or no value when a file cannot be read.
   */
  template<class Hash>
  bool validate_component_manifest(const std::filesystem::path &active_root,
                                   Hash hash, std::string &error) {
    try {
      boost::property_tree::ptree manifest;
      std::ifstream stream(active_root / "component.json", std::ios::binary);
      boost::property_tree::read_json(stream, manifest);
      boost::property_tree::ptree compatibility;
      std::ifstream contract(active_root / "component-compatibility.json", std::ios::binary);
      boost::property_tree::read_json(contract, compatibility);
      // The payload stage travels with the component so a manager can refuse a
      // capability the artifact does not have. Both fields stay optional: a
      // manager that predates them writes only the digests, and rejecting such
      // an installation before the new component ships would strand it.
      const auto payload_policy_known = [](const std::string &stage) {
        return stage.empty() || stage == "legacy-v1" || stage == "composite-only-v1" ||
               stage == "audited-hid-v1";
      };
      const auto contract_policy = compatibility.get<std::string>("payload_policy", "");
      const auto manifest_policy = manifest.get<std::string>("payload_policy", "");
      const bool hid_only_agrees =
        compatibility.get<bool>("capabilities.hid_only", true) ||
        !manifest.get<bool>("capabilities.hid_only", true);
      if (manifest.get<std::uint32_t>("protocol", 0) != virtual_device_host::protocol::VERSION ||
          compatibility.get<std::uint32_t>("schema", 0) != 1 ||
          compatibility.get<std::string>("component_version", "").empty() ||
          manifest.get<std::string>("component_version", "") != compatibility.get<std::string>("component_version") ||
          manifest.get<std::string>("usbip_policy", "") != "externally-managed-v1" ||
          compatibility.get<std::string>("usbip.policy", "") != "externally-managed-v1" ||
          compatibility.get<std::string>("usbip.version", "").empty() ||
          manifest.get<std::string>("usbip_version", "") != compatibility.get<std::string>("usbip.version") ||
          manifest.get<std::string>("sidecar_file", "") != "Sunshine.Ds5Sidecar.exe" ||
          !payload_policy_known(contract_policy) ||
          !payload_policy_known(manifest_policy) ||
          (!manifest_policy.empty() && manifest_policy != contract_policy) ||
          !hid_only_agrees) {
        error = "The active DualSense component manifest is incomplete or incompatible";
        return false;
      }
      // sha256 is the installed Core DLL digest. sidecar_package_sha256, when
      // present, describes the downloaded ZIP and cannot validate a binary.
      for (const auto &[field, file] : {
             std::pair { "sidecar_sha256", "Sunshine.Ds5Sidecar.exe" },
             std::pair { "sha256", "HIDMaestro.Core.dll" },
             std::pair { "compatibility_sha256", "component-compatibility.json" } }) {
        auto expected = manifest.get<std::string>(field, "");
        std::ranges::transform(expected, expected.begin(), [](unsigned char character) {
          return static_cast<char>(std::tolower(character));
        });
        const auto actual = hash(active_root / file);
        if (expected.size() != 64 || !actual || *actual != expected) {
          error = std::string("The active DualSense component failed digest verification: ") + file;
          return false;
        }
      }
      return true;
    }
    catch (const std::exception &exception) {
      error = std::string("Unable to validate the active DualSense component manifest: ") + exception.what();
      return false;
    }
  }
}  // namespace platf::ds5

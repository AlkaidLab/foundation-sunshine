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
      if (manifest.get<std::uint32_t>("protocol", 0) != virtual_device_host::protocol::VERSION ||
          manifest.get<std::string>("component_version", "") != "1.4.0" ||
          manifest.get<std::string>("sidecar_file", "") != "Sunshine.Ds5Sidecar.exe") {
        error = "The active DualSense component manifest is incomplete or incompatible";
        return false;
      }
      // sha256 is the installed Core DLL digest. sidecar_package_sha256, when
      // present, describes the downloaded ZIP and cannot validate a binary.
      for (const auto &[field, file] : {
             std::pair { "sidecar_sha256", "Sunshine.Ds5Sidecar.exe" },
             std::pair { "sha256", "HIDMaestro.Core.dll" } }) {
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

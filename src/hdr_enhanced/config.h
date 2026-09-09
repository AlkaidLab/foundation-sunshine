/**
 * @file src/hdr_enhanced/config.h
 * @brief Independent HDR backend configuration and version ownership.
 */
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <boost/smart_ptr/shared_ptr.hpp>
#include <nlohmann/json.hpp>

namespace hdr_enhanced {
  inline constexpr std::string_view NVIDIA_RTX_VIDEO_BACKEND = "alkaidlab.nvidia_rtx_video";
  inline constexpr char NVIDIA_RTX_VIDEO_ADAPTER[] = "foundation_rtx_video_adapter.dll";
  inline constexpr char NVIDIA_RTX_VIDEO_RUNTIME[] = "nvngx_truehdr.dll";

  struct settings_t {
    std::string selected_backend;
    std::unordered_map<std::string, std::string> versions;
    bool
    operator==(const settings_t &) const = default;
  };

  struct backend_use_t {
    std::string id;
    std::string version;
    std::filesystem::path path;
  };

  struct result_t {
    int status = 200;
    std::string error;
    settings_t settings;
    std::string etag;
    bool changed = false;
  };

  /** Parse the complete public configuration document without loading any DLL. */
  bool
  parse_settings(const nlohmann::json &input, settings_t &output);
  nlohmann::json
  settings_json(const settings_t &settings);
  bool
  valid_version(std::string_view version);

  /** Owns configuration transactions; streaming readers retain immutable version references. */
  class manager_t {
  public:
    manager_t(std::filesystem::path config_file, std::filesystem::path component_root,
      nlohmann::json trusted_components);
    ~manager_t();
    manager_t(const manager_t &) = delete;
    manager_t &
    operator=(const manager_t &) = delete;

    bool
    initialize();
    result_t
    query();
    result_t
    update(const settings_t &requested, std::optional<std::string_view> if_match,
      std::string_view operation_id = {});
    boost::shared_ptr<const backend_use_t>
    acquire_selected();
    nlohmann::json
    status();
    std::filesystem::path
    maintenance_path() const;
    result_t
    begin_maintenance(std::string_view id, std::string &operation_id);
    result_t
    verify_maintenance(std::string_view id, std::string_view operation_id);
    result_t
    inspect_maintenance(std::string_view id, std::string &operation_id);
    result_t
    finish_maintenance(std::string_view id, std::string_view operation_id);
    result_t
    recover_maintenance(std::string_view id);

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };

  manager_t &
  manager();
}  // namespace hdr_enhanced

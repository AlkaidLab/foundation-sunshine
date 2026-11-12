/**
 * @file src/ds5/tuning_profile.h
 * @brief ALK-DS5T/1 profile metadata, validation and persistence.
 *
 * The table in tuning_descriptors.inc is the single source of ranges, defaults
 * and UI metadata for the 106 keys in the DS5 tuning design.  The Web UI
 * renders that table; it does not keep a second key list or hard-code ranges.
 *
 * Source: docs/architecture/ds5-platform-design.zh.md sections 16.4.1-16.4.12
 * on branch docs/ds5-tuning-profile (commit 29f7673fb, read 2026-10-31).
 * Key ids are append-only.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace ds5::tuning {
  enum class ValueType : std::uint32_t {
    boolean = 1,
    u32 = 2,
    i32 = 3,
    f32 = 4,
    enumeration = 5,
  };

  enum class Group : std::uint32_t {
    general = 0,
    haptics = 1,
    rumble = 2,
    adaptive_trigger = 3,
    sticks = 4,
    trigger_axes = 5,
    motion = 6,
    touchpad = 7,
    lighting = 8,
    audio = 9,
    compatibility = 10,
    sync = 11,
    routing = 12,
  };

  enum class Control : std::uint32_t {
    slider = 1,
    toggle = 2,
    segmented = 3,
    menu = 4,
    color_channel = 5,
    curve = 6,
    chip_multiselect = 7,
    preset_grid = 8,
  };

  enum class Tier : std::uint32_t {
    common = 1,
    advanced = 2,
  };

  enum class Scale : std::uint32_t {
    linear = 1,
    log = 2,
  };

  enum class Authority : std::uint32_t {
    device = 0,
    host = 1,
    client = 2,
    shared_host = 3,
    shared_client = 4,
    product = 5,
  };

  enum class Stage : std::uint32_t {
    host_source = 1,
    host_endpoint = 2,
    client_sink = 3,
    client_endpoint = 4,
  };

  enum class Live : std::uint32_t {
    immediate = 1,
    ramp = 2,
    next_packet = 3,
    next_session = 4,
  };

  enum : std::uint32_t {
    FLAG_EXPERT = 1u << 0,
    FLAG_REQUIRES_USB = 1u << 1,
    FLAG_REQUIRES_BT = 1u << 2,
    FLAG_REQUIRES_PCM = 1u << 3,
    FLAG_REQUIRES_TRIGGER = 1u << 4,
    FLAG_PERSISTED = 1u << 5,
  };

  struct descriptor_t {
    std::uint32_t id;
    const char *key;
    ValueType type;
    double min_value;
    double max_value;
    double step;
    double default_number;
    const char *default_enum;
    const char *const *enum_names;
    std::uint32_t enum_count;
    Group group;
    Control control;
    Tier tier;
    Scale scale;
    Authority authority;
    Stage stage;
    Live live;
    std::uint32_t flags;
    const char *unit;
    const char *legacy_key;
    const char *title_i18n_key;
    const char *description_i18n_key;
  };

  using value_t = std::variant<bool, std::uint32_t, float, std::string>;

  const std::vector<descriptor_t> &
  descriptors();
  const descriptor_t *
  find_descriptor(std::uint32_t id);
  const descriptor_t *
  find_descriptor(std::string_view key);
  value_t
  default_value(const descriptor_t &descriptor);
  std::string
  human_label(const descriptor_t &descriptor);
  std::string
  group_name(Group group);
  std::string
  control_name(Control control);
  std::string
  tier_name(Tier tier);
  std::string
  authority_name(Authority authority);
  std::string
  wire_reason(std::uint32_t id);
  std::string
  wire_note(std::uint32_t id);
  std::filesystem::path
  profile_path_for(std::string_view config_file, const std::filesystem::path &appdata);

  struct patch_result_t {
    bool ok { false };
    int http_status { 400 };
    std::uint32_t code { 9 };
    std::string error;
    nlohmann::json body;
  };

  /**
   * @brief Validated, atomically-replaced profile file.
   *
   * The file only contains keys this build can apply.  A valid key that the
   * running path does not implement is reported as UNAVAILABLE in the POST
   * result and is never written to disk.
   */
  class store_t {
  public:
    explicit store_t(std::filesystem::path path);

    bool
    load(std::string *error = nullptr);
    const std::filesystem::path &
    path() const noexcept { return path_; }
    std::uint64_t
    revision() const;

    nlohmann::json
    snapshot_json() const;
    patch_result_t
    apply_json(const nlohmann::json &body);

    /**
     * @brief Publish the current effective values to the native feedback path.
     */
    void
    publish() const;

  private:
    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::map<std::uint32_t, value_t> values_;
    std::uint64_t revision_ { 0 };
    std::string load_error_;
    std::vector<std::string> load_warnings_;

    nlohmann::json
    snapshot_locked() const;
    std::map<std::uint32_t, value_t>
    effective_locked() const;
    bool
    save_locked(const std::map<std::uint32_t, value_t> &values, std::uint64_t revision, std::string *error);
    /**
     * @brief First start only: carry the values of the earlier ds5_config.json (and the same
     *        keys in sunshine.conf) into this store and write the profile once.
     */
    void
    migrate_legacy_locked();
  };
}  // namespace ds5::tuning

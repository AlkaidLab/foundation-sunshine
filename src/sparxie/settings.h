#pragma once
#include "src/sparxie/model.h"
#include <functional>
#include <set>
namespace sparxie {
  struct GameValues {
    std::string resolution = "1920x1080";
    std::string display = "fullscreen";
    int fps = 60;
    bool vsync = false;
    double render_scale = 1.0;
    std::string preset = "high";
    bool hdr = false;
    // Title-specific values cross the JSON boundary without changing the
    // common settings shape used by existing adapters.
    json extra = json::object();
  };
  struct SettingsResult {
    Status status;
    bool running = false;
    GameValues values;
    std::vector<std::string> unavailable;
    std::vector<std::string> applied;
  };
  std::vector<std::uint8_t>
  sleepy_write(std::string_view text, const std::vector<std::uint8_t> &magic);
  std::string
  sleepy_read(const std::vector<std::uint8_t> &bytes, const std::vector<std::uint8_t> &magic);
  SettingsResult
  read_game_settings(const Install &install, Registry &registry, const std::vector<std::vector<std::uint8_t>> &zzz_magics, const std::function<bool()> &running);
  SettingsResult
  write_game_settings(const Install &install, Registry &registry, const GameValues &values, const std::vector<std::vector<std::uint8_t>> &zzz_magics, const std::function<bool()> &running, const fs::path &backup_dir, const std::string &policy = "restore", const std::set<std::string> &fields = {});
  // Arknights: Endfield's display (HKCU\\Software\\Hypergryph\\Endfield): the game applies its own saved size and window mode after it has started, over any launch argument, so a launch writes
  // them first, with the same backup and restore as the other titles. `width`/`height` 0 leave the size, an empty `window` leaves the mode (fullscreen | windowed | borderless).
  Status
  write_endfield_display(const Install &install, Registry &registry, int width, int height, const std::string &window, const fs::path &backup_dir, const std::string &policy);
  // The keys of a settings object that this host manages (resolution, display, fps, vsync, renderScale, preset, hdr). An object that holds every default value, as the first builds stored, counts as holding none: the person never chose them.
  std::set<std::string>
  chosen_fields(const json &settings);
  // Launch backup restore (docs/sparxie-api.md, Game settings: restore and keep). A restore
  // policy puts the previous bytes back when the live values still equal what
  // was written. A keep policy, or a value the game changed, leaves the live
  // values and drops the backup.
  void
  reconcile_pending_settings(const Install &install, Registry &registry, const fs::path &backup_dir, const std::vector<std::vector<std::uint8_t>> &zzz_magics);
  json
  values_json(const GameValues &values);
  GameValues
  values_from_json(const json &body, const GameValues &base);
}  // namespace sparxie

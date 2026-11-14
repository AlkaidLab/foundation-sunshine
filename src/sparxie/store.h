#pragma once
#include "src/sparxie/model.h"
#include <functional>
namespace sparxie {
  struct StoreHooks {
    bool running = false;
    int fail_replace_times = 0;
    std::function<std::string()> utc_stamp;
    std::function<void(const fs::path &)> after_replace;
  };
  struct ProfileDraft {
    std::string id;
    std::string display_name;
    std::string game;
    std::string executable;
    json options = json::object();
    // The numeric target of options.fps when it is the "display" tier, already
    // resolved by the caller (the refresh rate of the streamed display); 0 keeps
    // the profile editor's own default while the tier itself stays "display".
    int display_fps = 0;
  };
  struct ConfigView {
    Status status;
    std::string state;
    int profile_count = 0;
    json document = json::object();
  };
  struct WriteResult {
    Status status;
    std::string state;
    std::string profile;
    std::string backup;
  };
  ConfigView
  read_store(const fs::path &dir);
  WriteResult
  add_profile(const fs::path &dir, ProfileDraft draft, StoreHooks hooks);
  WriteResult
  update_profile(const fs::path &dir, const std::string &id, ProfileDraft draft, StoreHooks hooks);
  // Takes the profiles with these ids out of Sparxie's config (backed up first). An id that is not there is ignored; the result's profile is the number removed as text.
  WriteResult
  remove_profiles(const fs::path &dir, const std::vector<std::string> &ids, StoreHooks hooks);
  std::string
  next_profile_id(const json &document, const std::string &prefix);
  // The per-application Sparxie options as the host stores them: the profile
  // values plus input (touch | kbm), which lives only in the application.
  json
  default_options();
  // input is validated (touch or kbm) and filled with its default when absent;
  // an unknown input is rejected. Everything else is kept as given.
  Status
  normalize_options(json &options);
  // options.fps may be the tier "display": the host writes Sparxie's numeric
  // targetFps, taken from the streamed display's refresh rate (fallback 120).
  int
  resolve_target_fps(const json &options, int display_fps);
  // Whether the target tier is "display" (the streamed display's refresh rate).
  bool
  fps_is_display(const json &options);
  // While Sparxie's unlock owns the frame rate of a touch application, the
  // game's own tier is not that application's to store or write.
  bool
  unlock_owns_fps(const json &block);
  json
  settings_without_game_fps(const std::string &mode, const json &options, const json &settings);
  json
  options_from_hoyo(const json &hoyo);
  // The sparxie block of a Sunshine application and the application around it.
  json
  app_block(const Install &install, const std::string &profile, const std::string &mode, const json &options, const std::string &templ, const json &settings = json::object(), const std::string &restore = "restore", const std::string &cover = "official");
  json
  make_app(const std::string &name, const std::string &cmd, bool elevated, const Install &install, const json &block);
  Status
  validate_document(const json &document);
}  // namespace sparxie

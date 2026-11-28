#pragma once
#include "src/sparxie/model.h"
#include "src/sparxie/sparxie_build.h"
#include <functional>
#include <optional>
namespace sparxie {
  struct StoreHooks {
    bool running = false;
    int fail_replace_times = 0;
    std::function<std::string()> utc_stamp;
    std::function<void(const fs::path &)> after_replace;
    // The Sparxie build the write is checked against (tests; unset: the host looks at the install around the data folder, see detect_build)
    std::optional<SparxieBuild> build;
  };
  // Whether the chosen API argument and custom run in the result are the host's to take out again. One the person had typed already is not.
  struct LaunchOwnership {
    bool api = true;
    bool run = true;
  };
  // What the host wants in a profile's extra launch arguments and what it may have left there (see with_launch_arguments), and what making that change did.
  struct LaunchChange {
    std::string api;
    std::vector<std::string> our_api;
    std::string run;
    std::vector<std::string> our_runs;
    LaunchOwnership owned;
    std::optional<std::string> before;  // the arguments as they were, when they were changed
    bool
    empty() const { return api.empty() && our_api.empty() && run.empty() && our_runs.empty(); }
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
    // made in the same write as the profile; filled in with what it did, which counts when the write succeeds
    LaunchChange *launch = nullptr;
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
  // The config as the installed Sparxie build would read it: a profile that build would reject makes the state "unreadable".
  ConfigView
  read_store(const fs::path &dir);
  ConfigView
  read_store(const fs::path &dir, const SparxieBuild &build);
  WriteResult
  add_profile(const fs::path &dir, ProfileDraft draft, StoreHooks hooks);
  WriteResult
  update_profile(const fs::path &dir, const std::string &id, ProfileDraft draft, StoreHooks hooks);
  // Takes the profiles with these ids out of Sparxie's config (backed up first). An id that is not there is ignored; the result's profile is the number removed as text.
  WriteResult
  remove_profiles(const fs::path &dir, const std::vector<std::string> &ids, StoreHooks hooks);
  // Sparxie's limit on a profile's extra launch arguments (LaunchSettingsRules), counted the way it counts: UTF-16 code units.
  inline constexpr std::size_t kMaxLaunchArguments = 512;
  std::size_t
  launch_arguments_length(const std::string &utf8);
  // The extra launch arguments a profile holds; empty when it holds none.
  std::string
  launch_arguments_of(const json &profile);
  // Sparxie's extra launch arguments with the graphics API argument the host wants: `wanted` is appended unless it is there already, the other API arguments give
  // way to it, and every one of `ours` (what the host put there before) that is not wanted any more goes. Words are told apart as a command line does (what
  // stands between quotes is never an argument of its own), and everything else stays as it was typed. `added`: whether `wanted` was appended.
  std::string
  with_graphics_argument(const std::string &arguments, const std::string &wanted, const std::vector<std::string> &ours, bool *added = nullptr);
  // Both things the host puts into those arguments: the graphics API argument (as above) and the person's custom arguments from the application, which go in and
  // out as one run of words (`our_runs` are the runs the host put there before), so that a value like the 3 of "-quality 3" never leaves on its own.
  std::string
  with_launch_arguments(const std::string &arguments, const std::string &api, const std::vector<std::string> &our_api, const std::string &run, const std::vector<std::string> &our_runs, LaunchOwnership *owned = nullptr);
  // with_launch_arguments on a profile's launch.extraArguments, and nothing else of the profile. A profile that already reads that way is not written (state "unchanged").
  // `before` gets what the arguments were when they are written.
  WriteResult
  set_launch_arguments(const fs::path &dir, const std::string &id, const std::string &api, const std::vector<std::string> &our_api, const std::string &run, const std::vector<std::string> &our_runs, StoreHooks hooks, LaunchOwnership *owned = nullptr, std::string *before = nullptr);
  // Puts a profile's extra launch arguments back as they were, for a save that did not end well.
  WriteResult
  restore_launch_arguments(const fs::path &dir, const std::string &id, const std::string &arguments, StoreHooks hooks);
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
  unlock_owns_fps(const json &block, int display_fps = 0);
  json
  settings_without_game_fps(const std::string &mode, const json &options, const json &settings);
  json
  options_from_hoyo(const json &hoyo);
  // The same for the second generation's Endfield profile, whose options are the plugin's "settings".
  json
  options_from_settings(const json &settings);
  // The options of a stored profile, whichever generation wrote it.
  json
  options_from_profile(const json &profile);
  // The sparxie block of a Sunshine application and the application around it.
  json
  app_block(const Install &install, const std::string &profile, const std::string &mode, const json &options, const std::string &templ, const json &settings = json::object(), const std::string &restore = "restore", const std::string &cover = "official");
  json
  make_app(const std::string &name, const std::string &cmd, bool elevated, const Install &install, const json &block);
  // The rules of the installed build's AppConfigValidator that would make Sparxie put the whole file aside. Without `build` the document is judged as an
  // unknown build: both generations' spellings of Endfield pass.
  Status
  validate_document(const json &document, const SparxieBuild *build = nullptr);
}  // namespace sparxie

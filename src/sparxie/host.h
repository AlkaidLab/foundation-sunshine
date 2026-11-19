#pragma once
#include "src/sparxie/art.h"
#include "src/sparxie/scan.h"
#include "src/sparxie/settings.h"
#include "src/sparxie/store.h"
#include "src/sparxie/stream_display.h"
#include <chrono>
namespace sparxie {
  struct HostEnv {
    fs::path install_root;
    fs::path data_dir;
    bool data_dir_provisional = true;
    fs::path host_data;
    fs::path apps_file;
    fs::path covers_dir;
    fs::path sparxie_exe;
    std::string launch_template;
    Registry *registry = nullptr;
    std::function<bool(const std::vector<std::string> &)> process_running;
    std::vector<std::vector<std::uint8_t>> zzz_magics;
    // The refresh rate of the display this host streams, in Hz (0 when it
    // cannot be read). It resolves the Sparxie tier fps "display" at launch.
    std::function<int()> display_refresh;
    // The size of that display in pixels ({0, 0} when it cannot be read): Zenless Zone Zero stores its resolution as a position in a list the game makes from this.
    std::function<std::pair<int, int>()> display_size;
    // true while a stream is running: the display then is the one a game launched now covers
    std::function<bool()> stream_running;
    // 提升游戏优先级: raises the game process (image path, how long to wait for it) to above normal and returns how many it raised; blocks, so the launch calls it from a thread of its own
    std::function<int(const fs::path &, std::chrono::milliseconds)> raise_priority;
    StoreHooks store_hooks;
    fs::path component_dir;
    // Storefront roots for detection; empty means the host resolves them from
    // the signed-in user's registry and ProgramData. Tests point them at fake
    // Steam and Epic trees.
    fs::path steam_root;
    fs::path epic_manifests;
    // Install roots of the official Hypergryph launcher; empty means the
    // host tries the default locations (Program Files and LocalAppData).
    // Tests point them at a fake launcher tree.
    std::vector<fs::path> hypergryph_roots;
    // The InstallLocation answers of vendor launchers' uninstall keys (the
    // key names come from vendors.h); unset means the host reads the machine's
    // own records. Tests answer with fixture folders.
    std::function<std::vector<fs::path>(const std::vector<std::string> &)> uninstall_locations;
    // Starts the Sparxie program in the signed-in user's session (Sparxie is a window; it must not run as the service).
    std::function<Status(const fs::path &)> launch_gui;
    // Folder search limits; tests lower them.
    ScanOptions scan;
    // Where the host writes a warning (tests capture it; the host logs it).
    std::function<void(const std::string &)> warn;
    // How many times a request rewrote apps.json; the HTTP layer reloads the host application list when it is not zero
    int apps_writes = 0;
  };
  struct ApiResult {
    Status status;
    json body = json::object();
  };
  fs::path
  find_sparxie_exe(const fs::path &root);
  fs::path
  resolve_data_dir(const fs::path &exe, const fs::path &override_dir, const fs::path &fallback);
  // The note the touch launcher leaves in the host's Sparxie data folder (launch-notice.json) after a launch that needed its safety net: the object, or null.
  json
  launch_notice(const HostEnv &env);
  ApiResult
  component_status(HostEnv &env);
  ApiResult
  identify_path(HostEnv &env, const fs::path &path, const std::string &game = {}, const std::string &region = {});
  // Searches a chosen folder level by level for the game's install: { path, game?, region?, depth? }.
  ApiResult
  scan_folder(HostEnv &env, const json &request);
  // Lists drives or one folder as the signed-in user, read-only: { path?, files? }.
  ApiResult
  browse_path(HostEnv &env, const json &request);
  // Opens the Sparxie program in the signed-in user's session.
  ApiResult
  open_component(HostEnv &env);
  ApiResult
  list_games(HostEnv &env);
  ApiResult
  list_profiles(HostEnv &env);
  ApiResult
  create_app(HostEnv &env, const json &request);
  // Changes a game application in place (name, launch mode, game settings, cover); touch launch adds the Sparxie profile when the game app has none.
  ApiResult
  update_app(HostEnv &env, int index, const json &request);
  ApiResult
  import_apps(HostEnv &env, const json &request);
  ApiResult
  acknowledge_warning(HostEnv &env);
  ApiResult
  game_settings_for(HostEnv &env, const std::string &install_id, const std::string &exe = {}, const std::string &region = {});
  ApiResult
  write_game_settings_for(HostEnv &env, const json &request);
  ApiResult
  set_cover(HostEnv &env, int index, const std::string &cover);
  /// The cover is composed from the background and the logo, or is `finished_cover` (a store capsule) as it is; the hero and the banner always come from the background.
  ApiResult
  publish_art(HostEnv &env, const std::string &install_id, const ArtUrls &urls, const Rgba &background, const Rgba &logo, const Rgba &icon, const Rgba *finished_cover = nullptr);
  /// Composes the portrait cover of an install again from a background and a logo (the cover file of that art version and its badged copy, rewritten when the
  /// picture differs). True when the plain cover changed. The apps keep pointing at the same file.
  bool
  publish_cover(HostEnv &env, const std::string &install_id, const std::string &version, const Rgba &background, const Rgba &logo);
  /// Writes posters/<install>/icon.png (the program icon, 256x256 PNG) when it differs from the one on disk; true when a file was written.
  bool
  publish_icon(HostEnv &env, const std::string &install_id, const Rgba &icon);
  /// Composes posters/<install>/hero.png (the 16:9 landscape poster) from a background and a logo, and points the host-made heroes (empty, the old banner strip,
  /// another hero.png) of the official-cover apps of that install at it; a custom hero is never touched. Returns how many apps changed, or -1 when no picture was made.
  int
  publish_hero(HostEnv &env, const std::string &install_id, const Rgba &background, const Rgba &logo);
  ApiResult
  refresh_official(HostEnv &env, const std::function<std::optional<std::string>(const std::string &)> &get_url, const std::function<Rgba(std::string_view)> &decode, const std::function<std::optional<std::string>(const std::string &, const std::string &)> &post_url = {});
  /// The badged copy of the newest official cover of an install when it is missing (an install whose posters were made before the Sparxie badge), and the applications
  /// of that install pointed at it; returns how many changed (1 when only the file was made).
  int
  publish_cover_badge(HostEnv &env, const std::string &install_id);
  /// An official-cover Sparxie application without a poster gets the newest official cover already on disk; returns how many changed.
  int
  attach_official_covers(HostEnv &env);
  /// Profiles in Sparxie's config that an earlier failed save left behind: ids of the host's own pattern <game>-<region>-<n> that no application uses. They are taken out (the config is backed up first); returns how many went, and does nothing while Sparxie is open or the application list cannot be read.
  int
  remove_orphan_profiles(HostEnv &env);
  // Writes what the application's settings and its display choice ask for into the game before it starts. `session` is the mode the client asked for;
  // `arguments` (when given) receives the arguments to append to the command of a game that starts directly.
  Status
  prepare_launch(HostEnv &env, const std::string &app_name, const std::string &cmd, const SessionMode &session = {}, std::string *arguments = nullptr);
  // The number of launches that wrote settings so far. A session remembers it when it starts; end_session leaves everything alone when a newer launch has started since.
  std::uint64_t
  launch_generation();
  // The application ended (the game quit or the stream ended): puts back the values its launch wrote (see reconcile_pending_settings): each value goes back where the game
  // has not changed it since, and a value the game changed during play stays as the game has it. 409 game_running while the game process is still there.
  Status
  end_session(HostEnv &env, const std::string &app_name, const std::string &cmd, std::uint64_t started_generation);
  // end_session, retried every `poll` until the game process is gone or `wait` is over; true when the values were put back (or there was nothing to do). Blocks.
  bool
  finish_session(HostEnv &env, const std::string &app_name, const std::string &cmd, std::uint64_t started_generation, std::chrono::milliseconds wait, std::chrono::milliseconds poll);
  // At host start: puts back what a session left when the host went down before it ended (every Sparxie application whose game is not running). Returns how many installs it looked at.
  int
  recover_pending_settings(HostEnv &env);
  // 提升游戏优先级 of the application that is being launched (direct start and Sparxie alike): waits for the game process and raises it; blocks up to `wait`.
  // Returns how many processes were raised, 0 when the application has no boost on or the host has no way to raise a priority.
  int
  run_launch_boost(HostEnv &env, const std::string &app_name, const std::string &cmd, std::chrono::milliseconds wait);
  // The text of apps.json with every `sparxie` block typed again (the generic writer stringifies them); the same text when there is nothing to fix.
  std::string
  typed_apps_text(const std::string &text);
  // Types the sparxie blocks of the apps file on disk again after the generic writer saved it.
  void
  heal_apps_file(const std::string &path);
}  // namespace sparxie

/**
 * @file src/process.h
 * @brief Declarations for the startup and shutdown of the apps started by a streaming Session.
 */
#pragma once

#ifndef __kernel_entry
  #define __kernel_entry
#endif

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <boost/process/v1.hpp>

#include "client_interface.h"
#include "config.h"
#include "generated_cover.h"
#include "platform/common.h"
#include "rtsp.h"
#include "utility.h"

namespace proc {
  using file_t = util::safe_ptr_v2<FILE, int, fclose>;

  typedef config::prep_cmd_t cmd_t;
  struct scmd_t {
    scmd_t(std::string &&id, std::string &&name, std::string &&do_cmd, bool &&elevated):
        id(std::move(id)), name(std::move(name)), do_cmd(std::move(do_cmd)), elevated(std::move(elevated)) {}
    std::string id;
    std::string name;
    std::string do_cmd;
    bool elevated;
  };
  /**
   * pre_cmds -- guaranteed to be executed unless any of the commands fail.
   * detached -- commands detached from Sunshine
   * cmd -- Runs indefinitely until:
   *    No session is running and a different set of commands it to be executed
   *    Command exits
   * working_dir -- the process working directory. This is required for some games to run properly.
   * cmd_output --
   *    empty    -- The output of the commands are appended to the output of sunshine
   *    "null"   -- The output of the commands are discarded
   *    filename -- The output of the commands are appended to filename
   */
  struct ctx_t {
    std::vector<cmd_t> prep_cmds;
    std::vector<scmd_t> menu_cmds;

    /**
     * Some applications, such as Steam, either exit quickly, or keep running indefinitely.
     *
     * Apps that launch normal child processes and terminate will be handled by the process
     * grouping logic (wait_all). However, apps that launch child processes indirectly or
     * into another process group (such as UWP apps) can only be handled by the auto-detach
     * heuristic which catches processes that exit 0 very quickly, but we won't have proper
     * process tracking for those.
     *
     * For cases where users just want to kick off a background process and never manage the
     * lifetime of that process, they can use detached commands for that.
     */
    std::vector<std::string> detached;

    std::string name;
    std::string cmd;
    std::string working_dir;
    std::string output;
    std::string image_path;
    std::string cover_style;
    bool xbox_app = false;
    client_interface::app_facts_t fs_facts;
    std::string id;
    bool elevated;
    bool auto_detach;
    bool wait_all;
    int mouse_mode;  ///< 0=auto (use global config), 1=force virtual mouse, 2=force SendInput
    int gamepad_mode;  ///< 0=inherit global, 1=auto, 2=Xbox 360, 3=DualShock 4, 4=DualSense
    std::chrono::seconds exit_timeout;
    std::optional<rtsp_stream::synthetic_hdr_config_t> rtx_hdr;
    std::optional<rtsp_stream::dlssnr_config_t> dlssnr;
  };

  class proc_t {
  public:
    KITTY_DEFAULT_CONSTR_MOVE_THROW(proc_t)

    proc_t(
      boost::process::v1::environment &&env,
      std::vector<ctx_t> &&apps):
        _app_id(0),
        _env(std::move(env)),
        _apps(std::move(apps)) {}

    int
    execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session);

    /**
     * @return `_app_id` if a process is running, otherwise returns `0`
     */
    int
    running();

    ~proc_t();

    const std::vector<ctx_t> &
    get_apps() const;
    std::vector<ctx_t> &
    get_apps();
    void
    set_apps(std::vector<ctx_t> apps);
    std::string
    get_app_image(int app_id);
    std::string
    get_app_name(int app_id);
    std::string
    get_app_cmd(int app_id);
    /** Return the PID of the process started for the current app, if any. */
    std::optional<unsigned long>
    get_running_pid() const;
    std::string
    get_last_run_app_name();
    std::optional<rtsp_stream::synthetic_hdr_config_t>
    get_app_rtx_hdr_config(int app_id) const;
    std::optional<rtsp_stream::dlssnr_config_t>
    get_app_dlssnr_config(int app_id) const;
    const boost::process::v1::environment &
    get_env() const;
    boost::process::v1::environment &
    get_env();
    void
    set_env(boost::process::v1::environment env);
    void
    run_menu_cmd(std::string cmd_id);
    void
    terminate();
    std::string
    get_apps_etag() const;

  private:
    /// Start and end of the application run in one place at a time: the global cancel, a new launch and the shutdown all end the running application, and two of them
    /// walking the undo commands together read `_app` while the other replaces it. Recursive: `execute()` ends the previous run through `terminate()`.
    /// Moving a `proc_t` (the application list is replaced on a reload) leaves the lock of the destination alone: it may be held by another thread right then.
    struct lifecycle_mutex_t: std::recursive_mutex {
      lifecycle_mutex_t() = default;
      lifecycle_mutex_t(lifecycle_mutex_t &&) noexcept {}
      lifecycle_mutex_t &
      operator=(lifecycle_mutex_t &&) noexcept {
        return *this;
      }
    };
    lifecycle_mutex_t _lifecycle_mutex;

    int _app_id;

    std::string _apps_etag;

    boost::process::v1::environment _env;
    std::vector<ctx_t> _apps;
    ctx_t _app;
    std::chrono::steady_clock::time_point _app_launch_time;

    // If no command associated with _app_id, yet it's still running
    bool placebo {};

    boost::process::v1::child _process;
    boost::process::v1::group _process_group;

    file_t _pipe;
    std::vector<cmd_t>::const_iterator _app_prep_it;
    std::vector<cmd_t>::const_iterator _app_prep_begin;
  };

  /**
   * @brief Calculate a stable id based on name and image data
   * @return Tuple of id calculated without index (for use if no collision) and one with.
   */
  std::tuple<std::string, std::string>
  calculate_app_id(const std::string &app_name, std::string app_image_path, int index);

  std::string
  validate_app_image_path(std::string app_image_path);

  /// The file of one kind of art ("poster", "hero", "logo", "icon") of an application as it exists now; empty when the application has none (or the configured file is not an image).
  std::string
  app_art_asset_path(const ctx_t &app, std::string_view type);

  /// A copy of the application list, made while no save is replacing it: for the readers that must not meet a list that is being replaced.
  std::vector<ctx_t>
  apps_snapshot();

  /// The host's own placeholder picture (assets/box.png): what the poster lookup falls back to.
  std::string
  default_app_image_path();

  /// The application as the cover generator sees it. `quiet` (the application list tag) finds the poster without logging a missing one or downloading a picture.
  generated_cover::app_t
  cover_app(const ctx_t &app, bool quiet = false);

  /// The version tag of the art of an application as it is now: the configured paths' own tag plus the size and the time of every present art file, so a file replaced under
  /// the same name is a new version, plus the generated cover when the application has no picture of its own (so the switch from the placeholder to a generated cover is a
  /// new version too). A URL poster is not statted here: resolving it can download a picture.
  /// `quiet` (the application list tag) finds the poster without logging a missing one.
  /// `cover_style` is the global style the caller chose the cover with (empty: the style in force now): an answer whose file was picked with one style carries the tag of that style.
  std::string
  app_art_version(const ctx_t &app, bool quiet = false, std::string_view cover_style = {});
  void
  refresh(const std::string &file_name);
  std::optional<proc::proc_t>
  parse(const std::string &file_name);

  /**
   * @brief Initialize proc functions
   * @return Unique pointer to `deinit_t` to manage cleanup
   */
  std::unique_ptr<platf::deinit_t>
  init();

  /**
   * @brief Terminates all child processes in a process group.
   * @param proc The child process itself.
   * @param group The group of all children in the process tree.
   * @param exit_timeout The timeout to wait for the process group to gracefully exit.
   */
  void
  terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout);

  extern proc_t proc;
}  // namespace proc

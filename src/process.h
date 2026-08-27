/**
 * @file src/process.h
 * @brief Declarations for the startup and shutdown of the apps started by a streaming Session.
 */
#pragma once

#ifndef __kernel_entry
  #define __kernel_entry
#endif

#include <optional>
#include <unordered_map>

#include <boost/process/v1.hpp>

#include "config.h"
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
    std::string id;
    bool elevated;
    bool auto_detach;
    bool wait_all;
    int mouse_mode;  ///< 0=auto (use global config), 1=force virtual mouse, 2=force SendInput
    int gamepad_mode;  ///< 0=inherit global, 1=auto, 2=Xbox 360, 3=DualShock 4
    std::chrono::seconds exit_timeout;

    /**
     * Server-side per-app display scheme (App Display Profile).
     *
     * These fields mirror the `display-*` keys of apps.json. Negative/empty
     * values mean "inherit": the client request and the global configuration
     * keep working exactly as in upstream. When set, the server overrides the
     * matching client requests (priority: app profile > client > global).
     *
     * The values are mapped onto existing launch_session fields only
     * (use_vdd, custom_screen_mode, enable_sops, SUNSHINE_CLIENT_DISPLAY_NAME,
     * width/height/fps, enable_hdr); no new launch_session fields are added.
     */
    int display_target {-1};          ///< -1=inherit, 0=physical, 1=virtual
    int display_device_prep {-1};     ///< -1=inherit, 0=no_operation, 1=ensure_active, 2=ensure_primary, 3=ensure_only_display, 4=ensure_secondary
    int display_resolution_mode {-1}; ///< -1=inherit, 0=no_operation (ignore client), 1=automatic (follow client)
    int display_refresh_rate_mode {-1}; ///< -1=inherit, 0=no_operation (ignore client), 1=automatic (follow client)
    std::string display_output_name;  ///< Physical display device id used when display_target=physical; empty = default physical display
    std::string display_resolution;   ///< Fixed resolution "WxH" (advanced option); empty = not set
    std::string display_refresh_rate; ///< Fixed refresh rate in Hz (advanced option); empty = not set
    int display_hdr {-1};             ///< -1=inherit, 0=force off, 1=force on (advanced option)
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
     * @brief Apply the per-app display scheme (App Display Profile) to a launch session.
     *
     * Called after the client parameters are parsed (nvhttp) and before the
     * display preparation flow starts. Apps without a configured scheme are
     * left untouched so the client and global configuration keep full control.
     *
     * @param app_id The application id to look up.
     * @param launch_session Session to apply the scheme to (in place).
     * @return True if the app exists (whether or not it had a scheme).
     */
    bool
    apply_app_display_profile(int app_id, rtsp_stream::launch_session_t &launch_session) const;

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
    std::string
    get_last_run_app_name();
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

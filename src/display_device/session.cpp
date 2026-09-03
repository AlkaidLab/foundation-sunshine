// standard includes
#include <boost/optional/optional_io.hpp>
#include <boost/process/v1.hpp>
#include <future>
#include <thread>
#include <utility>

// local includes
#include "parsed_config.h"
#include "session.h"
#include "src/globals.h"
#include "src/platform/common.h"
#include "src/platform/windows/display_device/session_listener.h"
#include "src/platform/windows/display_device/windows_utils.h"
#ifdef _WIN32
  #include "src/platform/windows/vulkan_hdr_bridge_session.h"
#endif
#include "src/rtsp.h"
#include "to_string.h"
#include "vdd_ioctl.h"
#include "vdd_utils.h"

namespace display_device {

  class session_t::StateRetryTimer {
  public:
    /**
     * @brief A constructor for the timer.
     * @param mutex A shared mutex for synchronization.
     * @warning Because we are keeping references to shared parameters, we MUST ensure they outlive this object!
     */
    StateRetryTimer(std::mutex &mutex, std::chrono::seconds timeout = std::chrono::seconds { 5 }):
        mutex { mutex }, timeout_duration { timeout }, timer_thread {
          std::thread { [this]() {
            std::unique_lock<std::mutex> lock { this->mutex };
            while (keep_alive) {
              can_wake_up = false;
              if (next_wake_up_time) {
                // We're going to sleep forever until manually woken up or the time elapses
                sleep_cv.wait_until(lock, *next_wake_up_time, [this]() { return can_wake_up; });
              }
              else {
                // We're going to sleep forever until manually woken up
                sleep_cv.wait(lock, [this]() { return can_wake_up; });
              }

              if (next_wake_up_time) {
                // Timer has just been started, or we have waited for the required amount of time.
                // We can check which case it is by comparing time points.

                const auto now { std::chrono::steady_clock::now() };
                if (now < *next_wake_up_time) {
                  // Thread has been woken up manually to synchronize the time points.
                  // We do nothing and just go back to waiting with a new time point.
                }
                else {
                  next_wake_up_time = boost::none;

                  const auto result { !this->retry_function || this->retry_function() };
                  if (!result) {
                    next_wake_up_time = now + this->timeout_duration;
                  }
                }
              }
              else {
                // Timer has been stopped.
                // We do nothing and just go back to waiting until notified (unless we are killing the thread).
              }
            }
          } }
        } {
    }

    /**
     * @brief A destructor for the timer that gracefully shuts down the thread.
     */
    ~StateRetryTimer() {
      {
        std::lock_guard lock { mutex };
        keep_alive = false;
        next_wake_up_time = boost::none;
        wake_up_thread();
      }

      timer_thread.join();
    }

    /**
     * @brief Start or stop the timer thread.
     * @param retry_function Function to be executed every X seconds.
     *                       If the function returns true, the loop is stopped.
     *                       If the function is of type nullptr_t, the loop is stopped.
     * @warning This method does NOT acquire the mutex! It is intended to be used from places
     *          where the mutex has already been locked.
     */
    void
    setup_timer(std::function<bool()> retry_function) {
      this->retry_function = std::move(retry_function);

      if (this->retry_function) {
        next_wake_up_time = std::chrono::steady_clock::now() + timeout_duration;
      }
      else {
        if (!next_wake_up_time) {
          return;
        }

        next_wake_up_time = boost::none;
      }

      wake_up_thread();
    }

  private:
    /**
     * @brief Manually wake up the thread.
     */
    void
    wake_up_thread() {
      can_wake_up = true;
      sleep_cv.notify_one();
    }

    std::mutex &mutex; /**< A reference to a shared mutex. */
    std::chrono::seconds timeout_duration { 5 }; /**< A retry time for the timer. */
    std::function<bool()> retry_function; /**< Function to be executed until it succeeds. */

    std::thread timer_thread; /**< A timer thread. */
    std::condition_variable sleep_cv; /**< Condition variable for waking up thread. */

    bool can_wake_up { false }; /**< Safeguard for the condition variable to prevent sporadic thread wake ups. */
    bool keep_alive { true }; /**< A kill switch for the thread when it has been woken up. */
    boost::optional<std::chrono::steady_clock::time_point> next_wake_up_time; /**< Next time point for thread to wake up. */
  };

  session_t::deinit_t::~deinit_t() {
#ifdef _WIN32
    platf::vulkan_hdr_bridge::shutdown_cleanup();
#endif
    // 清理事件监听器
    SessionEventListener::deinit();
    
    // 兜底：退出时如果 VDD 仍存在且 vdd_keep_enabled=false，直接销毁
    // 使用 nolog 版本，因为析构时 boost::log 可能已被销毁
    if (!config::video.vdd_keep_enabled) {
      vdd_utils::destroy_vdd_monitor_nolog();
    }
  }

  session_t &
  session_t::get() {
    static session_t session;
    return session;
  }

  std::unique_ptr<session_t::deinit_t>
  session_t::init() {
#ifdef _WIN32
    // Remove a registration left behind by a terminated Sunshine process
    // before recovering display state or accepting a new stream.
    platf::vulkan_hdr_bridge::startup_cleanup();
#endif
    session_t::get().settings.set_filepath(platf::appdata() / "original_display_settings.json");
    
    // 初始化会话事件监听器（用于检测解锁事件）
    SessionEventListener::init();
    
    session_t::get().restore_state();
    return std::make_unique<deinit_t>();
  }

  void
  session_t::clear_vdd_state() {
    current_device_prep.reset();
    current_vdd_prep.reset();
    current_use_vdd.reset();
    pending_vdd_.reset();
    // 恢复原始的 output_name，避免下一个会话使用已销毁的 VDD 设备 ID
    if (!original_output_name.empty()) {
      config::video.output_name = original_output_name;
      original_output_name.clear();
      BOOST_LOG(debug) << "已恢复原始 output_name: " << config::video.output_name;
    }
  }

  void
  session_t::cancel_pending_display_retry() {
    pending_restore_ = false;
    SessionEventListener::clear_unlock_task();
    timer->setup_timer(nullptr);
  }

  void
  session_t::stop_timer_and_clear_vdd_state() {
    cancel_pending_display_retry();
    clear_vdd_state();
  }

  namespace {
    /**
     * @brief Get client identifier from session.
     * @details Prioritizes client certificate UUID (stored in env) over client_name as it is more stable.
     * @param session The launch session containing client information.
     * @return Client identifier string, or empty string if not available.
     */
    std::string
    get_client_id_from_session(const rtsp_stream::launch_session_t &session) {
      if (!session.client_cert_uuid.empty()) {
        return session.client_cert_uuid;
      }

      // 兼容尚未通过独立字段传递 client_cert_uuid、只写入启动环境的内部调用方。
      if (auto cert_uuid_it = session.env.find("SUNSHINE_CLIENT_CERT_UUID");
        cert_uuid_it != session.env.end()) {
        if (std::string cert_uuid = cert_uuid_it->to_string(); !cert_uuid.empty()) {
          return cert_uuid;
        }
      }

      if (!session.client_name.empty() && session.client_name != "unknown") {
        return session.client_name;
      }

      return {};
    }

    /**
     * @brief Wait for VDD device to be available (active or inactive).
     * @param device_zako Output parameter for the device ID.
     * @param max_attempts Maximum number of retry attempts.
     * @param initial_delay Initial delay between retries.
     * @param max_delay Maximum delay between retries.
     * @return true if device was found (active or inactive), false otherwise.
     */
    bool
    wait_for_vdd_device(std::string &device_zako, int max_attempts,
      std::chrono::milliseconds initial_delay,
      std::chrono::milliseconds max_delay) {
      return vdd_utils::retry_with_backoff(
        [&device_zako]() {
          device_zako = display_device::find_device_by_friendlyname(ZAKO_NAME);
          if (device_zako.empty()) {
            BOOST_LOG(debug) << "VDD device not found by friendly name";
            return false;
          }

          // Device found by friendly name - that's all we need
          // It can be activated later during display configuration
          BOOST_LOG(debug) << "VDD device found: " << device_zako;
          return true;
        },
        { .max_attempts = max_attempts,
          .initial_delay = initial_delay,
          .max_delay = max_delay,
          .context = "Waiting for VDD device availability" });
    }

    bool
    wait_for_vdd_device_departure(int max_attempts,
      std::chrono::milliseconds initial_delay,
      std::chrono::milliseconds max_delay) {
      return vdd_utils::retry_with_backoff(
        []() {
          return display_device::find_device_by_friendlyname(ZAKO_NAME).empty();
        },
        { .max_attempts = max_attempts,
          .initial_delay = initial_delay,
          .max_delay = max_delay,
          .context = "Waiting for VDD monitor departure" });
    }

    /**
     * @brief Attempt to recover VDD device with retries.
     * @param client_id Client identifier for the VDD monitor.
     * @param client_name Client name for getting physical size from config.
     * @param hdr_brightness hdr_brightness_t.
     * @param device_zako Output parameter for the device ID.
     * @return true if recovery succeeded, false otherwise.
     */
    bool
    try_recover_vdd_device(const std::string &client_id, const std::string &client_name, const vdd_utils::hdr_brightness_t &hdr_brightness, std::string &device_zako) {
      constexpr int max_retries = 3;
      const vdd_utils::physical_size_t physical_size = vdd_utils::get_client_physical_size(client_name);

      // 复用模式使用固定标识符，否则使用客户端ID
      const std::string vdd_identifier = config::video.vdd_reuse
        ? "shared_vdd"
        : client_id;

      for (int retry = 1; retry <= max_retries; ++retry) {
        BOOST_LOG(info) << "正在执行第" << retry << "次VDD恢复尝试...";

        if (!vdd_utils::create_vdd_monitor(vdd_identifier, hdr_brightness, physical_size)) {
          BOOST_LOG(error) << "创建虚拟显示器失败，尝试" << retry << "/" << max_retries;
          if (retry < max_retries) {
            std::this_thread::sleep_for(std::chrono::seconds(1 << retry));
          }
          continue;
        }

        if (wait_for_vdd_device(device_zako, 5, 233ms, 2000ms)) {
          BOOST_LOG(info) << "VDD设备恢复成功！";
          return true;
        }

        BOOST_LOG(error) << "VDD设备检测失败，正在第" << retry << "/" << max_retries << "次重试...";
        if (retry < max_retries) {
          std::this_thread::sleep_for(std::chrono::seconds(1 << retry));
        }
      }

      return false;
    }

    session_t::configure_result_t
    make_apply_configure_result(settings_t::apply_result_t apply_result) {
      using configure_result_e = session_t::configure_result_t::result_e;
      using apply_result_e = settings_t::apply_result_t::result_e;

      configure_result_e result { configure_result_e::success };
      std::string hint { "Try setting the display, resolution, refresh rate, HDR, and VDD options to Auto, then start the session again." };

      switch (apply_result.result) {
        case apply_result_e::success:
          result = configure_result_e::success;
          hint = {};
          break;
        case apply_result_e::topology_fail:
          result = configure_result_e::topology_fail;
          hint = "Check that the target display or virtual display is available, then set display/VDD options to Auto and try again.";
          break;
        case apply_result_e::primary_display_fail:
          result = configure_result_e::primary_display_fail;
          hint = "Set the target display to Auto or choose a connected display that Windows can make primary.";
          break;
        case apply_result_e::modes_fail:
          result = configure_result_e::modes_fail;
          hint = "Choose a resolution and refresh rate supported by the display, or set resolution and FPS to Auto.";
          break;
        case apply_result_e::hdr_states_fail:
          result = configure_result_e::hdr_states_fail;
          hint = "Disable HDR for this session or make sure the selected display supports the requested HDR mode.";
          break;
        case apply_result_e::color_profile_fail:
          result = configure_result_e::hdr_states_fail;
          hint = "Use the system color profile, or reinstall the selected ICC profile and try again.";
          break;
        case apply_result_e::file_save_fail:
          result = configure_result_e::file_save_fail;
          hint = "Check Sunshine's app data folder permissions and free disk space, then try again.";
          break;
        case apply_result_e::revert_fail:
          result = configure_result_e::revert_fail;
          hint = "Restore Windows display settings manually or reset Sunshine display persistence before retrying.";
          break;
      }

      return { result, apply_result.get_error_message(), hint };
    }

  }  // namespace

  session_t::vdd_stage_result_e
  session_t::apply_vdd_display_stage(const parsed_config_t &config,
    const boost::optional<device_info_map_t> &pre_vdd_devices) {
    if (config.vdd_prep != parsed_config_t::vdd_prep_e::no_operation &&
        !vdd_utils::apply_vdd_prep(config.device_id, config.vdd_prep, pre_vdd_devices)) {
      return vdd_stage_result_e::topology_failed;
    }

    // 仅在 VDD 预期已经激活后检查 Windows 是否已发布目标模式。
    // 锁屏场景会在解锁后执行本阶段；SYSTEM+RDP 不进入本阶段。
    if (config.resolution && config.refresh_rate) {
      const display_mode_t requested_mode {*config.resolution, *config.refresh_rate};
      if (!vdd_utils::wait_for_mode_publication(config.device_id, requested_mode)) {
        BOOST_LOG(error) << "VDD monitor did not publish "
                         << to_string(*config.resolution) << "@" << to_string(*config.refresh_rate);
        return vdd_stage_result_e::modes_failed;
      }
    }

    // 在 settings_t 应用目标 HDR 状态前先重置新准备的显示器。
    // 此处失败不作为最终错误，实际 HDR 操作及结果仍以 settings_t 为准。
    if (config.change_hdr_state && !vdd_utils::set_hdr_state(false)) {
      BOOST_LOG(debug) << "首次设置HDR状态失败，等待设备稳定后重试";
      std::this_thread::sleep_for(500ms);
      vdd_utils::set_hdr_state(false);
    }

    return vdd_stage_result_e::ready;
  }

  session_t::configure_result_t
  session_t::configure_display(const config::video_t &config,
    const rtsp_stream::launch_session_t &session,
    bool is_reconfigure) {
    std::lock_guard lock { mutex };

    // 恢复运行中的应用时可能换成另一个客户端。取消旧的延迟恢复任务，
    // 但在完成模式兼容性判断前保留当前 VDD；仅客户端身份变化不要求重建。
    if (!is_reconfigure) {
      if (const std::string new_client_id = get_client_id_from_session(session);
        !current_vdd_client_id.empty() && !new_client_id.empty() &&
        current_vdd_client_id != new_client_id) {
        BOOST_LOG(info) << "New client is resuming the app; preserving the current VDD while checking the requested mode";
        cancel_pending_display_retry();
      }
    }

    auto parsed_config = make_parsed_config(config, session, is_reconfigure);
    if (!parsed_config) {
      BOOST_LOG(error) << "Failed to parse configuration for the display device settings!";
      restore_state_impl(revert_reason_e::config_cleanup);
      return {
        configure_result_t::result_e::parse_fail,
        "Failed to parse display configuration.",
        "Set display, VDD, resolution, refresh rate, and HDR options to Auto or valid values, then try again."
      };
    }

    // Parsing is side-effect free. From this point on, parsed_config is the
    // single source of truth for whether this session needs VDD preparation.
    const bool rdp_session_active = display_device::w_utils::is_any_rdp_session_active();
    const bool is_rdp_blocking_vdd = !is_running_as_system_user && rdp_session_active;
    const bool use_vdd = parsed_config->use_vdd.value_or(false);
    const bool should_prepare_vdd = use_vdd && !is_rdp_blocking_vdd;
    const bool is_system_rdp_vdd_session = should_prepare_vdd && is_running_as_system_user && rdp_session_active;
    if (use_vdd && is_rdp_blocking_vdd) {
      BOOST_LOG(info) << "[Display] RDP环境：强制使用RDP虚拟显示器，跳过VDD准备";
    }

    if (should_prepare_vdd || config.capture == "vdd") {
      const auto vdd_status = vdd_utils::get_vdd_status();
      if (!vdd_status.is_usable()) {
        const bool missing = !vdd_status.installed;
        BOOST_LOG(error) << "VDD was requested, but the ZakoVDD adapter is " << (missing ? "not installed" : "not healthy");
        return {
          missing ? configure_result_t::result_e::vdd_not_installed : configure_result_t::result_e::vdd_unavailable,
          missing ? "The virtual display driver is not installed." : "The virtual display driver is installed but unavailable.",
          missing ?
            "Open Sunshine settings, install the virtual display driver, then try again." :
            "Open Sunshine settings, repair the virtual display driver or restart Windows if requested, then try again."
        };
      }
    }

    const bool vulkan_hdr_bridge_requested =
      should_prepare_vdd && !is_system_rdp_vdd_session &&
      display_prepared_for_hdr(config, session) && config.vdd_vulkan_hdr_bridge;
    const bool will_disable_physical_displays =
      should_prepare_vdd && !is_system_rdp_vdd_session &&
      parsed_config->vdd_prep == parsed_config_t::vdd_prep_e::display_off;

    // 在创建 VDD 可能使 Windows 自动激活显示器、Sunshine 应用目标布局之前保存拓扑。
    boost::optional<active_topology_t> pre_saved_initial_topology = pending_vdd_.initial_topology;
    if (should_prepare_vdd) {
      const bool vdd_already_exists = !display_device::find_device_by_friendlyname(ZAKO_NAME).empty();
      if (will_disable_physical_displays) {
        settings.capture_audio_sink();
      }

      if (pending_restore_ && settings.has_persistent_data()) {
        BOOST_LOG(info) << (vdd_already_exists ?
                              "有待恢复的设置且 VDD 仍存在，保留原有初始拓扑" :
                              "有待恢复的设置，保留原有初始拓扑");
        cancel_pending_display_retry();
      }
      else if (vdd_already_exists) {
        BOOST_LOG(debug) << "VDD already exists, skipping initial topology save (topology may be corrupted)";
      }
      else if (!is_system_rdp_vdd_session) {
        pending_vdd_.initial_topology = get_current_topology();
        pre_saved_initial_topology = pending_vdd_.initial_topology;
        BOOST_LOG(debug) << "Pre-saved initial topology before VDD creation: " << to_string(*pre_saved_initial_topology);
      }

      boost::optional<device_info_map_t> captured_pre_vdd_devices;
      const auto prepare_result = prepare_vdd(*parsed_config, session, captured_pre_vdd_devices);
      if (captured_pre_vdd_devices) {
        pending_vdd_.pre_vdd_devices = std::move(*captured_pre_vdd_devices);
      }

      if (prepare_result != vdd_stage_result_e::ready) {
        if (will_disable_physical_displays) {
          settings.release_audio_sink();
        }

        const bool modes_failed = prepare_result == vdd_stage_result_e::modes_failed;
        BOOST_LOG(error) << (modes_failed ?
                              "Failed to update the VDD mode list" :
                              "Failed to prepare the VDD device");
        restore_state_impl(revert_reason_e::config_cleanup);
        return {
          modes_failed ? configure_result_t::result_e::modes_fail : configure_result_t::result_e::vdd_create_failed,
          modes_failed ? "Sunshine could not configure the requested virtual display mode." : "Sunshine could not create the virtual display.",
          modes_failed ?
            "Choose a resolution and refresh rate supported by the virtual display, or set resolution and FPS to Auto." :
            "Repair the virtual display driver in Sunshine settings, then try again."
        };
      }

      // NVHTTP 的短间隔重试可能在 VDD 已创建后再次进入 configure_display。
      // 在 settings_t 持久化状态或会话完成清理前，始终保留第一次创建前的快照。
      pre_saved_initial_topology = pending_vdd_.initial_topology;
    }

    // 保存当前会话的配置模式（可能包含客户端的override）
    current_device_prep = parsed_config->device_prep;
    current_vdd_prep = parsed_config->vdd_prep;
    current_use_vdd = parsed_config->use_vdd;

    if (is_system_rdp_vdd_session) {
      // SYSTEM 服务可以在 RDP 会话中创建 VDD，但此时 Windows 无法提供
      // 可靠的交互式 CCD 拓扑。因此这里不修改拓扑和显示设置，最终由编码器探测
      // 确认 VDD 是否可捕获，避免在持有会话锁时等待一个不会改变处理结果的条件。
      BOOST_LOG(info) << "[Display] SYSTEM RDP session: VDD is ready; skipping topology and display-setting changes";
      pending_vdd_.reset();
      timer->setup_timer(nullptr);
      return {
        configure_result_t::result_e::success,
        {},
        {}
      };
    }

    const bool should_defer_display_settings = settings.is_changing_settings_going_to_fail();
    if (should_defer_display_settings) {
      timer->setup_timer([this, config_copy = *parsed_config, client_name = session.client_name,
                           client_cert_uuid = session.client_cert_uuid,
                           enable_hdr = session.enable_hdr,
                           hdr_target_source = session.hdr_target_source,
                           hdr_capabilities = session.hdr_capabilities,
                           pre_saved_initial_topology,
                           pre_vdd_devices = pending_vdd_.pre_vdd_devices,
                           should_prepare_vdd,
                           vulkan_hdr_bridge_requested]() {
        if (settings.is_changing_settings_going_to_fail()) {
          BOOST_LOG(warning) << "Applying display settings will fail - retrying later...";
          return false;
        }

        if (should_prepare_vdd) {
          const auto vdd_stage_result = apply_vdd_display_stage(config_copy, pre_vdd_devices);
          if (vdd_stage_result == vdd_stage_result_e::modes_failed) {
            BOOST_LOG(warning) << "The rebuilt VDD has not published the requested mode yet; retrying the deferred display configuration";
            return false;
          }
          if (vdd_stage_result == vdd_stage_result_e::topology_failed) {
            BOOST_LOG(warning) << "The deferred VDD topology change is still unavailable; retrying without tearing down the active monitor";
            return false;
          }
        }

        auto retry_session = rtsp_stream::launch_session_t {};
        retry_session.client_name = client_name;
        retry_session.client_cert_uuid = client_cert_uuid;
        retry_session.enable_hdr = enable_hdr;
        retry_session.hdr_target_source = hdr_target_source;
        if (!client_cert_uuid.empty()) {
          retry_session.env["SUNSHINE_CLIENT_CERT_UUID"] = client_cert_uuid;
        }
        retry_session.hdr_capabilities = hdr_capabilities;
        if (!settings.apply_config(config_copy, retry_session, pre_saved_initial_topology)) {
          BOOST_LOG(warning) << "Failed to apply display settings - will stop trying, but will allow stream to continue.";
          // WARNING! After call to the method below, this lambda function is no longer valid!
          // DO NOT access anything from the capture list!
          restore_state_impl(revert_reason_e::config_cleanup);
          return true;
        }
#ifdef _WIN32
        else if (vulkan_hdr_bridge_requested) {
          if (!platf::vulkan_hdr_bridge::enable_for_vdd_hdr_session()) {
            BOOST_LOG(warning) << "Vulkan HDR bridge validation or registration failed; continuing without the workaround";
          }
        }
        else {
          platf::vulkan_hdr_bridge::disable();
        }
#endif
        pending_vdd_.reset();
        return true;
      });

      BOOST_LOG(warning) << "It is already known that display settings cannot be changed. Allowing stream to start without changing the settings, but will retry changing settings later...";
      return {
        configure_result_t::result_e::deferred_retry,
        "Display settings cannot be changed yet; Sunshine will retry while the stream starts.",
        "Unlock the desktop, make sure Windows display settings are available, and Sunshine will retry automatically."
      };
    }

    // 延迟任务尚未执行时可能收到新的重配置。本次调用已经接管立即应用，
    // 因此必须取消旧回调，避免它随后使用过期配置再次修改显示状态。
    cancel_pending_display_retry();

    if (should_prepare_vdd) {
      const auto vdd_stage_result = apply_vdd_display_stage(*parsed_config, pending_vdd_.pre_vdd_devices);
      if (vdd_stage_result == vdd_stage_result_e::topology_failed) {
        restore_state_impl(revert_reason_e::config_cleanup);
        return {
          configure_result_t::result_e::topology_fail,
          "Sunshine could not apply the requested virtual display topology.",
          "Unlock the desktop, make sure Windows display settings are available, and try again."
        };
      }
      if (vdd_stage_result == vdd_stage_result_e::modes_failed) {
        BOOST_LOG(warning) << "VDD mode publication failed; cleaning up the prepared VDD state";
        restore_state_impl(revert_reason_e::config_cleanup);
        return {
          configure_result_t::result_e::modes_fail,
          "The virtual display did not publish the requested mode.",
          "Choose a resolution and refresh rate supported by the virtual display, or set resolution and FPS to Auto."
        };
      }
    }

    const auto apply_result = settings.apply_config(*parsed_config, session, pre_saved_initial_topology);
    if (apply_result) {
      timer->setup_timer(nullptr);
      pending_vdd_.reset();
#ifdef _WIN32
      if (vulkan_hdr_bridge_requested) {
        if (!platf::vulkan_hdr_bridge::enable_for_vdd_hdr_session()) {
          BOOST_LOG(warning) << "Vulkan HDR bridge validation or registration failed; continuing without the workaround";
        }
      }
      else {
        platf::vulkan_hdr_bridge::disable();
      }
#endif
      return make_apply_configure_result(apply_result);
    }

    auto configure_result = make_apply_configure_result(apply_result);
    if (apply_result.result == settings_t::apply_result_t::result_e::modes_fail ||
        apply_result.result == settings_t::apply_result_t::result_e::hdr_states_fail) {
      configure_result.cleanup_on_failure = true;
      BOOST_LOG(warning) << "Display mode/HDR configuration failed; deferring cleanup so startup can probe the current display state";
      return configure_result;
    }

    restore_state_impl(revert_reason_e::config_cleanup);
    return configure_result;
  }

  bool
  session_t::create_vdd_monitor(const std::string &client_name) {
    const vdd_utils::physical_size_t physical_size = vdd_utils::get_client_physical_size(client_name);
    // 复用模式使用固定标识符，否则使用客户端名称
    const std::string vdd_identifier = config::video.vdd_reuse
      ? "shared_vdd"
      : client_name;
    return vdd_utils::create_vdd_monitor(vdd_identifier, vdd_utils::hdr_brightness_t { 1000.0f, 0.001f, 1000.0f }, physical_size);
  }

  bool
  session_t::create_vdd_monitor_noninteractive() {
    return vdd_utils::create_vdd_monitor_noninteractive();
  }

  bool
  session_t::destroy_vdd_monitor() {
    current_vdd_client_id.clear();
    current_vdd_hdr_brightness.reset();
    return vdd_utils::destroy_vdd_monitor();
  }

  bool
  session_t::is_display_on() {
    return vdd_utils::is_display_on();
  }

  bool
  session_t::toggle_display_power() {
    return vdd_utils::toggle_display_power();
  }

  session_t::vdd_mode_update_e
  session_t::update_vdd_resolution(const parsed_config_t &config,
    const vdd_utils::VddSettings &vdd_settings,
    const std::string &vdd_device_id) {
    if (!config.resolution || !config.refresh_rate) {
      BOOST_LOG(debug) << "VDD session mode update skipped: resolution or refresh rate is not set";
      return vdd_mode_update_e::failed;
    }

    const auto new_setting = to_string(*config.resolution) + "@" + to_string(*config.refresh_rate);

    const auto setmodes_result = vdd_utils::set_vdd_session_mode(config, vdd_settings);
    switch (setmodes_result) {
      case vdd_utils::set_vdd_result::ok: {
        BOOST_LOG(info) << "VDD会话模式列表更新完成（未写入XML）: " << new_setting;
        const display_mode_t requested_mode {*config.resolution, *config.refresh_rate};
        const bool mode_advertised =
          !vdd_device_id.empty() && vdd_utils::is_mode_advertised(vdd_device_id, requested_mode);
        if (vdd_device_id.empty() || mode_advertised) {
          // A monitor created below will publish both monitor and target mode
          // lists from the new in-memory settings. An existing monitor can
          // switch live when Windows already exposes the requested mode.
          if (!vdd_device_id.empty()) {
            BOOST_LOG(debug) << "VDD monitor already advertises " << new_setting << "; live mode switch is safe";
          }
          return vdd_mode_update_e::ready;
        }

        // IddCxMonitorUpdateModes2 refreshes target modes, but a live console
        // monitor can retain its old monitor-description modes. Recreate only
        // this monitor so Windows republishes the complete intersection and
        // builds a correctly sized swapchain producer.
        BOOST_LOG(info) << "VDD monitor does not advertise " << new_setting
                        << "; recreating the monitor through IOCTL";
        return vdd_mode_update_e::recreate_monitor;
      }
      case vdd_utils::set_vdd_result::failed:
        BOOST_LOG(error) << "VDD SETMODES IOCTL failed for " << new_setting;
        return vdd_mode_update_e::failed;
      case vdd_utils::set_vdd_result::invalid_config:
        BOOST_LOG(error) << "VDD session mode is invalid: " << new_setting;
        return vdd_mode_update_e::failed;
      case vdd_utils::set_vdd_result::interface_missing:
        BOOST_LOG(error) << "VDD SETMODES IOCTL is unavailable; install a current ZakoVDD build";
        return vdd_mode_update_e::failed;
    }

    return vdd_mode_update_e::failed;
  }

  session_t::vdd_stage_result_e
  session_t::prepare_vdd(parsed_config_t &config, const rtsp_stream::launch_session_t &session, boost::optional<device_info_map_t> &pre_vdd_devices) {
    pre_vdd_devices.reset();

    const std::string current_client_id = get_client_id_from_session(session);
    const vdd_utils::hdr_brightness_t hdr_brightness {
      session.hdr_capabilities.max_nits,
      session.hdr_capabilities.min_nits,
      session.hdr_capabilities.max_full_frame_nits
    };
    BOOST_LOG(info) << "VDD HDR luminance source: " << hdr::to_string(session.hdr_target_source)
                    << " [max=" << hdr_brightness.max_nits
                    << ", min=" << hdr_brightness.min_nits
                    << ", max-full-frame=" << hdr_brightness.max_full_nits << "]";
    const vdd_utils::physical_size_t physical_size = vdd_utils::get_client_physical_size(session.client_name);

    if (config::video.capture == "vdd") {
      bool hardware_cursor_changed = false;
      if (vdd_utils::ensure_hardware_cursor_enabled_for_capture(&hardware_cursor_changed)) {
        if (hardware_cursor_changed) {
          BOOST_LOG(info) << "VDD hardware cursor export enabled; waiting for one-time driver reload";
          std::this_thread::sleep_for(1200ms);
        }
      }
      else {
        BOOST_LOG(warning) << "Failed to enable VDD hardware cursor export; cursor overlay and local synchronization may be unavailable";
      }
    }

    auto device_zako = display_device::find_device_by_friendlyname(ZAKO_NAME);

    std::string rebuild_old_vdd_id;
    const bool client_changed = !current_vdd_client_id.empty() &&
                                !current_client_id.empty() &&
                                current_vdd_client_id != current_client_id;
    const bool hdr_capabilities_changed = !current_vdd_hdr_brightness ||
                                          !current_vdd_hdr_brightness->nearly_equal(hdr_brightness);

    // An existing VDD must be rebuilt when its advertised HDR capabilities no
    // longer match this session. A shared VDD may otherwise be reused across
    // clients when the effective capabilities are identical.
    if (!device_zako.empty() && (client_changed || hdr_capabilities_changed)) {
      const bool reuse_vdd = config::video.vdd_reuse;

      if (reuse_vdd && !hdr_capabilities_changed) {
        BOOST_LOG(info) << "共享VDD模式，复用现有VDD（客户端: " << current_vdd_client_id << " -> " << current_client_id << "）";
        current_vdd_client_id = current_client_id;
      }
      else {
        BOOST_LOG(info) << "Rebuilding VDD to refresh session identity or HDR luminance capabilities"
                        << " (client: " << current_vdd_client_id << " -> " << current_client_id
                        << ", HDR capabilities changed: " << hdr_capabilities_changed << ")";
        const auto old_vdd_id = device_zako;
        destroy_vdd_monitor();
        device_zako.clear();
        rebuild_old_vdd_id = old_vdd_id;

        if (!config::video.vdd_keep_enabled) {
          BOOST_LOG(debug) << "从initial拓扑中移除VDD: " << old_vdd_id;
          settings.remove_vdd_from_initial_topology(old_vdd_id);
        }

        std::this_thread::sleep_for(500ms);
      }
    }

    std::string mode_rebuild_old_vdd_id;
    // Update VDD resolution configuration
    if (auto vdd_settings = vdd_utils::prepare_vdd_settings(config);
      config.resolution && config.refresh_rate) {
      const auto mode_update = update_vdd_resolution(config, vdd_settings, device_zako);
      if (mode_update == vdd_mode_update_e::failed) {
        return vdd_stage_result_e::modes_failed;
      }

      if (mode_update == vdd_mode_update_e::recreate_monitor) {
        mode_rebuild_old_vdd_id = device_zako;
        if (!vdd_utils::destroy_vdd_monitor()) {
          BOOST_LOG(error) << "Failed to destroy the VDD monitor for an IOCTL mode-list rebuild";
          return vdd_stage_result_e::create_failed;
        }
        if (!wait_for_vdd_device_departure(5, 100ms, 1000ms)) {
          // The driver has already removed its monitor object synchronously.
          // Continue with recreation so a delayed Windows device-interface
          // update cannot leave a headless host without a recovery attempt.
          BOOST_LOG(warning) << "VDD monitor departure is not visible yet; continuing the IOCTL rebuild";
        }
        device_zako.clear();
      }
    }

    // Create VDD device if not present
    if (device_zako.empty()) {
      // 在创建 VDD 之前捕获物理显示器快照
      // 此时无 VDD 存在（新建 or 重建后已销毁），物理屏应处于正常状态
      pre_vdd_devices = display_device::enum_available_devices();
      BOOST_LOG(info) << "已保存pre-VDD设备列表: " << display_device::to_string(*pre_vdd_devices);

      BOOST_LOG(info) << "创建虚拟显示器...";
      // 共享模式使用固定标识符；独立模式则使用当前客户端标识符创建显示器。
      const std::string vdd_identifier = config::video.vdd_reuse ? "shared_vdd" : current_client_id;
      if (!vdd_utils::create_vdd_monitor(vdd_identifier, hdr_brightness, physical_size)) {
        BOOST_LOG(error) << "VDD monitor creation command failed";
        return vdd_stage_result_e::create_failed;
      }
      std::this_thread::sleep_for(200ms);
    }

    // Wait for device to be ready
    if (!wait_for_vdd_device(device_zako, 5, 200ms, 1000ms)) {
      // 优先走 IOCTL DESTROY/CREATE 干净路径：driver 内部会调
      // IddCxMonitorDeparture，PnP 数据库里不会留 phantom monitor。
      // 历史上这里曾用 vdd_utils::disable_enable_vdd()，等同于 DevManView
      // /disable_enable 强拔 adapter，会留 CM_PROB_PHANTOM monitor，下一次
      // CREATEMONITOR 用新 GUID 会与 phantom 同 HardwareId 冲突，监视器永远attach 不上。
      BOOST_LOG(error) << "VDD设备初始化失败，尝试通过 IOCTL DESTROY+CREATE 恢复";
      vdd_utils::destroy_vdd_monitor();
      std::this_thread::sleep_for(500ms);

      if (!try_recover_vdd_device(current_client_id, session.client_name, hdr_brightness, device_zako)) {
        BOOST_LOG(error) << "VDD设备最终初始化失败";

        // last-resort：只有 IOCTL 通路彻底死掉才动 adapter，因为
        // disable_enable_vdd() 必然会留 phantom monitor（清理需要 SetupAPI
        // 工具函数，留作后续 PR）。
        if (!vdd_ioctl::ping()) {
          BOOST_LOG(warning) << "VDD IOCTL ping 失败，driver 可能已死；last-resort disable/enable adapter（注意会留 phantom monitor）";
          vdd_utils::disable_enable_vdd();
        }
        else {
          BOOST_LOG(warning) << "VDD IOCTL 仍可用，跳过 disable/enable，避免制造 phantom monitor";
        }
        return vdd_stage_result_e::create_failed;
      }
    }

    if (device_zako.empty()) {
      return vdd_stage_result_e::create_failed;
    }

    if (!mode_rebuild_old_vdd_id.empty() && mode_rebuild_old_vdd_id != device_zako) {
      BOOST_LOG(info) << "Replacing VDD ID after mode-list rebuild: "
                      << mode_rebuild_old_vdd_id << " -> " << device_zako;
      settings.replace_vdd_id(mode_rebuild_old_vdd_id, device_zako);
    }

    if (!rebuild_old_vdd_id.empty() && rebuild_old_vdd_id != device_zako) {
      BOOST_LOG(info) << "Replacing VDD ID after client or HDR capability rebuild: "
                      << rebuild_old_vdd_id << " -> " << device_zako;
      settings.replace_vdd_id(rebuild_old_vdd_id, device_zako);
    }

    if (original_output_name.empty()) {
      original_output_name = config::video.output_name;
      BOOST_LOG(debug) << "保存原始 output_name: " << original_output_name;
    }

    // Update configuration and state
    config.device_id = device_zako;
    config::video.output_name = device_zako;
    current_vdd_client_id = current_client_id;
    current_vdd_hdr_brightness = hdr_brightness;
    BOOST_LOG(info) << "成功配置VDD设备: " << device_zako;

    return vdd_stage_result_e::ready;
  }

  void
  session_t::restore_state() {
    std::lock_guard lock { mutex };
    restore_state_impl();
  }

  void
  session_t::reset_persistence() {
    std::lock_guard lock { mutex };
    settings.reset_persistence();
    stop_timer_and_clear_vdd_state();
    current_vdd_client_id.clear();
  }

  void
  session_t::restore_state_impl(revert_reason_e reason) {
#ifdef _WIN32
    // Stop exposing the implicit layer before changing HDR state or removing
    // the VDD. The layer itself is pass-through, but registration must remain
    // scoped to the stream that requested HDR on a Zako display.
    platf::vulkan_hdr_bridge::disable();
#endif
    // 统一的VDD清理逻辑（在恢复拓扑之前执行，不需要CCD API，锁屏时也可以执行）
    const auto vdd_id = display_device::find_device_by_friendlyname(ZAKO_NAME);

    // 常驻模式：只影响 VDD 是否销毁，不影响拓扑恢复
    const bool is_keep_enabled = config::video.vdd_keep_enabled;

    // 如果没有会话配置过（current_use_vdd 为 nullopt），说明：
    // 1. 程序刚启动进行崩溃恢复（init() 调用）
    // 2. 或者上一次会话已经正常结束且清理了状态
    // 此时不需要恢复拓扑（没有拓扑被修改过），只需要清理可能残留的 VDD
    if (!current_use_vdd.has_value()) {
      BOOST_LOG(debug) << " 无会话配置（current_use_vdd=nullopt），仅执行 VDD 清理";
      
      if (!vdd_id.empty() && !is_keep_enabled) {
        if (settings.has_persistent_data()) {
          BOOST_LOG(info) << "非常驻模式，销毁残留 VDD";
        }
        else {
          BOOST_LOG(info) << "检测到异常残留的 VDD（无 persistent_data），清理 VDD";
        }
        destroy_vdd_monitor();
        std::this_thread::sleep_for(1000ms);
      }

      // 无头主机自动创建检查
      if (reason == revert_reason_e::stream_ended && config::video.vdd_headless_create_enabled) {
        auto devices = display_device::enum_available_devices();
        if (devices.empty()) {
          BOOST_LOG(info) << "无头主机检测：未找到显示设备，自动创建基地显示器";
          create_vdd_monitor("");
          constexpr int max_attempts = 5;
          constexpr auto wait_time = std::chrono::milliseconds(233);
          for (int i = 0; i < max_attempts && !is_display_on(); ++i) {
            std::this_thread::sleep_for(wait_time);
          }
        }
      }

      stop_timer_and_clear_vdd_state();
      return;
    }

    // 以下逻辑仅在有会话配置时执行（current_use_vdd 有值）
    const bool is_vdd_mode = *current_use_vdd;

    // 获取当前有效的配置模式
    // VDD模式：从统一值映射到 vdd_prep
    // 普通模式：从统一值映射到 device_prep
    const auto display_prep = current_device_prep.value_or(
      static_cast<parsed_config_t::device_prep_e>(config::video.display_device_prep)
    );
    const auto vdd_prep = current_vdd_prep.value_or(
      parsed_config_t::to_vdd_prep(display_prep)
    );
    const auto device_prep = is_vdd_mode
      ? display_prep
      : parsed_config_t::to_physical_device_prep(display_prep);
    
    // 判断是否是无操作模式（会话配置了 no_operation，意味着拓扑从未被修改过）
    // VDD模式看 vdd_prep，普通模式看 device_prep
    const bool is_no_operation = is_vdd_mode 
      ? (vdd_prep == parsed_config_t::vdd_prep_e::no_operation)
      : (device_prep == parsed_config_t::device_prep_e::no_operation);

    BOOST_LOG(debug) << "restore_state_impl 决策参数:"
                     << " is_vdd_mode=" << is_vdd_mode
                     << " vdd_prep=" << static_cast<int>(vdd_prep)
                     << " device_prep=" << static_cast<int>(device_prep)
                     << " is_no_operation=" << is_no_operation;

    // 检查 apply_config 是否曾成功执行（persistent_data 是否存在）
    const bool has_persistent = settings.has_persistent_data();

    // 立即执行完整 restore
    // VDD 销毁逻辑
    if (!vdd_id.empty()) {
      bool should_destroy = false;
      
      // 判断1：常驻模式 - 保留VDD
      if (is_keep_enabled) {
        BOOST_LOG(debug) << "常驻模式，保留VDD";
      }
      // 判断2：非常驻模式 - 销毁VDD（无论是否是无操作模式）
      else if (has_persistent) {
        BOOST_LOG(info) << "非常驻模式，销毁VDD";
        should_destroy = true;
      }
      // 判断3：无persistent_data - apply_config 从未执行成功（如锁屏中退出串流）
      else {
        BOOST_LOG(info) << "apply_config 未执行（无persistent_data），销毁VDD并跳过拓扑恢复";
        should_destroy = true;
      }

      // 无头主机保护：如果销毁后会变成无头（VDD 是唯一显示设备），跳过销毁
      // 这避免了无意义的销毁+重建循环（device ID 变化导致 persistent_data 失效）
      if (should_destroy) {
        auto devices = display_device::enum_available_devices();
        bool only_vdd = (devices.size() == 1 && devices.count(vdd_id));
        if (only_vdd || devices.empty()) {
          BOOST_LOG(info) << "无头主机检测：VDD 是唯一显示设备，跳过销毁";
          should_destroy = false;
        }
      }

      if (should_destroy) {
        destroy_vdd_monitor();
        std::this_thread::sleep_for(1000ms);
      }
    }

    // 如果 apply_config 从未执行成功，拓扑从未被修改过，不需要恢复
    if (!has_persistent) {
      BOOST_LOG(info) << "apply_config 从未执行成功，跳过拓扑恢复";
      settings.release_audio_sink();
      stop_timer_and_clear_vdd_state();
      return;
    }

    // 添加诊断日志
    const bool settings_will_fail = settings.is_changing_settings_going_to_fail();
    BOOST_LOG(debug) << "Checking if reverting settings will fail: " << settings_will_fail;
    
    // VDD生命周期已在上面的逻辑中决定（销毁或保留），通知revert_settings不要再处理VDD销毁
    const bool vdd_already_handled = true;
    
    if (!settings_will_fail && settings.revert_settings(reason, vdd_already_handled)) {
      stop_timer_and_clear_vdd_state();
    }
    else {
      // 无法立即恢复，添加任务到解锁队列
      BOOST_LOG(warning) << "无法立即恢复显示设置";
      
      // 设置待恢复标志
      pending_restore_ = true;
      
      // 添加恢复任务（自动处理锁屏检查和立即执行）
      SessionEventListener::add_unlock_task([this, reason]() {
        // 快速检查是否还需要恢复（最小化锁持有时间）
        {
          std::lock_guard lock { mutex };
          if (!pending_restore_) {
            BOOST_LOG(info) << "恢复操作已取消，跳过";
            return;
          }
        }
        
        // 在锁外执行CCD检查和恢复操作（避免阻塞托盘等其他操作）
        if (settings.is_changing_settings_going_to_fail()) {
          BOOST_LOG(warning) << "CCD API仍不可用，启动轮询机制";
          std::lock_guard lock { mutex };
          this->start_polling_restore(reason);
          return;
        }
        
        // 执行恢复
        auto result = settings.revert_settings(reason, true);
        BOOST_LOG(info) << "恢复显示设置" << (result ? "成功" : "失败");
        
        // 恢复完成后清除标志和状态
        {
          std::lock_guard lock { mutex };
          pending_restore_ = false;
          stop_timer_and_clear_vdd_state();
        }
      });
    }
  }

  void
  session_t::start_polling_restore(revert_reason_e reason) {
    polling_retry_count_.store(0, boost::memory_order_relaxed);  // 重置计数器
    const int max_retries = 20;

    timer->setup_timer([this, reason, max_retries]() {
      // 检查是否还需要恢复
      if (!pending_restore_) {
        BOOST_LOG(debug) << "恢复操作已取消，跳过";
        return true;
      }
      
      if (settings.is_changing_settings_going_to_fail()) {
        const int current_count = polling_retry_count_.fetch_add(1, boost::memory_order_relaxed) + 1;
        if (current_count >= max_retries) {
          BOOST_LOG(warning) << "已达到最大重试次数，停止尝试恢复显示设置";
          pending_restore_ = false;
          clear_vdd_state();
          return true;
        }
        BOOST_LOG(warning) << "Timer: 仍在等待CCD恢复... (Count: " << current_count << "/" << max_retries << ")";
        return false;
      }

      // VDD生命周期已由restore_state_impl决定，跳过revert_settings中的VDD销毁
      auto result = settings.revert_settings(reason, true);
      BOOST_LOG(info) << "轮询恢复显示设置" << (result ? "成功" : "失败") << "，不再重试";
      pending_restore_ = false;
      clear_vdd_state();
      return true;
    });
  }

  session_t::session_t():
      timer { std::make_unique<StateRetryTimer>(mutex) } {
  }
}  // namespace display_device

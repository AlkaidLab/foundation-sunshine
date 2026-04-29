// standard includes
#include <boost/optional/optional_io.hpp>
#include <boost/process/v1.hpp>
#include <future>
#include <thread>

// local includes
#include "session.h"
#include "src/confighttp.h"
#include "src/globals.h"
#include "src/platform/common.h"
#include "src/platform/windows/display_device/session_listener.h"
#include "src/platform/windows/display_device/windows_utils.h"
#include "src/rtsp.h"
#include "to_string.h"
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
    session_t::get().settings.set_filepath(platf::appdata() / "original_display_settings.json");
    
    // 初始化会话事件监听器（用于检测解锁事件）
    SessionEventListener::init();
    
    session_t::get().restore_state();
    return std::make_unique<deinit_t>();
  }

  void
  session_t::clear_vdd_state() {
    last_vdd_setting.clear();
    current_device_prep.reset();
    current_vdd_prep.reset();
    current_use_vdd.reset();
    // 恢复原始的 output_name，避免下一个会话使用已销毁的 VDD 设备 ID
    if (!original_output_name.empty()) {
      config::video.output_name = original_output_name;
      original_output_name.clear();
      BOOST_LOG(debug) << "已恢复原始 output_name: " << config::video.output_name;
    }
  }

  void
  session_t::stop_timer_and_clear_vdd_state() {
    timer->setup_timer(nullptr);
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
  }  // namespace

  void
  session_t::configure_display(const config::video_t &config,
    const rtsp_stream::launch_session_t &session,
    bool is_reconfigure) {
    std::lock_guard lock { mutex };

    // Clean up VDD state if this is a new session with a different client
    if (!is_reconfigure) {
      if (const std::string new_client_id = get_client_id_from_session(session);
        !current_vdd_client_id.empty() && !new_client_id.empty() &&
        current_vdd_client_id != new_client_id) {
        BOOST_LOG(info) << "New session detected with different client ID, cleaning up VDD state";
        // Cancel any pending restore from the old session before it can interfere
        pending_restore_ = false;
        SessionEventListener::clear_unlock_task();
        stop_timer_and_clear_vdd_state();
      }
    }

    // 在 make_parsed_config 之前保存真实的初始拓扑
    // 因为 make_parsed_config 内部会调用 prepare_vdd，它会创建VDD并切换到扩展模式，导致原有显示器变成inactive
    boost::optional<active_topology_t> pre_saved_initial_topology;
    
    // 检查是否会使用VDD
    std::string device_id_to_use = config.output_name;
    if (auto it = session.env.find("SUNSHINE_CLIENT_DISPLAY_NAME"); it != session.env.end()) {
      const std::string client_display_name = it->to_string();
      if (!client_display_name.empty()) {
        device_id_to_use = client_display_name;
      }
    }
    
    // 检查VDD是否已存在
    const auto existing_vdd_id = display_device::find_device_by_friendlyname(ZAKO_NAME);
    const bool vdd_already_exists = !existing_vdd_id.empty();
    
    // 如果会使用VDD且VDD当前不存在，在创建前保存拓扑
    // 如果VDD已存在，说明拓扑已被破坏，不应该保存当前拓扑
    const auto requested_device_id = display_device::find_one_of_the_available_devices(device_id_to_use);
    const bool is_vdd_device = (display_device::get_display_friendly_name(device_id_to_use) == ZAKO_NAME);
    
    const bool needs_vdd = session.use_vdd || requested_device_id.empty() || is_vdd_device;
    
    // - 如果不需要 VDD：跳过 VDD 相关逻辑
    // - 如果不是 SYSTEM 权限且处于 RDP 中：使用 RDP 虚拟显示器，不创建 VDD
    // - 其他情况（包括 SYSTEM 权限）：准备 VDD 设备
    const bool is_rdp_blocking_vdd = !is_running_as_system_user && display_device::w_utils::is_any_rdp_session_active();
    const bool will_use_vdd = needs_vdd && !is_rdp_blocking_vdd;

    if (will_use_vdd && !vdd_already_exists) {

      // 如果有待恢复的设置，保留旧的初始拓扑，不要覆盖
      if (pending_restore_ && settings.has_persistent_data()) {
        BOOST_LOG(info) << "有待恢复的设置，保留原有初始拓扑";
        // 取消待恢复标志，因为新串流要开始了
        pending_restore_ = false;
        SessionEventListener::clear_unlock_task();
        timer->setup_timer(nullptr);
        // 不设置 pre_saved_initial_topology，让 apply_config 复用已有的
      }
      else {
        pre_saved_initial_topology = get_current_topology();
        BOOST_LOG(debug) << "Pre-saved initial topology before VDD creation: " << to_string(*pre_saved_initial_topology);
      }
    }
    else if (will_use_vdd && vdd_already_exists) {
      if (pending_restore_ && settings.has_persistent_data()) {
        // 有待恢复的设置且 VDD 仍存在（CCD 曾失败），保留原有初始拓扑
        BOOST_LOG(info) << "有待恢复的设置且 VDD 仍存在，保留原有初始拓扑";
        pending_restore_ = false;
        SessionEventListener::clear_unlock_task();
        timer->setup_timer(nullptr);
      }
      else {
        BOOST_LOG(debug) << "VDD already exists, skipping initial topology save (topology may be corrupted)";
      }
    }

    const auto parsed_config = make_parsed_config(config, session, is_reconfigure);
    if (!parsed_config) {
      BOOST_LOG(error) << "Failed to parse configuration for the display device settings!";
      return;
    }

    // 保存当前会话的配置模式（可能包含客户端的override）
    current_device_prep = parsed_config->device_prep;
    current_vdd_prep = parsed_config->vdd_prep;
    current_use_vdd = parsed_config->use_vdd;

    if (settings.is_changing_settings_going_to_fail()) {
      timer->setup_timer([this, config_copy = *parsed_config, &session, pre_saved_initial_topology]() {
        if (settings.is_changing_settings_going_to_fail()) {
          BOOST_LOG(warning) << "Applying display settings will fail - retrying later...";
          return false;
        }

        if (!settings.apply_config(config_copy, session, pre_saved_initial_topology)) {
          BOOST_LOG(warning) << "Failed to apply display settings - will stop trying, but will allow stream to continue.";
          // WARNING! After call to the method below, this lambda function is no longer valid!
          // DO NOT access anything from the capture list!
          restore_state_impl(revert_reason_e::config_cleanup);
        }
        return true;
      });

      BOOST_LOG(warning) << "It is already known that display settings cannot be changed. Allowing stream to start without changing the settings, but will retry changing settings later...";
      return;
    }

    if (settings.apply_config(*parsed_config, session, pre_saved_initial_topology)) {
      timer->setup_timer(nullptr);
    }
    else {
      restore_state_impl(revert_reason_e::config_cleanup);
    }
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
  session_t::destroy_vdd_monitor() {
    current_vdd_client_id.clear();
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

  void
  session_t::update_vdd_resolution(const parsed_config_t &config,
    const vdd_utils::VddSettings &vdd_settings) {
    const auto new_setting = to_string(*config.resolution) + "@" + to_string(*config.refresh_rate);

    if (last_vdd_setting == new_setting) {
      BOOST_LOG(debug) << "VDD配置未变更: " << new_setting;
      return;
    }

    if (!confighttp::saveVddSettings(vdd_settings.resolutions, vdd_settings.fps, config::video.adapter_name)) {
      BOOST_LOG(error) << "VDD配置保存失败 [resolutions: " << vdd_settings.resolutions
                       << " fps: " << vdd_settings.fps << "]";
      return;
    }

    last_vdd_setting = new_setting;
    BOOST_LOG(info) << "VDD配置更新完成: " << new_setting;

    BOOST_LOG(info) << "重新加载VDD驱动...";
    vdd_utils::reload_driver();
    std::this_thread::sleep_for(1200ms);
  }

  void
  session_t::prepare_vdd(parsed_config_t &config, const rtsp_stream::launch_session_t &session) {
    const std::string current_client_id = get_client_id_from_session(session);
    const vdd_utils::hdr_brightness_t hdr_brightness { session.max_nits, session.min_nits, session.max_full_nits };
    const vdd_utils::physical_size_t physical_size = vdd_utils::get_client_physical_size(session.client_name);

    auto device_zako = display_device::find_device_by_friendlyname(ZAKO_NAME);

    // pre_vdd_devices: 在 VDD 创建前一刻保存的物理显示器快照
    // 延迟到 VDD 创建前才捕获，确保无论是新建还是重建都能拿到正确状态
    device_info_map_t pre_vdd_devices;

    // Rebuild VDD device on client switch
    if (!device_zako.empty() && !current_vdd_client_id.empty() &&
        !current_client_id.empty() && current_vdd_client_id != current_client_id) {
      
      // 是否复用VDD（由独立配置项控制）
      const bool reuse_vdd = config::video.vdd_reuse;

      if (reuse_vdd) {
        // 复用VDD：所有客户端共享同一VDD，只更新客户端ID
        BOOST_LOG(info) << "共享VDD模式，复用现有VDD（客户端: " << current_vdd_client_id << " -> " << current_client_id << "）";
        current_vdd_client_id = current_client_id;
      }
      else {
        // 不复用：销毁并重建VDD（每个客户端独立VDD）
        BOOST_LOG(info) << "独立VDD模式，重建VDD设备（客户端: " << current_vdd_client_id << " -> " << current_client_id << "）";
        
        const auto old_vdd_id = device_zako;
        destroy_vdd_monitor();
        clear_vdd_state();
        device_zako.clear();
        
        // Handle VDD ID in persistent_data
        if (config::video.vdd_keep_enabled) {
          // 常驻模式：需要替换ID（保留VDD在persistent_data中）
          should_replace_vdd_id_ = true;
          old_vdd_id_ = old_vdd_id;
          BOOST_LOG(debug) << "标记需要替换VDD ID: " << old_vdd_id;
        }
        else {
          // 非常驻模式：从initial中移除VDD
          BOOST_LOG(debug) << "从initial拓扑中移除VDD: " << old_vdd_id;
          settings.remove_vdd_from_initial_topology(old_vdd_id);
        }
        
        std::this_thread::sleep_for(500ms);
      }
    }

    // Update VDD resolution configuration
    if (auto vdd_settings = vdd_utils::prepare_vdd_settings(config);
      vdd_settings.needs_update && config.resolution) {
      update_vdd_resolution(config, vdd_settings);
    }

    // Create VDD device if not present
    if (device_zako.empty()) {
      // 在创建 VDD 之前捕获物理显示器快照
      // 此时无 VDD 存在（新建 or 重建后已销毁），物理屏应处于正常状态
      pre_vdd_devices = display_device::enum_available_devices();
      BOOST_LOG(info) << "已保存pre-VDD设备列表: " << display_device::to_string(pre_vdd_devices);

      BOOST_LOG(info) << "创建虚拟显示器...";
      // 复用模式使用固定标识符，否则使用客户端ID生成唯一GUID
      const std::string vdd_identifier = config::video.vdd_reuse
        ? "shared_vdd"  // 固定标识符，所有客户端共用同一GUID
        : current_client_id;  // 为每个客户端生成不同GUID
      if (!vdd_utils::create_vdd_monitor(vdd_identifier, hdr_brightness, physical_size)) {
        BOOST_LOG(error) << "创建虚拟显示器失败（驱动报告错误），尝试恢复";
        if (!try_recover_vdd_device(current_client_id, session.client_name, hdr_brightness, device_zako)) {
          BOOST_LOG(error) << "VDD设备最终初始化失败";
          vdd_utils::disable_enable_vdd();
          return;
        }
      }
      else {
        std::this_thread::sleep_for(200ms);
      }
    }

    // Wait for device to be ready
    if (!wait_for_vdd_device(device_zako, 5, 200ms, 1000ms)) {
      BOOST_LOG(error) << "VDD设备初始化失败，尝试恢复";
      vdd_utils::disable_enable_vdd();
      std::this_thread::sleep_for(2s);

      if (!try_recover_vdd_device(current_client_id, session.client_name, hdr_brightness, device_zako)) {
        BOOST_LOG(error) << "VDD设备最终初始化失败";
        vdd_utils::disable_enable_vdd();
        return;
      }
    }

    if (device_zako.empty()) {
      return;
    }

    if (original_output_name.empty()) {
      original_output_name = config::video.output_name;
      BOOST_LOG(debug) << "保存原始 output_name: " << original_output_name;
    }

    // Replace VDD ID if needed (after client switch in keep_enabled mode)
    if (should_replace_vdd_id_ && !old_vdd_id_.empty()) {
      BOOST_LOG(info) << "替换persistent_data中的VDD ID: " << old_vdd_id_ << " -> " << device_zako;
      settings.replace_vdd_id(old_vdd_id_, device_zako);
      should_replace_vdd_id_ = false;
      old_vdd_id_.clear();
    }
    
    // Update configuration and state
    config.device_id = device_zako;
    config::video.output_name = device_zako;
    current_vdd_client_id = current_client_id;
    BOOST_LOG(info) << "成功配置VDD设备: " << device_zako;

    // Apply VDD prep settings to handle display topology
    // This determines how VDD interacts with physical displays
    // VDD模式下的拓扑控制与普通模式分开处理
    if (config.vdd_prep != parsed_config_t::vdd_prep_e::no_operation) {
      // User has specified a display configuration, apply it
      if (vdd_utils::apply_vdd_prep(device_zako, config.vdd_prep, pre_vdd_devices)) {
        BOOST_LOG(info) << "已应用VDD屏幕布局设置";
        std::this_thread::sleep_for(200ms);
      }
    }
    else {
      // No specific configuration, ensure VDD is in extended mode (default behavior)
      if (vdd_utils::ensure_vdd_extended_mode(device_zako)) {
        BOOST_LOG(info) << "已将VDD切换到扩展模式";
        std::this_thread::sleep_for(500ms);
      }
    }

    // Set HDR state with retry
    if (!vdd_utils::set_hdr_state(false)) {
      BOOST_LOG(debug) << "首次设置HDR状态失败，等待设备稳定后重试";
      std::this_thread::sleep_for(500ms);
      vdd_utils::set_hdr_state(false);
    }
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
    pending_restore_ = false;
    SessionEventListener::clear_unlock_task();
    stop_timer_and_clear_vdd_state();
    current_vdd_client_id.clear();
  }

  void
  session_t::restore_state_impl(revert_reason_e reason) {
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
          if (!create_vdd_monitor("")) {
            BOOST_LOG(error) << "无头主机自动创建显示器失败";
          }
          else {
            constexpr int max_attempts = 5;
            constexpr auto wait_time = std::chrono::milliseconds(233);
            for (int i = 0; i < max_attempts && !is_display_on(); ++i) {
              std::this_thread::sleep_for(wait_time);
            }
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

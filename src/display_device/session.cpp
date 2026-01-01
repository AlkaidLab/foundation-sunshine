// standard includes
#include <boost/optional/optional_io.hpp>
#include <boost/process.hpp>
#include <future>
#include <thread>

// local includes
#include "session.h"
#include "src/confighttp.h"
#include "src/globals.h"
#include "src/platform/common.h"
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
    // 不在析构函数中调用 restore_state()
    // 原因：析构函数在程序退出时被调用，此时 boost::log、timer 等资源可能已被销毁
    // 解决方案：在 main.cpp 的信号处理程序中显式调用 restore_state()
  }

  session_t &
  session_t::get() {
    static session_t session;
    return session;
  }

  std::unique_ptr<session_t::deinit_t>
  session_t::init() {
    session_t::get().settings.set_filepath(platf::appdata() / "original_display_settings.json");
    session_t::get().restore_state();
    return std::make_unique<deinit_t>();
  }

  void
  session_t::clear_vdd_state() {
    current_vdd_client_id.clear();
    last_vdd_setting.clear();
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

      for (int retry = 1; retry <= max_retries; ++retry) {
        BOOST_LOG(info) << "正在执行第" << retry << "次VDD恢复尝试...";

        if (!vdd_utils::create_vdd_monitor(client_id, hdr_brightness, physical_size)) {
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
        stop_timer_and_clear_vdd_state();
      }
    }

    const auto parsed_config = make_parsed_config(config, session, is_reconfigure);
    if (!parsed_config) {
      BOOST_LOG(error) << "Failed to parse configuration for the display device settings!";
      return;
    }

    if (settings.is_changing_settings_going_to_fail()) {
      timer->setup_timer([this, config_copy = *parsed_config, &session]() {
        if (settings.is_changing_settings_going_to_fail()) {
          BOOST_LOG(warning) << "Applying display settings will fail - retrying later...";
          return false;
        }

        if (!settings.apply_config(config_copy, session)) {
          BOOST_LOG(warning) << "Failed to apply display settings - will stop trying, but will allow stream to continue.";
          // WARNING! After call to the method below, this lambda function is no longer valid!
          // DO NOT access anything from the capture list!
          restore_state_impl(revert_reason_e::config_cleanup);
        }
        return true;
      });

      BOOST_LOG(warning) << "It is already known that display settings cannot be changed. "
                         << "Allowing stream to start without changing the settings, "
                         << "but will retry changing settings later...";
      return;
    }

    if (settings.apply_config(*parsed_config, session)) {
      timer->setup_timer(nullptr);
    }
    else {
      restore_state_impl(revert_reason_e::config_cleanup);
    }
  }

  bool
  session_t::create_vdd_monitor(const std::string &client_name) {
    const vdd_utils::physical_size_t physical_size = vdd_utils::get_client_physical_size(client_name);
    return vdd_utils::create_vdd_monitor(client_name, vdd_utils::hdr_brightness_t { 1000.0f, 0.001f, 1000.0f }, physical_size);
  }

  bool
  session_t::destroy_vdd_monitor() {
    return vdd_utils::destroy_vdd_monitor();
  }

  bool
  session_t::is_display_on() {
    return vdd_utils::is_display_on();
  }

  void
  session_t::toggle_display_power() {
    vdd_utils::toggle_display_power();
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
    std::this_thread::sleep_for(1500ms);
  }

  void
  session_t::prepare_vdd(parsed_config_t &config, const rtsp_stream::launch_session_t &session) {
    const std::string current_client_id = get_client_id_from_session(session);
    const vdd_utils::hdr_brightness_t hdr_brightness { session.max_nits, session.min_nits, session.max_full_nits };
    const vdd_utils::physical_size_t physical_size = vdd_utils::get_client_physical_size(session.client_name);

    auto device_zako = display_device::find_device_by_friendlyname(ZAKO_NAME);

    // Rebuild VDD device on client switch
    if (!device_zako.empty() && !current_vdd_client_id.empty() &&
        !current_client_id.empty() && current_vdd_client_id != current_client_id) {
      BOOST_LOG(info) << "客户端切换，重建VDD设备";
      vdd_utils::destroy_vdd_monitor();
      clear_vdd_state();
      device_zako.clear();
      std::this_thread::sleep_for(500ms);
    }

    // Update VDD resolution configuration
    if (auto vdd_settings = vdd_utils::prepare_vdd_settings(config);
      vdd_settings.needs_update && config.resolution) {
      update_vdd_resolution(config, vdd_settings);
    }

    // Create VDD device if not present
    if (device_zako.empty()) {
      BOOST_LOG(info) << "创建虚拟显示器...";
      vdd_utils::create_vdd_monitor(current_client_id, hdr_brightness, physical_size);
      std::this_thread::sleep_for(500ms);
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

    // Update configuration and state
    config.device_id = device_zako;
    config::video.output_name = device_zako;
    current_vdd_client_id = current_client_id;
    BOOST_LOG(info) << "成功配置VDD设备: " << device_zako;

    // Ensure VDD is in extended mode
    if (vdd_utils::ensure_vdd_extended_mode(device_zako)) {
      BOOST_LOG(info) << "已将VDD切换到扩展模式";
      std::this_thread::sleep_for(500ms);
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
    stop_timer_and_clear_vdd_state();
  }

  void
  session_t::restore_state_impl(revert_reason_e reason) {
    // 检测RDP会话
    if (w_utils::is_any_rdp_session_active()) {
      BOOST_LOG(info) << "Detected RDP remote session, disabling display settings recovery";
      stop_timer_and_clear_vdd_state();
      return;
    }

    // 在串流结束时处理VDD销毁（即使锁屏也可以执行清理工作）
    // 注意：创建VDD需要在 revert_settings 中处理，因为需要用户会话解锁才能被DXGI捕获
    if (reason == revert_reason_e::stream_ended) {
      auto devices = display_device::enum_available_devices();
      if (devices.size() > 1 && is_display_on()) {
        // 销毁VDD：可以在锁屏状态下执行（清理工作）
        BOOST_LOG(info) << "检测到多个显示器，关闭VDD" << (settings.is_changing_settings_going_to_fail() ? "（锁屏状态）" : "");
        destroy_vdd_monitor();
      }
    }

    if (!settings.is_changing_settings_going_to_fail() && settings.revert_settings(reason)) {
      stop_timer_and_clear_vdd_state();
    }
    else {
      if (settings.is_changing_settings_going_to_fail()) {
        BOOST_LOG(warning) << "Try reverting display settings will fail - retrying later...";
      }

      // 限制重试次数，避免无限循环
      static int retry_count = 0;
      const int max_retries = 20;

      timer->setup_timer([this, reason]() {
        if (settings.is_changing_settings_going_to_fail()) {
          retry_count++;
          if (retry_count >= max_retries) {
            BOOST_LOG(warning) << "已达到最大重试次数，停止尝试恢复显示设置";
            clear_vdd_state();
            return true;  // 返回true停止重试
          }
          BOOST_LOG(warning) << "Timer: Reverting display settings will still fail - retrying later... (Count: " << retry_count << "/" << max_retries << ")";
          return false;
        }

        // 只恢复一次
        auto result = settings.revert_settings(reason);
        BOOST_LOG(info) << "尝试恢复显示设置" << (result ? "成功" : "失败") << "，不再重试";
        clear_vdd_state();
        return true;
      });
    }
  }

  session_t::session_t():
      timer { std::make_unique<StateRetryTimer>(mutex) } {
  }
}  // namespace display_device

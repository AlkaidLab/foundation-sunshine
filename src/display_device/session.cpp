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
    // Stop vdd timer before destruction
    // session_t::get().vdd_timer->setup_timer(nullptr);
    session_t::get().restore_state();
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
  }

  void
  session_t::stop_timer_and_clear_vdd_state() {
    timer->setup_timer(nullptr);
    clear_vdd_state();
  }

  namespace {
    // Get client identifier from session
    // Prioritize client certificate UUID (stored in env) over client_name as it is more stable
    std::string
    get_client_id_from_session(const rtsp_stream::launch_session_t &session) {
      auto cert_uuid_it = session.env.find("SUNSHINE_CLIENT_CERT_UUID");
      if (cert_uuid_it != session.env.end()) {
        std::string cert_uuid = cert_uuid_it->to_string();
        if (!cert_uuid.empty()) {
          return cert_uuid;
        }
      }

      if (!session.client_name.empty() && session.client_name != "unknown") {
        return session.client_name;
      }

      return "";
    }

    // 等待VDD设备初始化
    bool
    wait_for_vdd_device(std::string &device_zako, int max_attempts, std::chrono::milliseconds initial_delay, std::chrono::milliseconds max_delay) {
      return vdd_utils::retry_with_backoff(
        [&device_zako]() {
          device_zako = display_device::find_device_by_friendlyname(ZAKO_NAME);
          return !device_zako.empty();
        },
        { .max_attempts = max_attempts,
          .initial_delay = initial_delay,
          .max_delay = max_delay,
          .context = "等待VDD设备初始化" });
    }

    // 尝试恢复VDD设备
    bool
    try_recover_vdd_device(const std::string &client_id, std::string &device_zako) {
      for (int retry = 1; retry <= 3; ++retry) {
        BOOST_LOG(info) << "正在执行第" << retry << "次VDD恢复尝试...";

        if (!vdd_utils::create_vdd_monitor(client_id)) {
          BOOST_LOG(error) << "创建虚拟显示器失败，尝试" << retry << "/3";
          if (retry < 3) {
            std::this_thread::sleep_for(std::chrono::seconds(1 << retry));
            continue;
          }
          break;
        }

        if (wait_for_vdd_device(device_zako, 5, 233ms, 2000ms)) {
          BOOST_LOG(info) << "VDD设备恢复成功！";
          return true;
        }

        BOOST_LOG(error) << "VDD设备检测失败，正在第" << retry << "/3次重试...";
        if (retry < 3) {
          std::this_thread::sleep_for(std::chrono::seconds(1 << retry));
        }
      }
      return false;
    }
  }  // namespace

  void
  session_t::configure_display(const config::video_t &config, const rtsp_stream::launch_session_t &session, bool is_reconfigure) {
    std::lock_guard lock { mutex };

    // 如果是新会话（非重新配置），检查是否需要清理VDD状态
    if (!is_reconfigure) {
      const std::string new_client_id = get_client_id_from_session(session);
      if (!current_vdd_client_id.empty() && !new_client_id.empty() && current_vdd_client_id != new_client_id) {
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
          restore_state_impl();
        }
        return true;
      });

      BOOST_LOG(warning) << "It is already known that display settings cannot be changed. Allowing stream to start without changing the settings, but will retry changing settings later...";
      return;
    }

    if (settings.apply_config(*parsed_config, session)) {
      timer->setup_timer(nullptr);
    }
    else {
      restore_state_impl();
    }
  }

  bool
  session_t::create_vdd_monitor(const std::string &client_name) {
    return vdd_utils::create_vdd_monitor(client_name);
  }

  bool
  session_t::destroy_vdd_monitor() {
    return vdd_utils::destroy_vdd_monitor();
  }

  void
  session_t::enable_vdd() {
    vdd_utils::enable_vdd();
  }

  void
  session_t::disable_vdd() {
    vdd_utils::disable_vdd();
  }

  void
  session_t::disable_enable_vdd() {
    vdd_utils::disable_enable_vdd();
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
  session_t::update_vdd_resolution(const parsed_config_t &config, const vdd_utils::VddSettings &vdd_settings) {
    const auto new_setting = to_string(*config.resolution) + "@" + to_string(*config.refresh_rate);

    if (last_vdd_setting == new_setting) {
      BOOST_LOG(debug) << "VDD配置未变更: " << new_setting;
      return;
    }

    if (!confighttp::saveVddSettings(vdd_settings.resolutions, vdd_settings.fps, config::video.adapter_name)) {
      BOOST_LOG(error) << "VDD配置保存失败 [resolutions: " << vdd_settings.resolutions << " fps: " << vdd_settings.fps << "]";
      return;
    }

    last_vdd_setting = new_setting;
    BOOST_LOG(info) << "VDD配置更新完成: " << new_setting;

    // 配置变更后执行驱动重载
    BOOST_LOG(info) << "重新加载VDD驱动...";
    vdd_utils::reload_driver();
    std::this_thread::sleep_for(1500ms);
  }

  void
  session_t::prepare_vdd(parsed_config_t &config, const rtsp_stream::launch_session_t &session) {
    const std::string current_client_id = get_client_id_from_session(session);
    auto device_zako = display_device::find_device_by_friendlyname(ZAKO_NAME);

    // 客户端切换时重建VDD设备
    if (!device_zako.empty() && !current_vdd_client_id.empty() &&
        !current_client_id.empty() && current_vdd_client_id != current_client_id) {
      BOOST_LOG(info) << "客户端切换，重建VDD设备";
      destroy_vdd_monitor();
      clear_vdd_state();
      device_zako.clear();
      std::this_thread::sleep_for(500ms);
    }

    // 更新VDD分辨率配置
    if (auto vdd_settings = vdd_utils::prepare_vdd_settings(config);
        vdd_settings.needs_update && config.resolution) {
      update_vdd_resolution(config, vdd_settings);
    }

    // 创建VDD设备
    if (device_zako.empty()) {
      BOOST_LOG(info) << "创建虚拟显示器...";
      create_vdd_monitor(current_client_id);
      std::this_thread::sleep_for(233ms);
    }

    // 等待设备就绪
    if (!wait_for_vdd_device(device_zako, 10, 100ms, 500ms)) {
      BOOST_LOG(error) << "VDD设备初始化失败，尝试恢复";
      disable_enable_vdd();
      std::this_thread::sleep_for(2s);

      if (!try_recover_vdd_device(current_client_id, device_zako)) {
        BOOST_LOG(error) << "VDD设备最终初始化失败";
        disable_enable_vdd();
        return;
      }
    }

    if (device_zako.empty()) {
      return;
    }

    // 更新配置和状态
    config.device_id = device_zako;
    config::video.output_name = device_zako;
    current_vdd_client_id = current_client_id;
    BOOST_LOG(info) << "成功配置VDD设备: " << device_zako;

    if (vdd_utils::ensure_vdd_extended_mode(device_zako)) {
      BOOST_LOG(info) << "已将VDD切换到扩展模式";
    }
    vdd_utils::set_hdr_state(false);
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
  session_t::restore_state_impl() {
    // 检测RDP会话
    if (w_utils::is_any_rdp_session_active()) {
      BOOST_LOG(info) << "Detected RDP remote session, disabling display settings recovery";
      stop_timer_and_clear_vdd_state();
      return;
    }

    if (!settings.is_changing_settings_going_to_fail() && settings.revert_settings()) {
      stop_timer_and_clear_vdd_state();
    }
    else {
      if (settings.is_changing_settings_going_to_fail()) {
        BOOST_LOG(warning) << "Try reverting display settings will fail - retrying later...";
      }

      // 限制重试次数，避免无限循环
      static int retry_count = 0;
      const int max_retries = 20;

      timer->setup_timer([this]() {
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
        auto result = settings.revert_settings();
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

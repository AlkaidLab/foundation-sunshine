// local includes
#include "session_listener.h"
#include "windows_utils.h"
#include "src/logging.h"

#include <atomic>

namespace display_device {

  // Static member initialization
  std::mutex SessionEventListener::mutex_;
  SessionEventListener::UnlockCallback SessionEventListener::pending_task_;
  std::thread SessionEventListener::worker_thread_;
  std::queue<SessionEventListener::UnlockCallback> SessionEventListener::task_queue_;
  std::condition_variable SessionEventListener::cv_;
  bool SessionEventListener::worker_running_ = false;
  HWND SessionEventListener::hidden_window_ = nullptr;
  std::thread SessionEventListener::message_thread_;
  std::atomic<bool> SessionEventListener::thread_running_ { false };
  std::atomic<bool> SessionEventListener::initialized_ { false };
  std::atomic<bool> SessionEventListener::event_based_ { false };

  namespace {
    const wchar_t *WINDOW_CLASS_NAME = L"SunshineSessionListener";
    std::condition_variable init_cv_;
    std::mutex init_mutex_;
    bool init_complete_ = false;
    bool init_success_ = false;
  }

  LRESULT CALLBACK
  SessionEventListener::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_WTSSESSION_CHANGE) {
      switch (wparam) {
        case WTS_SESSION_UNLOCK:
          {
            BOOST_LOG(info) << "[SessionListener] 检测到会话解锁事件";
            
            // 将pending_task_移入队列执行
            {
              std::lock_guard<std::mutex> lock(mutex_);
              if (pending_task_) {
                BOOST_LOG(info) << "[SessionListener] 执行解锁任务";
                task_queue_.push(std::move(pending_task_));
                pending_task_ = nullptr;
                cv_.notify_one();
              }
            }
          }
          break;
        case WTS_SESSION_LOCK:
          BOOST_LOG(info) << "[SessionListener] 检测到会话锁定事件";
          break;
        case WTS_CONSOLE_DISCONNECT:
          BOOST_LOG(info) << "[SessionListener] 检测到控制台断开事件";
          break;
        default:
          break;
      }
    }
    else if (message == WM_DESTROY) {
      PostQuitMessage(0);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }

  void
  SessionEventListener::message_loop() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = WINDOW_CLASS_NAME;

    if (!RegisterClassExW(&wc)) {
      DWORD last_error = GetLastError();
      if (last_error != ERROR_CLASS_ALREADY_EXISTS) {
        BOOST_LOG(error) << "[SessionListener] 注册窗口类失败: " << last_error;
        {
          std::lock_guard<std::mutex> lock(init_mutex_);
          init_complete_ = true;
          init_success_ = false;
        }
        init_cv_.notify_one();
        return;
      }
    }

    hidden_window_ = CreateWindowExW(
      0, WINDOW_CLASS_NAME, L"SunshineSessionListenerWindow",
      0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandle(nullptr), nullptr
    );

    if (!hidden_window_) {
      BOOST_LOG(error) << "[SessionListener] 创建隐藏窗口失败: " << GetLastError();
      {
        std::lock_guard<std::mutex> lock(init_mutex_);
        init_complete_ = true;
        init_success_ = false;
      }
      init_cv_.notify_one();
      return;
    }

    if (!WTSRegisterSessionNotification(hidden_window_, NOTIFY_FOR_THIS_SESSION)) {
      BOOST_LOG(warning) << "[SessionListener] 注册会话通知失败: " << GetLastError();
      DestroyWindow(hidden_window_);
      hidden_window_ = nullptr;
      {
        std::lock_guard<std::mutex> lock(init_mutex_);
        init_complete_ = true;
        init_success_ = false;
      }
      init_cv_.notify_one();
      return;
    }

    BOOST_LOG(info) << "[SessionListener] 会话事件监听器初始化成功";
    
    {
      std::lock_guard<std::mutex> lock(init_mutex_);
      init_complete_ = true;
      init_success_ = true;
    }
    init_cv_.notify_one();

    MSG msg;
    while (thread_running_ && GetMessage(&msg, nullptr, 0, 0)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }

    if (hidden_window_) {
      WTSUnRegisterSessionNotification(hidden_window_);
      DestroyWindow(hidden_window_);
      hidden_window_ = nullptr;
    }
    UnregisterClassW(WINDOW_CLASS_NAME, GetModuleHandle(nullptr));
  }

  void
  SessionEventListener::worker_loop() {
    BOOST_LOG(info) << "[SessionListener] Worker线程已启动";
    
    while (true) {
      UnlockCallback task;
      
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [] { 
          return !task_queue_.empty() || !worker_running_; 
        });
        
        if (!worker_running_ && task_queue_.empty()) {
          break;
        }
        
        if (task_queue_.empty()) {
          continue;
        }
        
        task = std::move(task_queue_.front());
        task_queue_.pop();
      }
      
      // 在锁外执行任务，避免死锁
      try {
        task();
      }
      catch (const std::exception& e) {
        BOOST_LOG(error) << "[SessionListener] 任务执行异常: " << e.what();
      }
    }
    
    BOOST_LOG(info) << "[SessionListener] Worker线程已退出";
  }

  bool
  SessionEventListener::init() {
    if (initialized_) {
      return event_based_;
    }

    {
      std::lock_guard<std::mutex> lock(init_mutex_);
      init_complete_ = false;
      init_success_ = false;
    }

    // 启动worker线程
    {
      std::lock_guard<std::mutex> lock(mutex_);
      worker_running_ = true;
    }
    worker_thread_ = std::thread(worker_loop);

    // 启动消息线程
    thread_running_ = true;
    message_thread_ = std::thread(message_loop);

    // 等待初始化完成
    {
      std::unique_lock<std::mutex> lock(init_mutex_);
      init_cv_.wait(lock, [] { return init_complete_; });
    }

    initialized_ = true;
    event_based_ = init_success_;

    if (!event_based_) {
      BOOST_LOG(warning) << "[SessionListener] 事件监听器初始化失败";
      thread_running_ = false;
      if (message_thread_.joinable()) {
        message_thread_.join();
      }
    }

    return event_based_;
  }

  void
  SessionEventListener::deinit() {
    if (!initialized_) {
      return;
    }

    BOOST_LOG(info) << "[SessionListener] 开始清理";

    thread_running_ = false;
    if (hidden_window_) {
      PostMessage(hidden_window_, WM_QUIT, 0, 0);
    }
    if (message_thread_.joinable()) {
      message_thread_.join();
    }

    // 停止worker线程
    {
      std::lock_guard<std::mutex> lock(mutex_);
      worker_running_ = false;
      cv_.notify_one();
    }

    if (worker_thread_.joinable()) {
      worker_thread_.join();
    }

    // 清理状态
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_task_ = nullptr;
      while (!task_queue_.empty()) {
        task_queue_.pop();
      }
    }

    initialized_ = false;
    event_based_ = false;
    BOOST_LOG(info) << "[SessionListener] 清理完成";
  }

  bool
  SessionEventListener::is_event_based() {
    return event_based_;
  }

  void
  SessionEventListener::add_unlock_task(UnlockCallback task) {
    if (!task) {
      return;
    }
    
    const bool is_locked = w_utils::is_user_session_locked();
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_locked) {
      // 未锁定：直接提交到队列执行
      BOOST_LOG(info) << "[SessionListener] 当前未锁定，立即执行任务";
      task_queue_.push(std::move(task));
      cv_.notify_one();
    }
    else {
      // 锁定中：保存任务等待解锁
      BOOST_LOG(info) << "[SessionListener] 任务已加入解锁队列";
      pending_task_ = std::move(task);
    }
  }

  void
  SessionEventListener::clear_unlock_task() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_task_ = nullptr;
  }

}  // namespace display_device

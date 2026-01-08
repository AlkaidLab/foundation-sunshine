#pragma once

// Windows includes must come first
#include <windows.h>

// standard includes
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

// lib includes
#include <wtsapi32.h>

namespace display_device {

  /**
   * @brief Listens for Windows session events (lock/unlock).
   * Uses WTSRegisterSessionNotification for event-based detection.
   */
  class SessionEventListener {
  public:
    using UnlockCallback = std::function<void()>;

    /**
     * @brief Initialize the session event listener.
     * @returns True if event-based listening was successfully registered.
     */
    static bool
    init();

    /**
     * @brief Cleanup and unregister the session event listener.
     */
    static void
    deinit();

    /**
     * @brief Check if event-based listening is active.
     */
    static bool
    is_event_based();

    /**
     * @brief Add a task to be executed on unlock (or immediately if already unlocked).
     * @param task Function to execute.
     * @note New task replaces existing pending task to avoid duplicates.
     */
    static void
    add_unlock_task(UnlockCallback task);

    /**
     * @brief Clear the pending unlock task.
     */
    static void
    clear_unlock_task();

  private:
    // 单一mutex管理所有共享状态
    static std::mutex mutex_;
    
    // 解锁等待任务（单任务模式避免重复）
    static UnlockCallback pending_task_;
    
    // Worker线程执行任务
    static std::thread worker_thread_;
    static std::queue<UnlockCallback> task_queue_;
    static std::condition_variable cv_;
    static bool worker_running_;
    
    // 消息循环线程
    static HWND hidden_window_;
    static std::thread message_thread_;
    static std::atomic<bool> thread_running_;
    static std::atomic<bool> initialized_;
    static std::atomic<bool> event_based_;

    static LRESULT CALLBACK
    window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    
    static void
    message_loop();
    
    static void
    worker_loop();
  };

}  // namespace display_device

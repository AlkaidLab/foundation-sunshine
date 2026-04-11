/**
 * @file globals.h
 * @brief Declarations for globally accessible variables and functions.
 */
#pragma once

#include <atomic>

#include "entry_handler.h"
#include "thread_pool.h"
/**
 * @brief The encryption flag for microphone data.
 */
#define SS_ENC_MIC 0x08

/**
 * @brief A thread pool for processing tasks.
 */
extern thread_pool_util::ThreadPool task_pool;

/**
 * @brief A boolean flag to indicate whether the cursor should be displayed.
 */
extern bool display_cursor;

/**
 * @brief Atomic flag set by the input path to notify the capture thread that input has arrived.
 * @details When set, the capture thread skips frame pacing sleep to reduce input-to-display latency.
 */
extern std::atomic<bool> capture_input_activity;

namespace platf {
  struct high_precision_timer;
}

/**
 * @brief Pointer to the active capture timer, used by the input path to interrupt frame pacing sleep.
 * @details Set by the capture thread when starting, cleared when stopping. Thread-safe via atomic.
 */
extern std::atomic<platf::high_precision_timer *> active_capture_timer;

#ifdef _WIN32
  // Declare global singleton used for NVIDIA control panel modifications
  #include "platform/windows/nvprefs/nvprefs_interface.h"

/**
 * @brief A global singleton used for NVIDIA control panel modifications.
 */
extern nvprefs::nvprefs_interface nvprefs_instance;

extern const std::string VDD_NAME;
extern const std::string ZAKO_NAME;
extern std::string zako_device_id;

/**
 * @brief Cached result of is_running_as_system() check.
 * @details This is set once at program startup and never changes during runtime.
 */
extern bool is_running_as_system_user;
#endif

/**
 * @brief Handles process-wide communication.
 */
namespace mail {
#define MAIL(x)                         \
  constexpr auto x = std::string_view { \
    #x                                  \
  }

  /**
   * @brief A process-wide communication mechanism.
   */
  extern safe::mail_t man;

  // Global mail
  MAIL(shutdown);
  MAIL(broadcast_shutdown);
  MAIL(video_packets);
  MAIL(audio_packets);
  MAIL(switch_display);

  // Local mail
  MAIL(touch_port);
  MAIL(idr);
  MAIL(invalidate_ref_frames);
  MAIL(gamepad_feedback);
  MAIL(hdr);
  MAIL(dynamic_param_change);
  MAIL(resolution_change);
#undef MAIL

}  // namespace mail

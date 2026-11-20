/**
 * @file src/streaming/session_join_stage.h
 * @brief The steps of ending a streaming session and how long each may take before the host restarts itself.
 *
 * `stream::session::join()` runs a watchdog (an NVENC deadlock under hardware-accelerated GPU scheduling never lets the encoder thread return, and a host that
 * waits for it can never start another stream). Until 11-19 one 10 second limit covered the whole function, including work that is slow without being stuck:
 * the display topology and the virtual display are restored under the display session's lock, the platform timers are reset, the end of the stream is
 * recorded. The 11-19 reports of that limit (EXCEPTION_BREAKPOINT in the task of the watchdog, 14 reports of 11 installs on every GPU vendor, the same stack
 * each time) could not say which step it was.
 *
 * Two budgets now: the threads of the stream (video, audio, the packet queue, control) keep 10 seconds, the steps after them get 60. Each step has a name,
 * and the watchdog calls a different function per step, so the stack of the report names it.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <string_view>

namespace streaming {

  enum class join_stage_e : int {
    video_thread,  ///< waiting for the video thread to return
    audio_thread,  ///< waiting for the audio thread to return
    live_session,  ///< waiting for the broadcast threads to let go of the session
    control_end,  ///< waiting for the control thread to see the end
    input_reset,  ///< releasing keys and buttons the client still held
    display_restore,  ///< restoring the display topology and the virtual display
    platform_stop,  ///< resetting the platform (timer resolution, power requests, network optimisations)
    bookkeeping,  ///< leases, statistics, webhook, the record of the client
  };

  inline constexpr std::size_t join_stage_count = 8;

  constexpr std::string_view
  join_stage_name(join_stage_e stage) {
    switch (stage) {
      case join_stage_e::video_thread: return "video-thread";
      case join_stage_e::audio_thread: return "audio-thread";
      case join_stage_e::live_session: return "live-session";
      case join_stage_e::control_end: return "control-end";
      case join_stage_e::input_reset: return "input-reset";
      case join_stage_e::display_restore: return "display-restore";
      case join_stage_e::platform_stop: return "platform-stop";
      case join_stage_e::bookkeeping: return "bookkeeping";
    }
    return "unknown";
  }

  /// The steps that wait for a thread of the stream: the ones a driver deadlock can hold for ever.
  constexpr bool
  join_stage_waits_for_thread(join_stage_e stage) {
    return stage == join_stage_e::video_thread || stage == join_stage_e::audio_thread || stage == join_stage_e::live_session || stage == join_stage_e::control_end;
  }

  /// How long the session may stay in a step before it counts as hung.
  constexpr std::chrono::seconds
  join_stage_budget(join_stage_e stage) {
    return join_stage_waits_for_thread(stage) ? std::chrono::seconds { 10 } : std::chrono::seconds { 60 };
  }

}  // namespace streaming

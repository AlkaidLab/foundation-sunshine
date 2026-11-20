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

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
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

  /**
   * @brief The progress of one `join()` call, shared with the callback of its watchdog.
   *
   * Every call owns one (a `std::shared_ptr` kept alive by the callbacks, so a callback that runs after `join()` returned still reads valid memory): two sessions
   * ended at once (the shutdown `clear()` runs while the RTSP thread runs its own) never see each other's step. Every step change gets a new generation, and
   * a watchdog armed for an older generation, or for a session that already returned, finds nothing to report and does nothing.
   */
  class join_progress_t {
  public:
    /// Moves to the next step and returns the generation the watchdog of that step is armed for. Called by the thread that runs `join()`.
    std::uint64_t
    enter(join_stage_e next) {
      const auto generation = (state_.load(std::memory_order_relaxed) >> 8) + 1;
      state_.store((generation << 8) | static_cast<std::uint64_t>(next), std::memory_order_release);
      return generation;
    }

    /// The session is done: every watchdog still pending or already queued becomes a no-op.
    void
    finish() {
      const auto generation = (state_.load(std::memory_order_relaxed) >> 8) + 1;
      state_.store((generation << 8) | finished_marker, std::memory_order_release);
    }

    /// The step to blame when the watchdog armed for `generation` fires: empty when the session left that step or returned since.
    std::optional<join_stage_e>
    hung_stage(std::uint64_t generation) const {
      const auto state = state_.load(std::memory_order_acquire);
      if ((state >> 8) != generation || (state & 0xff) == finished_marker) {
        return std::nullopt;
      }
      return static_cast<join_stage_e>(state & 0xff);
    }

  private:
    static constexpr std::uint64_t finished_marker = 0xff;
    std::atomic<std::uint64_t> state_ { static_cast<std::uint64_t>(join_stage_e::video_thread) };
  };

}  // namespace streaming

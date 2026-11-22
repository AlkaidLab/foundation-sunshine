/**
 * @file src/audio_sink_switch.h
 * @brief Switch and restore the host default playback sink at stream start,
 *        on runtime host-playback toggles, and at stream end. Kept apart from
 *        platform audio APIs so both directions and restoration can be tested
 *        with a fake control.
 */
#pragma once

#include <string>

namespace audio_sink_switch {
  /**
   * @brief Apply the desired default playback sink for the current host-audio state.
   * @tparam Control Anything with `set_sink(const std::string &)`,
   *         `restore_sink_if_assigned(const std::string &)`, and
   *         `has_assigned_sink()`: `platf::audio_control_t` or a test fake.
   * @param control Audio control backend.
   * @param host_sink Original default host playback sink (`ctx.sink.host`).
   * @param target_sink Desired default sink (`host_sink` when playing on host,
   *        or the virtual sink when host playback is muted).
   * @param keep_sink_default Whether `config::audio.keep_sink_default` is enabled.
   * @param restore_sink Tracks whether the default sink is currently replaced away
   *        from `host_sink`.
   * @return True when capture may continue; false when switching away from
   *         `host_sink` failed.
   */
  template <class Control>
  bool
  apply_target_sink(Control &control,
    const std::string &host_sink,
    const std::string &target_sink,
    bool keep_sink_default,
    bool &restore_sink) {
    if (host_sink != target_sink) {
      if (control.has_assigned_sink() && !host_sink.empty()) {
        // Restore any prior host-sink assignment first so a subsequent
        // set_sink() records the pre-stream default roles instead of the
        // sink assigned by Sunshine.
        if (control.restore_sink_if_assigned(host_sink)) {
          restore_sink = false;
        }
      }
      if (control.set_sink(target_sink) != 0) {
        return false;
      }
      restore_sink = true;
      return true;
    }

    bool set_called = false;
    if (restore_sink || control.has_assigned_sink()) {
      if (!host_sink.empty()) {
        if (control.restore_sink_if_assigned(host_sink)) {
          restore_sink = false;
        }
        else if (restore_sink) {
          const int sink_status = control.set_sink(host_sink);
          set_called = true;
          if (sink_status == 0) {
            restore_sink = false;
          }
        }
      }
      else {
        restore_sink = false;
      }
    }

    if (!set_called && keep_sink_default && !target_sink.empty()) {
      (void) control.set_sink(target_sink);
    }
    return true;
  }

  /**
   * @brief Restore the original host sink when the audio context stops.
   */
  template <class Control>
  void
  restore_on_stop(Control &control,
    const std::string &host_sink,
    const std::string &configured_sink,
    bool keep_sink_default,
    bool &restore_sink) {
    const bool restore_assigned_sink = keep_sink_default && control.has_assigned_sink();
    if (!restore_sink && !restore_assigned_sink) {
      return;
    }

    const std::string &sink = host_sink.empty() ? configured_sink : host_sink;
    if (!sink.empty()) {
      if (!control.restore_sink_if_assigned(sink)) {
        if (restore_sink) {
          control.set_sink(sink);
        }
      }
    }
    restore_sink = false;
  }
}  // namespace audio_sink_switch

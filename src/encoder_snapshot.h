/**
 * @file src/encoder_snapshot.h
 * @brief The chosen encoder as an immutable published snapshot, and when a launch may reuse the last encoder probe instead of probing again.
 *
 * The probe used to write the live encoder objects: it reset `chosen_encoder` to null, then rewrote the capability flags of every candidate while capture and
 * encode threads of a running or just-starting session dereferenced the same pointer (a null `video::chosen_encoder` in `video::capture` ended the host with
 * 0xc0000005 on 2026-11-12). Now a probe works on private copies and publishes the winner with one pointer swap. A session takes the snapshot once and keeps it:
 * it never sees null after the first successful probe, never sees a half-written flag set, and a failed probe leaves the previous snapshot in place.
 *
 * The reuse decision is pure logic over a fingerprint of everything the answer depends on; the platform code only fills the fingerprint in.
 */
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace encoder_snapshot {

  /**
   * @brief One published, immutable value. Readers get a shared owner that stays valid for as long as they hold it.
   * @note `publish` ignores an empty pointer: nothing can reset the slot to null, which is the guarantee the capture threads rely on.
   */
  template <class T>
  class slot_t {
  public:
    std::shared_ptr<const T>
    get() const {
      std::lock_guard lock { mutex };
      return value;
    }

    void
    publish(std::shared_ptr<const T> next) {
      if (!next) {
        return;
      }
      std::lock_guard lock { mutex };
      value = std::move(next);
    }

  private:
    mutable std::mutex mutex;
    std::shared_ptr<const T> value;
  };

  /** Everything the probe's answer depends on. Two fingerprints are the same when every part is; an empty `gpus` means the platform cannot tell. */
  struct probe_fingerprint_t {
    std::string gpus;  ///< vendor, device, LUID and driver of every adapter
    std::string options;  ///< encoder, codec modes, adapter, output and capture options that reach the probe
    std::string capture_backend;  ///< the configured backend
    std::string probe_capture;  ///< the backend the probe captures with (may differ from the configured one)
    std::string probe_display;  ///< the display the probe opens, "" for the backend's own choice
    std::string desktop;  ///< every active display: id, mode and HDR state

    friend bool
    operator==(const probe_fingerprint_t &, const probe_fingerprint_t &) = default;
  };

  enum class reuse_e {
    reuse,
    no_snapshot,  ///< nothing was ever chosen
    always_reprobe,  ///< the chosen encoder is one of last resort
    unknown_gpu,  ///< the platform gave no GPU identity, so nothing can be proven unchanged
    no_record,  ///< the last probe did not succeed, or was forgotten
    changed,  ///< a part of the fingerprint differs
  };

  inline const char *
  describe(reuse_e verdict) {
    switch (verdict) {
      case reuse_e::reuse:
        return "unchanged";
      case reuse_e::no_snapshot:
        return "no encoder chosen yet";
      case reuse_e::always_reprobe:
        return "the chosen encoder is probed on every start";
      case reuse_e::unknown_gpu:
        return "the GPU identity is unknown";
      case reuse_e::no_record:
        return "the last probe did not succeed";
      case reuse_e::changed:
        return "the display, GPU or options changed";
    }
    return "";
  }

  inline reuse_e
  decide_reuse(bool has_snapshot, bool always_reprobe, const std::optional<probe_fingerprint_t> &last_success, const probe_fingerprint_t &now) {
    if (!has_snapshot) {
      return reuse_e::no_snapshot;
    }
    if (always_reprobe) {
      return reuse_e::always_reprobe;
    }
    if (now.gpus.empty()) {
      return reuse_e::unknown_gpu;
    }
    if (!last_success) {
      return reuse_e::no_record;
    }
    return *last_success == now ? reuse_e::reuse : reuse_e::changed;
  }

  /** The fingerprint of the last probe that succeeded. A failed probe or a temporary option change forgets it. */
  class memo_t {
  public:
    std::optional<probe_fingerprint_t>
    last() const {
      std::lock_guard lock { mutex };
      return fingerprint;
    }

    void
    remember(probe_fingerprint_t next) {
      std::lock_guard lock { mutex };
      fingerprint = std::move(next);
    }

    void
    forget() {
      std::lock_guard lock { mutex };
      fingerprint.reset();
    }

  private:
    mutable std::mutex mutex;
    std::optional<probe_fingerprint_t> fingerprint;
  };

}  // namespace encoder_snapshot

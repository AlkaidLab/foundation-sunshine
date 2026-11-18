/**
 * @file src/encoder_probe_guard.h
 * @brief Remembers which encoder was being probed when the process ended, so the next start does not walk into the same crash.
 */
#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace encoder_probe_guard {

  inline constexpr const char *FILE_NAME = "encoder_probe_guard.txt";

  inline std::filesystem::path
  marker_path(const std::filesystem::path &dir) {
    return dir / FILE_NAME;
  }

  /**
   * @brief The encoder the previous process was inside of when it ended, and forget it ("" when there is none).
   *
   * The probe loads the graphics driver's encoder, capture and shader code. When that code crashes (a crash report from a start-up loop ended inside the
   * NVIDIA driver 1.3 s after the start, 29 starts in a row; another inside the AMD runtime 5 s after the start), the service starts the core again and the
   * same probe runs into the same crash. The marker is written when a probe starts and removed when it ends, so a process that died in between leaves it
   * behind: the next start skips that one encoder (the others still run) and the stale marker is gone, so the encoder is tried again at the start after.
   */
  inline std::string
  take_interrupted(const std::filesystem::path &dir) {
    const auto path = marker_path(dir);
    std::string name;
    {
      std::ifstream in { path };
      if (in) std::getline(in, name);
    }
    while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
    if (name.size() > 64) name.clear();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return name;
  }

  /**
   * @brief Whether the probe of @p encoder is to be skipped because the previous process ended inside it.
   * A person who asked for this encoder (@p configured), and a check the person started (@p user_check), are not held back.
   */
  inline bool
  should_skip(const std::string &interrupted, const std::string &encoder, const std::string &configured, bool user_check) {
    return !user_check && !interrupted.empty() && interrupted == encoder && configured != encoder;
  }

  /** The probe of @p name starts: write the marker (best effort, a probe never fails because of it). */
  inline void
  enter(const std::filesystem::path &dir, const std::string &name) {
    std::error_code ignored;
    std::filesystem::create_directories(dir, ignored);
    std::ofstream out { marker_path(dir), std::ios::trunc };
    if (out) out << name << '\n';
  }

  /** The probe ended: remove the marker. */
  inline void
  leave(const std::filesystem::path &dir) {
    std::error_code ignored;
    std::filesystem::remove(marker_path(dir), ignored);
  }

  /** Marks a probe for the length of the scope. */
  class scope_t {
  public:
    scope_t(std::filesystem::path dir, const std::string &name):
        dir_ { std::move(dir) } {
      enter(dir_, name);
    }

    ~scope_t() {
      leave(dir_);
    }

    scope_t(const scope_t &) = delete;
    scope_t &
    operator=(const scope_t &) = delete;

  private:
    std::filesystem::path dir_;
  };

}  // namespace encoder_probe_guard

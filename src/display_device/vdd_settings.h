/**
 * @file src/display_device/vdd_settings.h
 * @brief Persist virtual display modes.
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>

#ifdef SUNSHINE_TESTS
  #include <functional>
#endif

namespace display_device::vdd_settings {
  enum class save_result {
    saved,
    missing_file,
    failed,
  };

  /**
   * @brief Stage legacy VDD mode/GPU settings and restore their original bytes unless committed.
   * @details The caller must hold the configuration save lock through destruction.
   * Missing XML is a no-op. Rollback errors are logged; this is not crash recovery.
   */
  class transaction {
  public:
    /** Snapshot the existing XML, refusing writes if it cannot be read safely. */
    transaction();
    /** Restore staged XML on a failed configuration save without throwing. */
    ~transaction();
    transaction(const transaction &) = delete;
    transaction &
    operator=(const transaction &) = delete;

    /** Write general modes/GPU only; an empty list uses the driver's fallback. */
    save_result
    save(std::string resArray, std::string fpsArray, std::string gpu_name);
    /** Keep the staged XML after the Sunshine configuration has been saved. */
    void
    commit() noexcept { restore_ = false; }

  private:
    std::filesystem::path path_;
    std::string original_;
    save_result state_ = save_result::failed;
    bool restore_ = false;
  };

#ifdef SUNSHINE_TESTS
  namespace test_support {
    /** Require an explicit fixture path; nullopt disables writes in test builds. */
    void
    set_settings_path(const std::optional<std::filesystem::path> &path);
    /** Pause a staged save for deterministic concurrency tests; absent in production. */
    void
    set_before_save_hook(std::function<void()> hook);
  }  // namespace test_support
#endif
}  // namespace display_device::vdd_settings

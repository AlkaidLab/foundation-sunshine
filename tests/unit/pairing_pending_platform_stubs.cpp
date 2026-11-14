/**
 * @file tests/unit/pairing_pending_platform_stubs.cpp
 * @brief Minimal platform values for the focused pairing-pending suite.
 */
#include "src/config.h"
#include "src/platform/common.h"
#include "src/platform/windows/nvprefs/nvprefs_interface.h"

#ifdef _WIN32
  #include "src/platform/windows/misc.h"
#endif

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The focused suite does not link the full configuration parser. Only the
// global values read by the pairing and Web UI modules are needed here.
namespace config {
  video_t video;
  audio_t audio;
  stream_t stream;
  nvhttp_t nvhttp;
  input_t input;
  sunshine_t sunshine;
}  // namespace config

namespace platf {
#ifdef _WIN32
  bool
  is_running_as_system() {
    return false;
  }

  HANDLE
  retrieve_users_token(bool) {
    return nullptr;
  }
#endif

  std::filesystem::path
  appdata() {
    return std::filesystem::temp_directory_path() / "sunshine-pairing-pending-test";
  }

  std::string
  get_host_name() {
    return "pairing-pending-test";
  }

  std::vector<supported_gamepad_t> &
  supported_gamepads(input_t *) {
    static std::vector<supported_gamepad_t> gamepads {
      { "auto", true, {} },
    };
    return gamepads;
  }

  void
  set_global_gamepad_mode(std::string_view) {
  }
}  // namespace platf

namespace nvprefs {

  nvprefs_interface::nvprefs_interface() = default;
  nvprefs_interface::~nvprefs_interface() = default;

  bool
  nvprefs_interface::configure() {
    return false;
  }

  bool
  nvprefs_interface::restore() {
    return false;
  }

  std::shared_ptr<void>
  nvprefs_interface::acquire_vsync() {
    return {};
  }

  nvprefs_interface::status_t
  nvprefs_interface::status() {
    return { false, "unavailable", false };
  }

}  // namespace nvprefs

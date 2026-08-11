/**
 * @file tests/unit/test_video_probe.cpp
 * @brief Tests for temporary encoder-probe display selection.
 */
#include <src/video_probe.h>

#include <array>
#include <iostream>

namespace {

  bool
  preserves_capture_ready_configured_display() {
    const std::array displays { std::string { R"(\\.\DISPLAY7)" }, std::string { R"(\\.\DISPLAY9)" } };
    return video::select_encoder_probe_display(R"(\\.\DISPLAY9)", displays, "Configured GPU") == R"(\\.\DISPLAY9)";
  }

  bool
  configured_adapter_uses_backend_autoselection() {
    const std::array displays { std::string { R"(\\.\DISPLAY7)" }, std::string { R"(\\.\DISPLAY9)" } };
    return video::select_encoder_probe_display(R"(\\.\DISPLAY18)", displays, "Configured GPU").empty();
  }

  bool
  unfiltered_probe_uses_first_capture_ready_display() {
    const std::array displays { std::string { R"(\\.\DISPLAY7)" }, std::string { R"(\\.\DISPLAY9)" } };
    return video::select_encoder_probe_display(R"(\\.\DISPLAY18)", displays, {}) == R"(\\.\DISPLAY7)";
  }

}  // namespace

int
main() {
  if (!preserves_capture_ready_configured_display()) {
    std::cerr << "configured display preservation test failed\n";
    return 1;
  }
  if (!configured_adapter_uses_backend_autoselection()) {
    std::cerr << "configured adapter selection test failed\n";
    return 1;
  }
  if (!unfiltered_probe_uses_first_capture_ready_display()) {
    std::cerr << "unfiltered display fallback test failed\n";
    return 1;
  }
  return 0;
}

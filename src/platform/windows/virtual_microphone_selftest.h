/**
 * @file src/platform/windows/virtual_microphone_selftest.h
 * @brief End-to-end check of the virtual microphone: a tone goes in, Windows must hear it.
 */
#pragma once

#include <string>

namespace platf::audio {

  struct virtual_microphone_selftest_result {
    bool success { false };
    /** MIC_USBIP_COMPONENT_UNAVAILABLE, MIC_TEST_DEVICE_UNAVAILABLE or MIC_TEST_WRITE_FAILED when it failed. */
    std::string error_code;
    /** Seconds of audio the Windows capture endpoint delivered during the test. */
    double heard_seconds { -1.0 };
    /** Amplitude of the 440 Hz tone found on the capture endpoint (0.18 was written). */
    double heard_amplitude { 0 };
    /** 500 ms windows judged, and how many of them held the tone. */
    int windows_total { 0 };
    int windows_good { 0 };
  };

  /**
   * Create a virtual microphone, write a 440 Hz tone into it in real time and
   * record the Windows capture endpoint of the device for a few seconds.  The
   * test passes when the tone dominates at least two thirds of the 500 ms windows
   * after the first second of the recording.
   */
  virtual_microphone_selftest_result
  run_virtual_microphone_selftest();

}  // namespace platf::audio

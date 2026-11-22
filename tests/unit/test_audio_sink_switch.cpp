/**
 * @file tests/unit/test_audio_sink_switch.cpp
 * @brief Default playback sink switching at stream start, on runtime host_audio
 *        toggles in both directions, and at stream end, tested with a fake control.
 */
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "src/audio_sink_switch.h"

namespace {
  struct fake_control_t {
    std::string current_default = "Speakers (Realtek)";
    std::optional<std::string> assigned_sink;
    std::optional<std::string> saved_original;
    bool supports_ownership_restore = true;
    int fail_set_sink_calls = 0;

    std::vector<std::string> set_sink_calls;
    std::vector<std::string> restore_calls;

    int
    set_sink(const std::string &sink) {
      set_sink_calls.push_back(sink);
      if (fail_set_sink_calls > 0) {
        --fail_set_sink_calls;
        return -1;
      }
      if (supports_ownership_restore) {
        // Mirrors Windows set_sink_locked(): captures the current default before switching.
        saved_original = current_default;
        assigned_sink = sink;
      }
      current_default = sink;
      return 0;
    }

    bool
    restore_sink_if_assigned(const std::string &sink) {
      restore_calls.push_back(sink);
      if (!supports_ownership_restore) {
        return false;
      }
      if (!assigned_sink) {
        return true;
      }
      if (current_default == *assigned_sink && saved_original && *saved_original != *assigned_sink) {
        current_default = *saved_original;
      }
      assigned_sink.reset();
      saved_original.reset();
      return true;
    }

    bool
    has_assigned_sink() const {
      return supports_ownership_restore && assigned_sink.has_value();
    }
  };

  constexpr auto kHostSink = "Speakers (Realtek)";
  constexpr auto kVirtualSink = "virtual-Surround 7.1{steam-guid}";
}  // namespace

TEST(AudioSinkSwitch, StartWithHostAudioTrueThenMuteAndRestoreOnRuntimeToggle) {
  fake_control_t control;
  bool restore_sink = false;

  // Stream starts with host_audio = true: default stays on host speakers.
  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kHostSink, false, restore_sink));
  EXPECT_FALSE(restore_sink);
  EXPECT_EQ(control.current_default, kHostSink);
  EXPECT_TRUE(control.set_sink_calls.empty());

  // Runtime toggle host_audio -> false: switches default to virtual sink.
  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kVirtualSink, false, restore_sink));
  EXPECT_TRUE(restore_sink);
  EXPECT_EQ(control.current_default, kVirtualSink);
  EXPECT_EQ(control.set_sink_calls, (std::vector<std::string> { kVirtualSink }));

  // Runtime toggle host_audio -> true: restores default back to host speakers.
  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kHostSink, false, restore_sink));
  EXPECT_FALSE(restore_sink);
  EXPECT_EQ(control.current_default, kHostSink);
  EXPECT_FALSE(control.has_assigned_sink());

  // Stream ends: nothing left to restore.
  const auto restore_count_before_stop = control.restore_calls.size();
  audio_sink_switch::restore_on_stop(control, kHostSink, "", false, restore_sink);
  EXPECT_EQ(control.restore_calls.size(), restore_count_before_stop);
  EXPECT_EQ(control.current_default, kHostSink);
}

TEST(AudioSinkSwitch, StartWithHostAudioTrueMuteMidStreamThenRestoreAtStreamEnd) {
  fake_control_t control;
  bool restore_sink = false;

  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kHostSink, false, restore_sink));
  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kVirtualSink, false, restore_sink));
  EXPECT_TRUE(restore_sink);
  EXPECT_EQ(control.current_default, kVirtualSink);

  // Stream ends while host_audio is still false: default must return to host speakers.
  audio_sink_switch::restore_on_stop(control, kHostSink, "", false, restore_sink);
  EXPECT_FALSE(restore_sink);
  EXPECT_EQ(control.current_default, kHostSink);
  EXPECT_FALSE(control.has_assigned_sink());
}

TEST(AudioSinkSwitch, StartWithHostAudioFalseThenUnmuteAndRemuteAndRestoreAtStreamEnd) {
  fake_control_t control;
  bool restore_sink = false;

  // Stream starts with host_audio = false: switches default to virtual sink.
  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kVirtualSink, false, restore_sink));
  EXPECT_TRUE(restore_sink);
  EXPECT_EQ(control.current_default, kVirtualSink);

  // Runtime toggle host_audio -> true: restores default to host speakers.
  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kHostSink, false, restore_sink));
  EXPECT_FALSE(restore_sink);
  EXPECT_EQ(control.current_default, kHostSink);
  EXPECT_FALSE(control.has_assigned_sink());

  // Runtime toggle host_audio -> false again: switches back to virtual sink.
  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kVirtualSink, false, restore_sink));
  EXPECT_TRUE(restore_sink);
  EXPECT_EQ(control.current_default, kVirtualSink);

  // Stream ends: restores default to host speakers.
  audio_sink_switch::restore_on_stop(control, kHostSink, "", false, restore_sink);
  EXPECT_FALSE(restore_sink);
  EXPECT_EQ(control.current_default, kHostSink);
}

TEST(AudioSinkSwitch, KeepSinkDefaultPreservesPreStreamOriginalAcrossToggles) {
  fake_control_t control;
  // Simulate pre-stream default being restored accurately across keep_sink_default transitions.
  control.current_default = kHostSink;
  bool restore_sink = false;

  // Start with host_audio = true and keep_sink_default = true.
  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kHostSink, true, restore_sink));
  EXPECT_FALSE(restore_sink);
  EXPECT_TRUE(control.has_assigned_sink());
  EXPECT_EQ(control.current_default, kHostSink);

  // Toggle -> false: restores prior assignment first so saved_original is kHostSink, not overwritten.
  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kVirtualSink, true, restore_sink));
  EXPECT_TRUE(restore_sink);
  EXPECT_EQ(control.current_default, kVirtualSink);
  EXPECT_EQ(control.saved_original, std::optional<std::string>(kHostSink));

  // Toggle -> true: restores kHostSink, then re-arms keep_sink_default on kHostSink.
  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kHostSink, true, restore_sink));
  EXPECT_FALSE(restore_sink);
  EXPECT_EQ(control.current_default, kHostSink);
  EXPECT_TRUE(control.has_assigned_sink());
  EXPECT_EQ(control.saved_original, std::optional<std::string>(kHostSink));

  // Stream ends: restores cleanly to kHostSink without bouncing back to virtual.
  audio_sink_switch::restore_on_stop(control, kHostSink, "", true, restore_sink);
  EXPECT_FALSE(restore_sink);
  EXPECT_FALSE(control.has_assigned_sink());
  EXPECT_EQ(control.current_default, kHostSink);
}

TEST(AudioSinkSwitch, FailedVirtualSwitchDoesNotMarkRestoreSink) {
  fake_control_t control;
  control.fail_set_sink_calls = 1;
  bool restore_sink = false;

  EXPECT_FALSE(audio_sink_switch::apply_target_sink(control, kHostSink, kVirtualSink, false, restore_sink));
  EXPECT_FALSE(restore_sink);
  EXPECT_EQ(control.current_default, kHostSink);
}

TEST(AudioSinkSwitch, RespectsUserDefaultChangeOnRuntimeUnmute) {
  fake_control_t control;
  bool restore_sink = false;

  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kVirtualSink, false, restore_sink));
  EXPECT_EQ(control.current_default, kVirtualSink);

  // User manually switches Windows default away from the virtual sink mid-stream.
  control.current_default = "USB Headphones";

  // Runtime toggle host_audio -> true must not clobber the user's chosen device.
  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kHostSink, false, restore_sink));
  EXPECT_FALSE(restore_sink);
  EXPECT_EQ(control.current_default, "USB Headphones");
  EXPECT_FALSE(control.has_assigned_sink());
}

TEST(AudioSinkSwitch, FallbackPlatformWithoutOwnershipRestoreSwitchesBothDirections) {
  fake_control_t control;
  control.supports_ownership_restore = false;
  bool restore_sink = false;

  // Start host_audio = true with keep_sink_default = false.
  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kHostSink, false, restore_sink));
  EXPECT_FALSE(restore_sink);
  EXPECT_TRUE(control.set_sink_calls.empty());

  // Toggle -> false.
  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kVirtualSink, false, restore_sink));
  EXPECT_TRUE(restore_sink);
  EXPECT_EQ(control.current_default, kVirtualSink);

  // Toggle -> true (keep_sink_default = true should not call set_sink twice).
  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kHostSink, true, restore_sink));
  EXPECT_FALSE(restore_sink);
  EXPECT_EQ(control.current_default, kHostSink);
  EXPECT_EQ(control.set_sink_calls, (std::vector<std::string> { kVirtualSink, kHostSink }));

  // Toggle -> false, then stream end restores via set_sink.
  ASSERT_TRUE(audio_sink_switch::apply_target_sink(control, kHostSink, kVirtualSink, true, restore_sink));
  EXPECT_EQ(control.current_default, kVirtualSink);
  audio_sink_switch::restore_on_stop(control, kHostSink, "", true, restore_sink);
  EXPECT_FALSE(restore_sink);
  EXPECT_EQ(control.current_default, kHostSink);
}

#include <future>
#include <limits>

#include <gtest/gtest.h>

#include "src/nvhttp.h"

namespace {
  TEST(PresetPinGeneration, FailedOldRequestCannotClearNewPin) {
    nvhttp::clear_preset_pin();
    const auto old_generation = nvhttp::set_preset_pin_with_generation("1234", "old", 120);
    const auto new_generation = nvhttp::set_preset_pin_with_generation("5678", "new", 120);
    ASSERT_TRUE(old_generation);
    ASSERT_TRUE(new_generation);
    EXPECT_NE(*old_generation, *new_generation);
    EXPECT_FALSE(nvhttp::clear_preset_pin_if_generation(*old_generation));
    EXPECT_EQ(nvhttp::consume_preset_pin(), "5678");
  }

  TEST(PresetPinGeneration, FailedCurrentRequestClearsOwnPin) {
    nvhttp::clear_preset_pin();
    const auto generation = nvhttp::set_preset_pin_with_generation("1234", "current", 120);
    ASSERT_TRUE(generation);
    EXPECT_TRUE(nvhttp::clear_preset_pin_if_generation(*generation));
    EXPECT_TRUE(nvhttp::consume_preset_pin().empty());
  }

  TEST(PresetPinGeneration, DelayedFailureOnAnotherThreadCannotClearReplacement) {
    nvhttp::clear_preset_pin();
    const auto generation = nvhttp::set_preset_pin_with_generation("1234", "old", 120);
    ASSERT_TRUE(generation);
    std::promise<void> replacement_ready;
    auto ready = replacement_ready.get_future();
    auto delayed_failure = std::async(std::launch::async, [&] {
      ready.wait();
      return nvhttp::clear_preset_pin_if_generation(*generation);
    });
    const auto replacement = nvhttp::set_preset_pin_with_generation("5678", "new", 120);
    replacement_ready.set_value();
    ASSERT_TRUE(replacement);
    EXPECT_FALSE(delayed_failure.get());
    EXPECT_EQ(nvhttp::consume_preset_pin(), "5678");
  }

  TEST(PresetPinGeneration, InvalidPinDoesNotReplaceOrClearCurrentGeneration) {
    nvhttp::clear_preset_pin();
    const auto generation = nvhttp::set_preset_pin_with_generation("1234", "current", 120);
    ASSERT_TRUE(generation);
    EXPECT_FALSE(nvhttp::set_preset_pin_with_generation("ab12", "invalid", 120));
    EXPECT_EQ(nvhttp::consume_preset_pin(), "1234");
  }
}  // namespace

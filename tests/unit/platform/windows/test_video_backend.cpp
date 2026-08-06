/**
 * @file tests/unit/platform/windows/test_video_backend.cpp
 * @brief Tests for Windows video backend selection.
 */
#include "src/platform/windows/video_backend.h"

#include <gtest/gtest.h>

namespace {
  namespace backend = platf::dxgi::video_backend;

  TEST(WindowsVideoBackend, DefaultsAutoToD3D11WhileStageIsUnavailable) {
    const auto selection = backend::resolve("auto", std::nullopt, false, false);

    EXPECT_EQ(selection.requested, backend::windows_video_backend_e::automatic);
    EXPECT_EQ(selection.effective, backend::effective_backend_e::d3d11);
    EXPECT_EQ(selection.fallback, backend::fallback_reason_e::build_stage_unavailable);
    EXPECT_FALSE(selection.strict);
    EXPECT_TRUE(selection.pipeline_available());
  }

  TEST(WindowsVideoBackend, D3D11SkipsD3D12Selection) {
    const auto selection = backend::resolve("d3d11", std::nullopt, true, false);

    EXPECT_EQ(selection.requested, backend::windows_video_backend_e::d3d11);
    EXPECT_EQ(selection.effective, backend::effective_backend_e::d3d11);
    EXPECT_EQ(selection.fallback, backend::fallback_reason_e::none);
    EXPECT_FALSE(selection.strict);
  }

  TEST(WindowsVideoBackend, EnvironmentOverridesPersistentConfiguration) {
    const auto selection = backend::resolve("d3d11", "d3d12", false, false);

    EXPECT_EQ(selection.requested, backend::windows_video_backend_e::d3d12);
    EXPECT_TRUE(selection.environment_override);
    EXPECT_EQ(selection.effective, backend::effective_backend_e::d3d11);
    EXPECT_EQ(selection.fallback, backend::fallback_reason_e::build_stage_unavailable);
  }

  TEST(WindowsVideoBackend, StrictD3D12StillStreamsUntilAnAttemptFails) {
    const auto selection = backend::resolve("auto", "d3d12", true, false);

    EXPECT_TRUE(selection.strict);
    EXPECT_EQ(selection.effective, backend::effective_backend_e::d3d11);
    EXPECT_TRUE(selection.pipeline_available());
  }

  TEST(WindowsVideoBackend, StrictD3D12FailsOnlyAfterARealInitializationFailure) {
    auto selection = backend::resolve("auto", "d3d12", true, false);
    backend::apply_d3d12_initialization(
      selection, false, backend::fallback_reason_e::d3d12_device_failed);

    EXPECT_EQ(selection.fallback, backend::fallback_reason_e::d3d12_device_failed);
    EXPECT_EQ(selection.effective, backend::effective_backend_e::unavailable);
    EXPECT_FALSE(selection.pipeline_available());
  }

  TEST(WindowsVideoBackend, SuccessfulD3D12InitializationClearsTheBuildStageFallback) {
    auto selection = backend::resolve("d3d12", std::nullopt, true, false);
    backend::apply_d3d12_initialization(
      selection, true, backend::fallback_reason_e::none);

    EXPECT_EQ(selection.fallback, backend::fallback_reason_e::none);
    EXPECT_TRUE(selection.pipeline_available());
  }

  TEST(WindowsVideoBackend, NonStrictD3D12FailureKeepsStreaming) {
    auto selection = backend::resolve("d3d12", std::nullopt, false, false);
    backend::apply_d3d12_initialization(
      selection, false, backend::fallback_reason_e::d3d12_device_failed);

    EXPECT_EQ(selection.effective, backend::effective_backend_e::d3d11);
    EXPECT_TRUE(selection.pipeline_available());
  }

  TEST(WindowsVideoBackend, AutoIsNeverTakenDownByAStrictFlag) {
    auto selection = backend::resolve("auto", std::nullopt, true, false);
    backend::apply_d3d12_initialization(
      selection, false, backend::fallback_reason_e::d3d12_device_failed);

    EXPECT_FALSE(selection.strict);
    EXPECT_EQ(selection.effective, backend::effective_backend_e::d3d11);
    EXPECT_TRUE(selection.pipeline_available());
  }

  TEST(WindowsVideoBackend, InvalidOverrideFallsBackToAuto) {
    const auto selection = backend::resolve("d3d11", "D3D12", true, false);

    EXPECT_TRUE(selection.invalid_value);
    EXPECT_EQ(selection.requested, backend::windows_video_backend_e::automatic);
    EXPECT_EQ(selection.effective, backend::effective_backend_e::d3d11);
    EXPECT_FALSE(selection.strict);
  }

  TEST(WindowsVideoBackend, StableFallbackReasonNames) {
    EXPECT_EQ(
      backend::to_string(backend::fallback_reason_e::encoder_resource_registration_failed),
      "encoder_resource_registration_failed");
    EXPECT_EQ(
      backend::to_string(backend::fallback_reason_e::runtime_fence_failed),
      "runtime_fence_failed");
  }
}  // namespace

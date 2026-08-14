/**
 * @file tests/unit/platform/windows/test_ds5_sidecar_client.cpp
 * @brief Regression coverage for cancellable DS5 Core pipe shutdown.
 */
#ifdef _WIN32

  #include <chrono>

  #include "src/config.h"
  #include "src/platform/windows/ds5/ds5_sidecar_client.h"
  #include <gtest/gtest.h>

TEST(Ds5SidecarClientTests, AllocThenFreeCancelsBlockedReader) {
  const auto previous_enabled = config::input.ds5_enabled;
  const auto previous_path = config::input.ds5_sidecar_path;
  config::input.ds5_enabled = true;
  config::input.ds5_sidecar_path = SUNSHINE_DS5_FAKE_SIDECAR_PATH;

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-lifecycle-test");
  platf::ds5::sidecar_client_t client;
  ASSERT_EQ(client.alloc({ 0, 0 }, std::move(feedback), false), 0);

  const auto started = std::chrono::steady_clock::now();
  client.free(0);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  config::input.ds5_enabled = previous_enabled;
  config::input.ds5_sidecar_path = previous_path;
  EXPECT_LT(elapsed, std::chrono::seconds(2));
}

#endif

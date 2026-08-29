/**
 * @file tests/unit/platform/windows/test_microphone_client.cpp
 * @brief Product-facing virtual microphone transport tests.
 */
#include <array>
#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

#include "src/platform/windows/virtual_device_host/microphone_client.h"

using namespace std::chrono_literals;

TEST(MicrophoneClient, CreatesStreamsFlushesAndPublishesStatus) {
  platf::virtual_device_host::microphone_client_t client;

  ASSERT_TRUE(client.start());
  EXPECT_TRUE(client.online());

  auto status = platf::virtual_device_host::microphone_status();
  EXPECT_TRUE(status.component_available);
  EXPECT_TRUE(status.online);
  EXPECT_TRUE(status.device_created);
  EXPECT_EQ(status.generation, 1u);
  EXPECT_EQ(status.state, "idle");

  std::array<std::int16_t, 480> pcm {};
  pcm[0] = 4096;
  for (int packet = 0; packet < 100; ++packet) {
    EXPECT_EQ(client.write_pcm(pcm.data(), pcm.size()),
              static_cast<int>(pcm.size() * sizeof(std::int16_t)));
    std::this_thread::sleep_for(10ms);
  }

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  do {
    status = platf::virtual_device_host::microphone_status();
    if (status.host_streaming) break;
    std::this_thread::sleep_for(10ms);
  } while (std::chrono::steady_clock::now() < deadline);

  EXPECT_TRUE(status.online);
  EXPECT_TRUE(status.device_created);
  EXPECT_TRUE(status.host_streaming);
  EXPECT_EQ(status.state, "remote_active");
  EXPECT_EQ(status.dropped_frames, 0u);
  EXPECT_EQ(status.submit_errors, 0u);
  EXPECT_TRUE(client.flush());
}

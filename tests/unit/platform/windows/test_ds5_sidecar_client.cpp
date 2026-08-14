/**
 * @file tests/unit/platform/windows/test_ds5_sidecar_client.cpp
 * @brief Regression coverage for cancellable DS5 Core pipe shutdown.
 */
#ifdef _WIN32

  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>

  #include <chrono>

  #include "src/config.h"
  #include "src/platform/windows/ds5/ds5_sidecar_client.h"
  #include <gtest/gtest.h>

namespace {
  struct config_scope_t {
    config_scope_t():
        enabled(config::input.ds5_enabled),
        path(config::input.ds5_sidecar_path) {}

    ~config_scope_t() {
      config::input.ds5_enabled = enabled;
      config::input.ds5_sidecar_path = path;
    }

    bool enabled;
    std::string path;
  };

  struct handle_scope_t {
    explicit handle_scope_t(HANDLE value): handle(value) {}
    ~handle_scope_t() {
      if (handle) CloseHandle(handle);
    }
    HANDLE handle;
  };
}  // namespace

TEST(Ds5SidecarClientTests, AllocThenFreeCancelsBlockedReader) {
  config_scope_t restore_config;
  config::input.ds5_enabled = true;
  config::input.ds5_sidecar_path = SUNSHINE_DS5_FAKE_SIDECAR_PATH;

  const auto process_id = std::to_wstring(GetCurrentProcessId());
  const auto reader_name = L"Local\\sunshine-ds5-test-reader-" + process_id;
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + process_id;
  handle_scope_t reader_event(CreateEventW(nullptr, FALSE, FALSE, reader_name.c_str()));
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, FALSE, continue_name.c_str()));
  ASSERT_NE(reader_event.handle, nullptr);
  ASSERT_NE(continue_event.handle, nullptr);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-lifecycle-test");
  auto feedback_for_test = feedback;
  platf::ds5::sidecar_client_t client;
  ASSERT_EQ(client.alloc({ 0, 0 }, std::move(feedback), false), 0);

  ASSERT_EQ(WaitForSingleObject(reader_event.handle, 2000), WAIT_OBJECT_0);
  ASSERT_TRUE(SetEvent(continue_event.handle));
  const auto marker = feedback_for_test->pop(std::chrono::seconds(2));
  ASSERT_TRUE(marker);
  ASSERT_EQ(marker->type, platf::gamepad_feedback_e::rumble);
  // The second signal is raised immediately before the next overlapped read.
  ASSERT_EQ(WaitForSingleObject(reader_event.handle, 2000), WAIT_OBJECT_0);

  const auto started = std::chrono::steady_clock::now();
  client.free(0);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST(Ds5SidecarClientTests, RejectsCompositeAttachWithoutAudioEndpoint) {
  config_scope_t restore_config;
  config::input.ds5_enabled = true;
  config::input.ds5_sidecar_path = SUNSHINE_DS5_FAKE_SIDECAR_PATH;

  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" +
                             std::to_wstring(GetCurrentProcessId());
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, TRUE, continue_name.c_str()));
  ASSERT_NE(continue_event.handle, nullptr);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-audio-attach-test");
  platf::ds5::sidecar_client_t client;
  EXPECT_EQ(client.alloc({ 0, 0 }, std::move(feedback), true), -1);
  EXPECT_FALSE(client.owns(0));
}

TEST(Ds5SidecarClientTests, RelaunchesOnceAfterUnexpectedExit) {
  config_scope_t restore_config;
  config::input.ds5_enabled = true;
  config::input.ds5_sidecar_path = SUNSHINE_DS5_FAKE_SIDECAR_PATH;

  const auto process_id = std::to_wstring(GetCurrentProcessId());
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + process_id;
  const auto crash_name = L"Local\\sunshine-ds5-test-crash-once-" + process_id;
  const auto recovered_name = L"Local\\sunshine-ds5-test-recovered-" + process_id;
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, FALSE, continue_name.c_str()));
  handle_scope_t crash_event(CreateEventW(nullptr, TRUE, FALSE, crash_name.c_str()));
  handle_scope_t recovered_event(CreateEventW(nullptr, FALSE, FALSE, recovered_name.c_str()));
  ASSERT_NE(continue_event.handle, nullptr);
  ASSERT_NE(crash_event.handle, nullptr);
  ASSERT_NE(recovered_event.handle, nullptr);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-recovery-test");
  auto feedback_for_test = feedback;
  platf::ds5::sidecar_client_t client;
  ASSERT_EQ(client.alloc({ 0, 0 }, std::move(feedback), false), 0);
  ASSERT_EQ(WaitForSingleObject(recovered_event.handle, 5000), WAIT_OBJECT_0);
  ASSERT_TRUE(SetEvent(continue_event.handle));
  const auto marker = feedback_for_test->pop(std::chrono::seconds(2));
  ASSERT_TRUE(marker);
  ASSERT_EQ(marker->type, platf::gamepad_feedback_e::rumble);
  EXPECT_TRUE(client.owns(0));
  client.free(0);
}

#endif

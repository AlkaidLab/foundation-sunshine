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

  struct event_namespace_scope_t {
    explicit event_namespace_scope_t(std::wstring_view test_name):
        suffix(std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(GetTickCount64()) + L"-" + std::wstring(test_name)) {
      SetEnvironmentVariableW(L"SUNSHINE_DS5_TEST_EVENT_SUFFIX", suffix.c_str());
    }

    ~event_namespace_scope_t() {
      SetEnvironmentVariableW(L"SUNSHINE_DS5_TEST_EVENT_SUFFIX", nullptr);
    }

    std::wstring suffix;
  };
}  // namespace

TEST(Ds5SidecarClientTests, UnassignedIndexIsNotOwned) {
  platf::ds5::sidecar_client_t client;
  EXPECT_FALSE(client.owns(-1));
}

TEST(Ds5SidecarClientTests, AllocThenFreeCancelsBlockedReader) {
  config_scope_t restore_config;
  config::input.ds5_enabled = true;
  config::input.ds5_sidecar_path = SUNSHINE_DS5_FAKE_SIDECAR_PATH;

  event_namespace_scope_t events(L"blocked-reader");
  const auto reader_name = L"Local\\sunshine-ds5-test-reader-" + events.suffix;
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + events.suffix;
  const auto marker_name = L"Local\\sunshine-ds5-test-marker-" + events.suffix;
  handle_scope_t reader_event(CreateEventW(nullptr, FALSE, FALSE, reader_name.c_str()));
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, FALSE, continue_name.c_str()));
  handle_scope_t marker_event(CreateEventW(nullptr, FALSE, FALSE, marker_name.c_str()));
  ASSERT_NE(reader_event.handle, nullptr);
  ASSERT_NE(continue_event.handle, nullptr);
  ASSERT_NE(marker_event.handle, nullptr);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-lifecycle-test");
  auto feedback_for_test = feedback;
  platf::ds5::sidecar_client_t client;
  ASSERT_EQ(client.alloc({ 0, 0 }, std::move(feedback), false), 0);

  ASSERT_EQ(WaitForSingleObject(reader_event.handle, 2000), WAIT_OBJECT_0);
  ASSERT_TRUE(SetEvent(continue_event.handle));
  ASSERT_EQ(WaitForSingleObject(marker_event.handle, 2000), WAIT_OBJECT_0);
  const auto marker = feedback_for_test->pop(std::chrono::seconds(2));
  ASSERT_TRUE(marker);
  ASSERT_EQ(marker->type, platf::gamepad_feedback_e::rumble);
  // The second signal is raised only after the marker was processed and the
  // following overlapped read is actually pending.
  ASSERT_EQ(WaitForSingleObject(reader_event.handle, 2000), WAIT_OBJECT_0);

  const auto started = std::chrono::steady_clock::now();
  client.free(0);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST(Ds5SidecarClientTests, AttachSurvivesInterleavedAsyncFeedback) {
  config_scope_t restore_config;
  config::input.ds5_enabled = true;
  config::input.ds5_sidecar_path = SUNSHINE_DS5_FAKE_SIDECAR_PATH;

  event_namespace_scope_t events(L"interleaved-feedback");
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + events.suffix;
  const auto marker_name = L"Local\\sunshine-ds5-test-marker-" + events.suffix;
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, FALSE, continue_name.c_str()));
  handle_scope_t marker_event(CreateEventW(nullptr, FALSE, FALSE, marker_name.c_str()));
  ASSERT_NE(continue_event.handle, nullptr);
  ASSERT_NE(marker_event.handle, nullptr);
  ASSERT_NE(SetEnvironmentVariableW(L"SUNSHINE_DS5_TEST_INTERLEAVE", L"1"), 0);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-interleave-test");
  auto feedback_for_test = feedback;
  platf::ds5::sidecar_client_t client;
  // The fake peer emits an async rumble ahead of the attach reply. The
  // transaction must dispatch it and still match the reply; before the
  // multiplexing fix the rumble was misread as the reply and alloc failed.
  EXPECT_EQ(client.alloc({ 0, 0 }, std::move(feedback), false), 0);
  SetEnvironmentVariableW(L"SUNSHINE_DS5_TEST_INTERLEAVE", nullptr);
  const auto early = feedback_for_test->pop(std::chrono::seconds(2));
  ASSERT_TRUE(early);
  EXPECT_EQ(early->type, platf::gamepad_feedback_e::rumble);

  ASSERT_TRUE(SetEvent(continue_event.handle));
  ASSERT_EQ(WaitForSingleObject(marker_event.handle, 2000), WAIT_OBJECT_0);
  const auto marker = feedback_for_test->pop(std::chrono::seconds(2));
  ASSERT_TRUE(marker);
  EXPECT_EQ(marker->type, platf::gamepad_feedback_e::rumble);
  client.free(0);
}

TEST(Ds5SidecarClientTests, RejectsCompositeAttachWithoutAudioEndpoint) {
  config_scope_t restore_config;
  config::input.ds5_enabled = true;
  config::input.ds5_sidecar_path = SUNSHINE_DS5_FAKE_SIDECAR_PATH;

  event_namespace_scope_t events(L"audio-endpoint");
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + events.suffix;
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

  event_namespace_scope_t events(L"recover-once");
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + events.suffix;
  const auto crash_name = L"Local\\sunshine-ds5-test-crash-once-" + events.suffix;
  const auto recovered_name = L"Local\\sunshine-ds5-test-recovered-" + events.suffix;
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

TEST(Ds5SidecarClientTests, FreeCancelsPendingRecovery) {
  config_scope_t restore_config;
  config::input.ds5_enabled = true;
  config::input.ds5_sidecar_path = SUNSHINE_DS5_FAKE_SIDECAR_PATH;

  event_namespace_scope_t events(L"cancel-recovery");
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + events.suffix;
  const auto crash_name = L"Local\\sunshine-ds5-test-crash-once-" + events.suffix;
  const auto recovery_started_name = L"Local\\sunshine-ds5-test-recovery-started-" + events.suffix;
  const auto recovery_wait_name = L"Local\\sunshine-ds5-test-recovery-wait-" + events.suffix;
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, FALSE, continue_name.c_str()));
  handle_scope_t crash_event(CreateEventW(nullptr, TRUE, FALSE, crash_name.c_str()));
  handle_scope_t recovery_started_event(CreateEventW(nullptr, FALSE, FALSE, recovery_started_name.c_str()));
  handle_scope_t recovery_wait_event(CreateEventW(nullptr, TRUE, FALSE, recovery_wait_name.c_str()));
  ASSERT_NE(continue_event.handle, nullptr);
  ASSERT_NE(crash_event.handle, nullptr);
  ASSERT_NE(recovery_started_event.handle, nullptr);
  ASSERT_NE(recovery_wait_event.handle, nullptr);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-cancel-recovery-test");
  platf::ds5::sidecar_client_t client;
  ASSERT_EQ(client.alloc({ 0, 0 }, std::move(feedback), false), 0);
  ASSERT_EQ(WaitForSingleObject(recovery_started_event.handle, 5000), WAIT_OBJECT_0);

  // The transport is offline, but the sidecar still owns the controller id.
  // The input layer relies on owns() to route release to sidecar_client_t::free().
  EXPECT_TRUE(client.owns(0));
  const auto started = std::chrono::steady_clock::now();
  client.free(0);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_LT(elapsed, std::chrono::seconds(3));
  EXPECT_FALSE(client.owns(0));
}

TEST(Ds5SidecarClientTests, ReallocatesAfterRecoveryFailure) {
  config_scope_t restore_config;
  config::input.ds5_enabled = true;
  config::input.ds5_sidecar_path = SUNSHINE_DS5_FAKE_SIDECAR_PATH;

  event_namespace_scope_t events(L"recovery-failure");
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + events.suffix;
  const auto crash_name = L"Local\\sunshine-ds5-test-crash-always-" + events.suffix;
  handle_scope_t continue_event(CreateEventW(nullptr, FALSE, FALSE, continue_name.c_str()));
  handle_scope_t crash_event(CreateEventW(nullptr, FALSE, FALSE, crash_name.c_str()));
  ASSERT_NE(continue_event.handle, nullptr);
  ASSERT_NE(crash_event.handle, nullptr);

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-recovery-failure-test");
  platf::ds5::sidecar_client_t client;
  ASSERT_EQ(client.alloc({ 0, 0 }, std::move(feedback), false), 0);

  // Both the initial sidecar and its one recovery attempt exit immediately.
  ASSERT_EQ(WaitForSingleObject(crash_event.handle, 5000), WAIT_OBJECT_0);
  ASSERT_EQ(WaitForSingleObject(crash_event.handle, 5000), WAIT_OBJECT_0);

  int result = -1;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (unsigned attempt = 0; result < 0 && std::chrono::steady_clock::now() < deadline; ++attempt) {
    auto retry_feedback = mail->queue<platf::gamepad_feedback_msg_t>(
      "ds5-recovery-failure-retry-" + std::to_string(attempt));
    result = client.alloc({ 1, 0 }, std::move(retry_feedback), false);
    if (result < 0) {
      Sleep(10);
    }
  }
  EXPECT_EQ(result, 0);
}

#endif

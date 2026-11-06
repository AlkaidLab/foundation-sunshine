/**
 * @file tests/unit/test_audio_vb_cable.cpp
 * @brief The VB-CABLE install and enable steps against a fake system: nothing is downloaded, installed or enabled.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "src/audio_vb_cable.h"

namespace {
  using namespace audio_devices::vb_cable;

  // the subject of the setup program of the pack, as read from a copy of it on Windows test host on 2026-11-12
  constexpr std::string_view burel = "CN=Vincent Burel, OU=Individual Developer, O=No Organization Affiliation, L=Eymet, S=Dordogne, C=FR";

  struct fake_t: environment_t {
    bool is_elevated = true;
    std::string state_now = "missing";
    std::string state_after = "ready";
    std::optional<std::string> disabled_id = std::string("ROOT\\MEDIA\\0000");
    bool download_ok = true;
    bool extract_ok = true;
    std::string setup_found = "C:/work/VBCABLE_Setup_x64.exe";
    signature_t signature { true, std::string(burel) };
    std::optional<int> setup_exit = 0;
    std::optional<int> pnputil_exit = 0;
    std::vector<std::string> calls;
    std::string url_used;
    std::string pnputil_arguments;

    bool elevated() override { return is_elevated; }
    std::string state() override { return state_now; }
    std::optional<std::string> disabled_instance_id() override { return disabled_id; }
    bool download(std::string_view url, const std::string &) override {
      calls.push_back("download");
      url_used = std::string(url);
      return download_ok;
    }
    bool extract(const std::string &, const std::string &) override {
      calls.push_back("extract");
      return extract_ok;
    }
    std::string find_setup(const std::string &) override { return setup_found; }
    signature_t verify_signature(const std::string &) override {
      calls.push_back("verify");
      return signature;
    }
    std::optional<int> run_setup(const std::string &) override {
      calls.push_back("setup");
      return setup_exit;
    }
    std::optional<int> run_pnputil(const std::string &arguments) override {
      calls.push_back("pnputil");
      pnputil_arguments = arguments;
      return pnputil_exit;
    }
    std::string work_directory() override {
      calls.push_back("mkdir");
      return "C:/work";
    }
    void remove_directory(const std::string &) override { calls.push_back("rmdir"); }
    std::string settle_state() override { return state_after; }
  };

  struct steps_t {
    std::vector<std::string> seen;
    progress_t callback() {
      return [this](std::string_view step) { seen.emplace_back(step); };
    }
  };
}  // namespace

TEST(VbCable, OnlyVbAudiosPublisherIsAccepted) {
  EXPECT_TRUE(signer_accepted(burel));
  EXPECT_TRUE(signer_accepted("CN=VB-Audio Software, O=VB-Audio, C=FR"));
  EXPECT_FALSE(signer_accepted("CN=Contoso Ltd, O=Contoso, C=US"));
  EXPECT_FALSE(signer_accepted("CN=Evil Corp, O=Vincent Burel"));  // naming him is not being him
  EXPECT_FALSE(signer_accepted(""));
}

TEST(VbCable, OnlyARootDeviceIdGoesToPnputil) {
  EXPECT_TRUE(valid_instance_id("ROOT\\MEDIA\\0000"));
  EXPECT_TRUE(valid_instance_id("ROOT\\VBAUDIOVACWDM\\0000"));
  EXPECT_FALSE(valid_instance_id(""));
  EXPECT_FALSE(valid_instance_id("ROOT\\"));
  EXPECT_FALSE(valid_instance_id("USB\\VID_046D&PID_C52B\\6&1234"));
  EXPECT_FALSE(valid_instance_id("ROOT\\MEDIA\\0000\" /force"));
  EXPECT_FALSE(valid_instance_id("ROOT\\MEDIA\\0000 & calc"));
  EXPECT_FALSE(valid_instance_id("ROOT\\MEDIA\\0000;calc"));
  EXPECT_FALSE(valid_instance_id(std::string("ROOT\\") + std::string(300, 'A')));
  EXPECT_EQ(enable_arguments("ROOT\\MEDIA\\0000"), "/enable-device \"ROOT\\MEDIA\\0000\"");
}

TEST(VbCable, InstallRunsDownloadVerifyAndSetupInThatOrder) {
  fake_t env;
  steps_t steps;
  const auto result = install(env, steps.callback());
  EXPECT_TRUE(result.ok) << result.error_code;
  EXPECT_EQ(env.url_used, std::string(download_url));
  EXPECT_EQ(env.calls, (std::vector<std::string> { "mkdir", "download", "extract", "verify", "setup", "rmdir" }));
  EXPECT_EQ(steps.seen, (std::vector<std::string> { "downloading", "verifying", "installing" }));
}

TEST(VbCable, InstallNeedsAnElevatedHostAndTouchesNothingWithout) {
  fake_t env;
  env.is_elevated = false;
  steps_t steps;
  const auto result = install(env, steps.callback());
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error_code, error::needs_admin);
  EXPECT_TRUE(env.calls.empty());
}

TEST(VbCable, InstallDoesNothingWhenItIsReadyAndEnablesInsteadWhenOnlyDisabled) {
  fake_t ready;
  ready.state_now = "ready";
  steps_t steps;
  EXPECT_EQ(install(ready, steps.callback()).error_code, error::already_ready);
  EXPECT_TRUE(ready.calls.empty());
  fake_t disabled;
  disabled.state_now = "disabled";
  EXPECT_EQ(install(disabled, steps.callback()).error_code, error::use_enable);
  EXPECT_TRUE(disabled.calls.empty());
}

TEST(VbCable, InstallStopsAtEachFailureAndCleansUp) {
  steps_t steps;
  {
    fake_t env;
    env.download_ok = false;
    EXPECT_EQ(install(env, steps.callback()).error_code, error::download_failed);
    EXPECT_EQ(env.calls.back(), "rmdir");
    EXPECT_EQ(std::count(env.calls.begin(), env.calls.end(), "extract"), 0);
  }
  {
    fake_t env;
    env.extract_ok = false;
    EXPECT_EQ(install(env, steps.callback()).error_code, error::extract_failed);
    EXPECT_EQ(env.calls.back(), "rmdir");
  }
  {
    fake_t env;
    env.setup_found.clear();
    EXPECT_EQ(install(env, steps.callback()).error_code, error::setup_missing);
    EXPECT_EQ(std::count(env.calls.begin(), env.calls.end(), "verify"), 0);
  }
}

TEST(VbCable, TheSetupIsNeverStartedWithoutAValidSignatureOfTheRightPublisher) {
  steps_t steps;
  {
    fake_t env;
    env.signature = { false, {} };
    const auto result = install(env, steps.callback());
    EXPECT_EQ(result.error_code, error::signature_invalid);
    EXPECT_EQ(std::count(env.calls.begin(), env.calls.end(), "setup"), 0);
    EXPECT_EQ(env.calls.back(), "rmdir");
  }
  {
    fake_t env;
    env.signature = { true, "CN=Contoso Ltd, C=US" };
    const auto result = install(env, steps.callback());
    EXPECT_EQ(result.error_code, error::signer_unexpected);
    EXPECT_EQ(result.detail, "CN=Contoso Ltd, C=US");
    EXPECT_EQ(std::count(env.calls.begin(), env.calls.end(), "setup"), 0);
  }
}

TEST(VbCable, InstallReportsATimeoutAFailingSetupAndADeviceThatStaysAway) {
  steps_t steps;
  {
    fake_t env;
    env.setup_exit = std::nullopt;
    EXPECT_EQ(install(env, steps.callback()).error_code, error::install_timeout);
  }
  {
    fake_t env;
    env.setup_exit = 5;
    const auto result = install(env, steps.callback());
    EXPECT_EQ(result.error_code, error::install_failed);
    EXPECT_EQ(result.detail, "exit code 5");
  }
  {
    fake_t env;
    env.state_after = "missing";
    const auto result = install(env, steps.callback());
    EXPECT_EQ(result.error_code, error::still_unavailable);
    EXPECT_EQ(result.detail, "missing");
  }
  {
    fake_t env;
    env.state_after = "partial";  // one side is enough to count as installed
    EXPECT_TRUE(install(env, steps.callback()).ok);
  }
}

TEST(VbCable, EnableHandsTheDisabledDeviceToPnputil) {
  fake_t env;
  env.state_now = "disabled";
  steps_t steps;
  const auto result = enable(env, steps.callback());
  EXPECT_TRUE(result.ok) << result.error_code;
  EXPECT_EQ(env.pnputil_arguments, "/enable-device \"ROOT\\MEDIA\\0000\"");
  EXPECT_EQ(steps.seen, (std::vector<std::string> { "enabling" }));
  EXPECT_EQ(std::count(env.calls.begin(), env.calls.end(), "download"), 0);
}

TEST(VbCable, EnableRefusesWhatIsNotADisabledVbCable) {
  steps_t steps;
  {
    fake_t env;
    env.is_elevated = false;
    EXPECT_EQ(enable(env, steps.callback()).error_code, error::needs_admin);
    EXPECT_TRUE(env.calls.empty());
  }
  {
    fake_t env;
    env.state_now = "ready";
    EXPECT_EQ(enable(env, steps.callback()).error_code, error::already_ready);
    EXPECT_TRUE(env.calls.empty());
  }
  {
    fake_t env;
    env.disabled_id = std::nullopt;
    EXPECT_EQ(enable(env, steps.callback()).error_code, error::nothing_to_enable);
    EXPECT_TRUE(env.calls.empty());
  }
  {
    fake_t env;
    env.disabled_id = std::string("USB\\VID_1234&PID_5678\\1");  // only a root device of a virtual driver
    EXPECT_EQ(enable(env, steps.callback()).error_code, error::nothing_to_enable);
    EXPECT_TRUE(env.calls.empty());
  }
}

TEST(VbCable, EnableOutcomes) {
  steps_t steps;
  {
    fake_t env;
    env.pnputil_exit = 3010;  // enabled, a restart is asked for
    EXPECT_TRUE(enable(env, steps.callback()).ok);
  }
  {
    fake_t env;
    env.pnputil_exit = 5;
    const auto result = enable(env, steps.callback());
    EXPECT_EQ(result.error_code, error::enable_failed);
    EXPECT_EQ(result.detail, "exit code 5");
  }
  {
    fake_t env;
    env.pnputil_exit = std::nullopt;
    EXPECT_EQ(enable(env, steps.callback()).detail, "timeout");
  }
  {
    fake_t env;
    env.state_after = "disabled";  // pnputil said yes, the device did not come back
    EXPECT_EQ(enable(env, steps.callback()).error_code, error::still_unavailable);
  }
}

TEST(VbCable, OnlyOneJobRunsAtATime) {
  job_runner_t runner;
  std::mutex gate;
  std::condition_variable open;
  bool released = false;
  EXPECT_TRUE(runner.start("install", [&](const progress_t &progress) {
    progress("downloading");
    std::unique_lock lock(gate);
    open.wait(lock, [&] { return released; });
    return result_t { true, {}, {} };
  }));
  EXPECT_FALSE(runner.start("enable", [](const progress_t &) { return result_t { true, {}, {} }; }));
  while (runner.status().step != "downloading") std::this_thread::yield();
  const auto running = runner.status();
  EXPECT_TRUE(running.running);
  EXPECT_EQ(running.action, "install");
  {
    std::lock_guard lock(gate);
    released = true;
  }
  open.notify_all();
  runner.wait();
  const auto done = runner.status();
  EXPECT_FALSE(done.running);
  EXPECT_EQ(done.step, "done");
  EXPECT_TRUE(runner.start("enable", [](const progress_t &) { return result_t { false, std::string(error::enable_failed), "exit code 5" }; }));
  runner.wait();
  const auto failed = runner.status();
  EXPECT_EQ(failed.action, "enable");
  EXPECT_EQ(failed.step, "failed");
  EXPECT_EQ(failed.error_code, error::enable_failed);
  const auto json = status_json(failed);
  EXPECT_EQ(json["error_code"], "VB_ENABLE_FAILED");
  EXPECT_FALSE(json["running"].get<bool>());
}

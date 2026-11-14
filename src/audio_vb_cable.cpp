/**
 * @file src/audio_vb_cable.cpp
 * @brief The order of the VB-CABLE install and enable steps and the rules around them (no Windows API in this file).
 */
#include "audio_vb_cable.h"

#include <algorithm>
#include <cctype>

namespace audio_devices::vb_cable {
  namespace {
    std::string
    lower(std::string_view text) {
      std::string out(text);
      std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return out;
    }

    result_t
    fail(std::string_view code, std::string detail = {}) {
      return { false, std::string(code), std::move(detail) };
    }

    /// The state after a step: usable (both sides, or one side as the microphone route may need only one) is success.
    result_t
    finish(environment_t &environment) {
      const auto state = environment.settle_state();
      if (state == "ready" || state == "partial") return { true, {}, state };
      return fail(error::still_unavailable, state);
    }
  }  // namespace

  bool
  signer_accepted(std::string_view subject) {
    const auto text = lower(subject);
    return text.find("cn=vincent burel") != std::string::npos || text.find("vb-audio") != std::string::npos;
  }

  bool
  valid_instance_id(std::string_view instance_id) {
    constexpr std::string_view prefix = "ROOT\\";
    if (instance_id.size() <= prefix.size() || instance_id.size() > 200) return false;
    if (instance_id.substr(0, prefix.size()) != prefix) return false;
    return std::all_of(instance_id.begin(), instance_id.end(), [](unsigned char c) {
      return std::isalnum(c) || c == '\\' || c == '_' || c == '&' || c == '{' || c == '}' || c == '.' || c == '-';
    });
  }

  std::string
  enable_arguments(std::string_view instance_id) {
    return "/enable-device \"" + std::string(instance_id) + "\"";
  }

  result_t
  install(environment_t &environment, const progress_t &progress) {
    if (!environment.elevated()) return fail(error::needs_admin);
    const auto state = environment.state();
    if (state == "ready") return fail(error::already_ready);
    // an installed driver whose device is only disabled is not installed again, it is enabled
    if (state == "disabled") return fail(error::use_enable);

    const auto directory = environment.work_directory();
    struct cleanup_t {
      environment_t &environment;
      const std::string &directory;
      ~cleanup_t() {
        if (!directory.empty()) environment.remove_directory(directory);
      }
    } cleanup { environment, directory };
    if (directory.empty()) return fail(error::download_failed, "no work directory");

    progress("downloading");
    const auto zip_path = directory + "/VBCABLE_Driver_Pack43.zip";
    if (!environment.download(download_url, zip_path)) return fail(error::download_failed);
    if (!environment.extract(zip_path, directory)) return fail(error::extract_failed);
    const auto setup = environment.find_setup(directory);
    if (setup.empty()) return fail(error::setup_missing);

    // nothing of the pack is started before its signature is known
    progress("verifying");
    const auto signature = environment.verify_signature(setup);
    if (!signature.valid) return fail(error::signature_invalid, signature.signer);
    if (!signer_accepted(signature.signer)) return fail(error::signer_unexpected, signature.signer);

    progress("installing");
    const auto code = environment.run_setup(setup);
    if (!code) return fail(error::install_timeout);
    if (*code != 0) return fail(error::install_failed, "exit code " + std::to_string(*code));
    return finish(environment);
  }

  result_t
  enable(environment_t &environment, const progress_t &progress) {
    if (!environment.elevated()) return fail(error::needs_admin);
    if (environment.state() == "ready") return fail(error::already_ready);
    const auto instance_id = environment.disabled_instance_id();
    if (!instance_id) return fail(error::nothing_to_enable);
    if (!valid_instance_id(*instance_id)) return fail(error::nothing_to_enable, "unexpected device id");

    progress("enabling");
    const auto code = environment.run_pnputil(enable_arguments(*instance_id));
    // 3010: enabled, a restart is still asked for; the state decides
    if (!code || (*code != 0 && *code != 3010)) return fail(error::enable_failed, code ? "exit code " + std::to_string(*code) : "timeout");
    return finish(environment);
  }

  bool
  job_runner_t::start(std::string action, std::function<result_t(const progress_t &)> work) {
    std::unique_lock lock(mutex_);
    if (status_.running) return false;
    if (worker_.joinable()) worker_.join();
    status_ = {};
    status_.action = std::move(action);
    status_.running = true;
    worker_ = std::thread([this, work = std::move(work)]() {
      const auto result = work([this](std::string_view step) {
        std::lock_guard guard(mutex_);
        status_.step = std::string(step);
      });
      std::lock_guard guard(mutex_);
      status_.running = false;
      status_.step = result.ok ? "done" : "failed";
      status_.error_code = result.error_code;
      status_.detail = result.detail;
      done_.notify_all();
    });
    return true;
  }

  job_status_t
  job_runner_t::status() const {
    std::lock_guard lock(mutex_);
    return status_;
  }

  void
  job_runner_t::wait() {
    std::unique_lock lock(mutex_);
    done_.wait(lock, [this]() { return !status_.running; });
  }

  job_runner_t::~job_runner_t() {
    wait();
    if (worker_.joinable()) worker_.join();
  }

  nlohmann::json
  status_json(const job_status_t &status) {
    return {
      { "action", status.action },
      { "step", status.step },
      { "running", status.running },
      { "error_code", status.error_code },
      { "detail", status.detail },
    };
  }
}  // namespace audio_devices::vb_cable

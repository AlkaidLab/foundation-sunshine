/**
 * @file src/audio_vb_cable.h
 * @brief Installing VB-CABLE and enabling its disabled device from the Web UI and the Panel.
 *
 * The original Foundation Sunshine only offers a download link (Web UI) and a script, install-vsink.bat, that an
 * administrator runs by hand: it downloads VBCABLE_Driver_Pack43.zip from VB-Audio, unpacks it and starts
 * `VBCABLE_Setup_x64.exe /S`. Here the host does the same on request: the download comes from VB-Audio only, the
 * setup program must carry a valid Authenticode signature of VB-Audio's publisher before it is started, a disabled device
 * is enabled with pnputil, and nothing starts without an explicit confirmation in the request.
 *
 * This file is the part that needs no Windows: the order of the steps, the rules (who may have signed the setup, which
 * device id may be handed to pnputil) and the single running job. The Windows side supplies the steps through
 * `environment_t` (src/platform/windows/audio_vb_cable.cpp); the tests supply fakes.
 */
#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

namespace audio_devices::vb_cable {
  /// The driver pack VB-Audio publishes and the original script downloads.
  constexpr std::string_view download_url = "https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip";
  /// The setup program inside the pack (64-bit).
  constexpr std::string_view setup_file = "VBCABLE_Setup_x64.exe";

  /// Error codes of a job (the `error_code` of the status), grouped by what a person can do about them.
  namespace error {
    constexpr std::string_view unsupported = "VB_UNSUPPORTED";  ///< not Windows
    constexpr std::string_view busy = "VB_BUSY";  ///< another job is running
    constexpr std::string_view needs_admin = "VB_NEEDS_ADMIN";  ///< the host does not run elevated
    constexpr std::string_view already_ready = "VB_ALREADY_READY";  ///< nothing to do
    constexpr std::string_view use_enable = "VB_USE_ENABLE";  ///< installed, only disabled: enable it
    constexpr std::string_view nothing_to_enable = "VB_NOTHING_TO_ENABLE";  ///< no disabled VB-CABLE device found
    constexpr std::string_view download_failed = "VB_DOWNLOAD_FAILED";
    constexpr std::string_view extract_failed = "VB_EXTRACT_FAILED";
    constexpr std::string_view setup_missing = "VB_SETUP_MISSING";  ///< the pack has no setup program
    constexpr std::string_view signature_invalid = "VB_SIGNATURE_INVALID";  ///< no valid Authenticode signature
    constexpr std::string_view signer_unexpected = "VB_SIGNER_UNEXPECTED";  ///< signed, but not by VB-Audio's publisher
    constexpr std::string_view install_failed = "VB_INSTALL_FAILED";  ///< the setup program failed or could not start
    constexpr std::string_view install_timeout = "VB_INSTALL_TIMEOUT";
    constexpr std::string_view enable_failed = "VB_ENABLE_FAILED";
    constexpr std::string_view still_unavailable = "VB_STILL_UNAVAILABLE";  ///< the step succeeded, the device is still not usable
  }  // namespace error

  /**
   * @brief Whether a certificate subject is VB-Audio's publisher.
   *
   * The pack is signed by the developer himself: the subject of the installed setup program is
   * "CN=Vincent Burel, OU=Individual Developer, ..., C=FR" (read on 2026-11-12 from a copy of the same pack). A subject
   * naming VB-Audio is accepted as well, in case the publisher moves to a company certificate.
   */
  bool
  signer_accepted(std::string_view subject);

  /// Whether a device instance id may be handed to pnputil: the root-enumerated media device of a virtual driver, nothing else.
  bool
  valid_instance_id(std::string_view instance_id);

  /// The arguments of `pnputil /enable-device <id>`.
  std::string
  enable_arguments(std::string_view instance_id);

  /// What a signature check found.
  struct signature_t {
    bool valid = false;  ///< WinVerifyTrust accepted the file
    std::string signer;  ///< the subject of the signing certificate
  };

  /// What the job needs from the system; the Windows implementation does it, the tests fake it.
  struct environment_t {
    virtual ~environment_t() = default;
    /// The host runs with an elevated token.
    virtual bool
    elevated() = 0;
    /// The state of VB-CABLE now (ready, partial, disabled, unavailable, missing).
    virtual std::string
    state() = 0;
    /// The instance id of the VB-CABLE device node that is disabled in the Device Manager, if there is one.
    virtual std::optional<std::string>
    disabled_instance_id() = 0;
    /// Downloads `url` into `zip_path`. Only called with `download_url`.
    virtual bool
    download(std::string_view url, const std::string &zip_path) = 0;
    /// Unpacks the zip into `directory`.
    virtual bool
    extract(const std::string &zip_path, const std::string &directory) = 0;
    /// The path of the setup program inside `directory`, or empty when it is not there.
    virtual std::string
    find_setup(const std::string &directory) = 0;
    virtual signature_t
    verify_signature(const std::string &file) = 0;
    /// Runs the setup program silently and waits. Returns its exit code, or nullopt on a timeout (the process is ended).
    virtual std::optional<int>
    run_setup(const std::string &setup_path) = 0;
    /// Runs `pnputil` with the arguments and waits; the exit code, or nullopt on a timeout.
    virtual std::optional<int>
    run_pnputil(const std::string &arguments) = 0;
    /// A working directory for one install, created for it and not shared with other users.
    virtual std::string
    work_directory() = 0;
    virtual void
    remove_directory(const std::string &directory) = 0;
    /// Waits up to a few seconds for the endpoints of a new or enabled device to show up, then returns the state.
    virtual std::string
    settle_state() = 0;
  };

  struct result_t {
    bool ok = false;
    std::string error_code;
    std::string detail;  ///< facts for the log and the diagnostics, never shown as a sentence
  };

  /// The step a running job is in, for the status.
  using progress_t = std::function<void(std::string_view step)>;

  /// Download, verify and run the setup of VB-CABLE. Steps: downloading, verifying, installing.
  result_t
  install(environment_t &environment, const progress_t &progress);

  /// Enable the disabled VB-CABLE device with pnputil. Step: enabling.
  result_t
  enable(environment_t &environment, const progress_t &progress);

  /// The status of the job as GET /api/audio/devices reports it.
  struct job_status_t {
    std::string action;  ///< "install", "enable" or "" before the first job
    std::string step;  ///< downloading, verifying, installing, enabling, done, failed, or ""
    bool running = false;
    std::string error_code;
    std::string detail;
  };

  /// One job at a time; a job runs on a thread of its own.
  class job_runner_t {
  public:
    /// Starts `work` as `action`. False (and nothing started) when a job is running.
    bool
    start(std::string action, std::function<result_t(const progress_t &)> work);
    job_status_t
    status() const;
    /// Waits for the running job to end (tests; the host never waits).
    void
    wait();
    ~job_runner_t();

  private:
    mutable std::mutex mutex_;
    std::condition_variable done_;
    job_status_t status_;
    bool thread_started_ = false;
    std::thread worker_;
  };

  nlohmann::json
  status_json(const job_status_t &status);

#ifdef _WIN32
  /// The real system: Windows APIs, VB-Audio's server, System32\\tar.exe and pnputil.exe.
  std::unique_ptr<environment_t>
  make_system_environment();

  /// The one job runner of the host (never destroyed: a job may still run at exit).
  job_runner_t &
  runner();
#endif
}  // namespace audio_devices::vb_cable

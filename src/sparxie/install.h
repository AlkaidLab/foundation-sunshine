#pragma once

#include "src/sparxie/host.h"

#include <functional>
#include <string>
#include <vector>

namespace sparxie {
  struct Pin {
    std::string version;
    std::string file_name;
    std::string sha256;
    std::uint64_t bytes = 0;
    std::string note;
    // a release older than kCurrentVersion: it still installs, and the person is told to use a newer one
    bool outdated = false;
  };

  struct Probes {
    bool supplied = false;
    bool os_known = false;
    int os_build = 0;
    bool os_x64 = false;
    bool free_known = false;
    std::uint64_t free_bytes = 0;
    bool webview2_known = false;
    bool webview2 = false;
    std::string webview2_version;
    bool vcruntime_known = false;
    bool vcruntime = false;
    std::string vcruntime_version;
    bool write_known = false;
    bool can_write = false;
    bool elevation_known = false;
    bool can_elevate = false;
    std::function<bool(const fs::path &)> isolated;
  };

  struct Progress {
    std::string stage;
    int percent = 0;
    std::string detail;
  };

  struct InstallRequest {
    fs::path zip;
    fs::path dest;
    bool confirm_unverified = false;
    bool acknowledged = false;
    bool running = false;
    Probes probes;
    std::vector<Pin> pins;
  };

  struct InstallOutcome {
    Status status;
    json body = json::object();
    std::vector<Progress> progress;
  };

  struct UninstallRequest {
    fs::path dest;
    fs::path data_dir;
    bool running = false;
    bool delete_config_set = false;
    bool delete_config = false;
  };

  // The oldest Sparxie this host is made for. Older releases are outdated: nothing is done to keep them working.
  inline constexpr const char *kCurrentVersion = "0.2.1";
  // True for a version number below kCurrentVersion ("0.2.0", "0.1.0-preview.9"); false when the text does not start with one.
  bool
  outdated_version(const std::string &version);
  // Whether release `a` comes after release `b`: runs of digits are compared as numbers (0.10.0 after 0.2.0), the rest as it is spelled.
  bool
  later_release(const std::string &a, const std::string &b);
  std::vector<Pin>
  builtin_pins();
  json
  manifest_json();
  bool
  notice_recorded(const HostEnv &env);
  json
  install_status_fields(const HostEnv &env);
  Probes
  current_probes(const fs::path &dest);
  InstallOutcome
  install_from_file(HostEnv &env, InstallRequest req, const std::function<void(const Progress &)> &progress = {});
  InstallOutcome
  uninstall_component(const UninstallRequest &req);
  std::string
  start_install_job(HostEnv env, InstallRequest req);
  json
  install_job(const std::string &id);
}  // namespace sparxie

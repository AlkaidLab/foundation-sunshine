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

  std::vector<Pin>
  builtin_pins();
  // True when only administrators (SYSTEM, Administrators, TrustedInstaller) can change anything the programs under `root` are made of: every entry of the tree is
  // owned by one of them and grants no other principal a write, and no ancestor lets another principal rename or replace the folder. A network or removable drive, a
  // link, an entry that cannot be read and a tree too large to check all answer false; `blocker` then names the first path that failed.
  // `host_root` is the host's own install folder: for a tree inside it the ancestors are checked up to that folder and no further, because whoever could swap a folder
  // above it could swap the host's own programs, which already run as SYSTEM.
  bool
  host_admin_only_tree(const fs::path &root, fs::path *blocker = nullptr, const fs::path &host_root = {});
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

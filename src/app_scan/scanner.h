/**
 * @file src/app_scan/scanner.h
 * @brief Host-side game library discovery for the configuration API.
 *
 * The scanner reads the host's installed game libraries and reports launchers
 * and installation paths. It never executes a program and never writes to the
 * scanned locations. Platform data sources and the source keys that justify
 * them are recorded in docs/authority/source-index.tsv.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace app_scan {
  namespace fs = std::filesystem;

  enum class HostPlatform {
    windows_host,
    linux_host,
    macos_host,
  };

  enum class Source {
    steam,
    epic,
    gog,
    xbox,
    ea,
    ubisoft,
    battlenet,
    riot,
    hoyoplay,
    wegame,
    sparxie,
  };

  /// One way to start a game. `id` is a stable choice key (for example touch or pc).
  struct LaunchChoice {
    std::string id;
    std::string launch;
  };

  /// One library entry. Empty optional fields are omitted from the API response.
  /// `platform` is the preferred source. `sources` lists every library that had this install.
  struct Game {
    std::string platform;
    std::string name;
    std::string id;
    std::string install_dir;
    std::string launch;
    std::string exe;
    std::string cover_hint;
    std::vector<std::string> sources;
    std::string region;
    std::string profile;
    std::vector<LaunchChoice> launches;
  };

  /// A packaged app the host can see for the signed-in user. Tests supply a fake list.
  struct PackageRecord {
    std::string family_name;
    std::string app_id;
    std::string install_path;
    std::string display_name;
  };

  class PackageView {
  public:
    virtual ~PackageView() = default;
    virtual std::vector<PackageRecord>
    packages() const = 0;
  };

  struct DirectoryLimits {
    int max_depth = 24;
    std::chrono::milliseconds time_limit { 5000 };
  };

  struct ScanRequest {
    std::vector<Source> sources;
    std::optional<fs::path> directory;
    DirectoryLimits directory_limits;
  };

  /// Registry access is injected so the discovery rules can be tested without a
  /// Windows host. Keys and values are UTF-8; views expose the 32-bit registry
  /// view the reference scanner reads.
  class RegistryView {
  public:
    virtual ~RegistryView() = default;
    virtual std::optional<std::string>
    read_string(const std::string &key, const std::string &value) const = 0;
    virtual std::vector<std::string>
    subkeys(const std::string &key) const = 0;
    // The signed-in user's hive. The host service reads these while impersonating
    // that user; the default is empty so a machine-wide fake registry still compiles.
    virtual std::optional<std::string>
    read_user_string(const std::string &key, const std::string &value) const {
      (void) key;
      (void) value;
      return std::nullopt;
    }
    virtual std::vector<std::string>
    user_subkeys(const std::string &key) const {
      (void) key;
      return {};
    }
  };

  struct HostContext {
    HostPlatform platform = HostPlatform::linux_host;
    std::optional<fs::path> home;
    std::optional<fs::path> program_data;
    const RegistryView *registry = nullptr;
    // Per-user folders of the signed-in person, not the service account.
    std::optional<fs::path> local_app_data;
    std::optional<fs::path> roaming_app_data;
    // Drive roots to inspect for library markers. Empty on Windows asks the
    // scanner to enumerate the drives visible to the impersonated user.
    std::vector<fs::path> drive_roots;
    std::optional<fs::path> sparxie_exe;
    std::optional<fs::path> sparxie_data;
    std::optional<fs::path> covers_dir;
    std::vector<fs::path> package_roots;
    const PackageView *packages = nullptr;
  };

  struct ScanResult {
    std::vector<Game> games;
    std::vector<std::string> warnings;
  };

  // Parsed Steam app manifest (appmanifest_<id>.acf).
  struct SteamManifest {
    std::string app_id;
    std::string name;
    std::string install_dir_name;
    std::optional<std::uint64_t> size_on_disk;
  };

  // Parsed Epic Games launcher manifest (*.item).
  struct EpicManifest {
    std::string display_name;
    std::string app_name;
    std::string install_location;
    std::string launch_executable;
    std::optional<std::uint64_t> install_size;
  };

  HostPlatform
  detect_host_platform();
  std::unique_ptr<RegistryView>
  make_native_registry();

  const char *
  source_name(Source source);
  bool
  parse_source(std::string_view name, Source &source);

  std::string
  to_utf8(const fs::path &path);
  fs::path
  from_utf8(const std::string &text);

  // Parsers are exposed so the manifest fixtures can be checked directly.
  std::optional<SteamManifest>
  parse_steam_manifest(std::string_view content);
  std::vector<std::string>
  parse_library_folder_paths(std::string_view content);
  std::optional<EpicManifest>
  parse_epic_manifest(std::string_view content);

  // Heuristic port of the reference directory scanner. Paths may be native or
  // Windows style; separators are normalised before matching.
  bool
  detect_if_game(const std::string &file_path, const std::string &name, const std::optional<std::string> &target_path);

  // Returns the rejection reason for a directory scan request, or nullopt when
  // the root may be read. UNC and device namespaces are refused before any I/O.
  std::optional<std::string>
  validate_scan_root(const fs::path &root);

  // Replaces the user's home directory with '~' so logs carry no user name.
  std::string
  mask_user_path(const std::string &path, const std::optional<fs::path> &home);

  ScanResult
  scan(const ScanRequest &request, const HostContext &context);

  // Store libraries beyond Steam, Epic and GOG, and the merge of duplicates.
  void
  scan_libraries(const ScanRequest &request, const HostContext &context, ScanResult &result);
  void
  dedupe_games(std::vector<Game> &games);

  // Package full name grammar: Name_Version_Architecture_ResourceId_PublisherId.
  std::optional<std::string>
  package_family_from_full_name(std::string_view full_name);
}  // namespace app_scan

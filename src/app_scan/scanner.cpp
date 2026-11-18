/**
 * @file src/app_scan/scanner.cpp
 * @brief Host-side game library discovery for the configuration API.
 *
 * Discovery rules follow the existing panel scanner: Steam registry path and
 * libraryfolders.vdf, Epic launcher manifests, the GOG.com registry key and a
 * bounded directory walk. The directory walk adds depth, time and symlink
 * bounds so a remote configuration request cannot turn into an unbounded host
 * read. See docs/authority/source-index.tsv for the platform sources.
 */
#include "src/app_scan/scanner.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
  #include <objbase.h>
  #include <shlobj.h>
  #include <windows.h>
#endif

namespace app_scan {
  namespace {
    using json = nlohmann::json;

    constexpr std::string_view kSteamRegistryKey = "SOFTWARE\\WOW6432Node\\Valve\\Steam";
    constexpr std::string_view kGogRegistryKey = "SOFTWARE\\WOW6432Node\\GOG.com\\Games";
    constexpr std::string_view kSteamHeaderPrefix = "https://cdn.akamai.steamstatic.com/steam/apps/";

    std::string
    ascii_lower(std::string_view value) {
      std::string result(value);
      std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return result;
    }

    [[maybe_unused]] bool
    equals_ignore_case(std::string_view lhs, std::string_view rhs) {
      return ascii_lower(lhs) == ascii_lower(rhs);
    }

    std::string
    trimmed(std::string_view value) {
      size_t begin = 0;
      size_t end = value.size();
      while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
      }
      while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
      }
      return std::string(value.substr(begin, end - begin));
    }

    // VDF string values escape backslash and double quote. Other backslash
    // sequences are left untouched so a Windows path written with single
    // separators still round-trips.
    std::string
    vdf_unescape(std::string_view value) {
      std::string result;
      result.reserve(value.size());
      for (size_t index = 0; index < value.size(); ++index) {
        const char current = value[index];
        if (current == '\\' && index + 1 < value.size()) {
          const char next = value[index + 1];
          if (next == '\\' || next == '"') {
            result.push_back(next);
            ++index;
            continue;
          }
        }
        result.push_back(current);
      }
      return result;
    }

    std::optional<std::string>
    extract_vdf_value(std::string_view content, std::string_view key) {
      const std::string prefix = '"' + std::string(key) + '"';
      size_t offset = 0;
      while (offset <= content.size()) {
        const size_t line_end = content.find('\n', offset);
        const size_t end = line_end == std::string_view::npos ? content.size() : line_end;
        std::string line = trimmed(content.substr(offset, end - offset));
        if (line.starts_with(prefix)) {
          std::string rest = trimmed(std::string_view(line).substr(prefix.size()));
          size_t begin = 0;
          size_t finish = rest.size();
          while (begin < finish && rest[begin] == '"') {
            ++begin;
          }
          while (finish > begin && rest[finish - 1] == '"') {
            --finish;
          }
          std::string value = vdf_unescape(std::string_view(rest).substr(begin, finish - begin));
          if (!value.empty()) {
            return value;
          }
        }
        if (line_end == std::string_view::npos) {
          break;
        }
        offset = line_end + 1;
      }
      return std::nullopt;
    }

    std::optional<std::string>
    read_text_file(const fs::path &path) {
      std::ifstream file(path, std::ios::binary);
      if (!file) {
        return std::nullopt;
      }
      std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      if (file.bad()) {
        return std::nullopt;
      }
      return content;
    }

    bool
    manifest_name_excluded(const std::string &name) {
      const std::string lowered = ascii_lower(name);
      static constexpr std::array<std::string_view, 21> keywords {
        "redistributable",
        " sdk",
        "dedicated server",
        "proton ",
        "steam linux runtime",
        "steamworks",
        "directx",
        "vcredist",
        "visual c++",
        "common redist",
        "mod tool",
        "editor",
        "soundtrack",
        "ost",
        "artbook",
        "art book",
        "benchmark",
        "demo",
        " test",
        "developer tool",
        "devkit",
      };
      return std::any_of(keywords.begin(), keywords.end(),
        [&](std::string_view keyword) { return lowered.find(keyword) != std::string::npos; });
    }

    bool
    manifest_type_excluded(const std::string &type) {
      const std::string lowered = ascii_lower(type);
      return lowered == "tool" || lowered == "demo" || lowered == "music" ||
             lowered == "dlc" || lowered == "config" || lowered == "media";
    }

    bool
    is_tool_exe(const std::string &name_lower) {
      static constexpr std::array<std::string_view, 19> tools {
        "uninstall",
        "uninst",
        "setup",
        "install",
        "update",
        "updater",
        "crash",
        "reporter",
        "helper",
        "service",
        "launcher_helper",
        "redist",
        "vcredist",
        "dxsetup",
        "dotnet",
        "ue4prereq",
        "bootstrapper",
        "cleanup",
        "repair",
      };
      return std::any_of(tools.begin(), tools.end(),
        [&](std::string_view tool) { return name_lower.find(tool) != std::string::npos; });
    }

    std::optional<std::string>
    find_main_exe(const fs::path &install_dir) {
      std::error_code ec;
      if (!fs::is_directory(install_dir, ec)) {
        return std::nullopt;
      }

      std::vector<std::pair<fs::path, std::uintmax_t>> candidates;
      for (fs::directory_iterator it(install_dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry &entry = *it;
        if (!entry.is_regular_file(ec)) {
          continue;
        }
        if (ascii_lower(entry.path().extension().string()) != ".exe") {
          continue;
        }
        const std::string name_lower = ascii_lower(entry.path().filename().string());
        if (is_tool_exe(name_lower)) {
          continue;
        }
        std::error_code size_ec;
        const std::uintmax_t size = entry.file_size(size_ec);
        candidates.emplace_back(entry.path(), size_ec ? 0 : size);
      }
      if (candidates.empty()) {
        return std::nullopt;
      }
      std::stable_sort(candidates.begin(), candidates.end(),
        [](const auto &lhs, const auto &rhs) { return lhs.second > rhs.second; });
      return to_utf8(candidates.front().first);
    }

    bool
    directory_exists(const fs::path &path) {
      std::error_code ec;
      return fs::is_directory(path, ec);
    }

    bool
    regular_file_exists(const fs::path &path) {
      std::error_code ec;
      return fs::is_regular_file(path, ec);
    }

    std::optional<fs::path>
    find_steam_root(const HostContext &context) {
      if (context.platform == HostPlatform::windows_host) {
        // 32-bit Steam view: ms-win-registry-redirector and
        // ms-win-alternate-registry-view.
        if (context.registry) {
          const auto install_path = context.registry->read_string(std::string(kSteamRegistryKey), "InstallPath");
          if (install_path && !install_path->empty()) {
            fs::path candidate = from_utf8(*install_path);
            if (directory_exists(candidate)) {
              return candidate;
            }
          }
        }
        fs::path fallback = from_utf8("C:\\Program Files (x86)\\Steam");
        if (directory_exists(fallback)) {
          return fallback;
        }
        return std::nullopt;
      }

      if (!context.home) {
        return std::nullopt;
      }

      std::vector<fs::path> candidates;
      if (context.platform == HostPlatform::linux_host) {
        // freedesktop-xdg-basedir and flatpak-sandbox-permissions.
        candidates.push_back(*context.home / ".steam/steam");
        candidates.push_back(*context.home / ".local/share/Steam");
        candidates.push_back(*context.home / ".var/app/com.valvesoftware.Steam/.local/share/Steam");
      }
      else if (context.platform == HostPlatform::macos_host) {
        // apple-filesystem-overview.
        candidates.push_back(*context.home / "Library/Application Support/Steam");
      }

      for (const fs::path &candidate : candidates) {
        if (directory_exists(candidate)) {
          return candidate;
        }
      }
      return std::nullopt;
    }

    std::vector<fs::path>
    steam_library_folders(const fs::path &steam_root) {
      std::vector<fs::path> folders { steam_root };

      fs::path vdf_path = steam_root / "steamapps" / "libraryfolders.vdf";
      if (!regular_file_exists(vdf_path)) {
        fs::path legacy = steam_root / "config" / "libraryfolders.vdf";
        if (!regular_file_exists(legacy)) {
          return folders;
        }
        vdf_path = std::move(legacy);
      }

      const auto content = read_text_file(vdf_path);
      if (!content) {
        return folders;
      }

      for (const std::string &value : parse_library_folder_paths(*content)) {
        fs::path folder = from_utf8(value);
        if (!directory_exists(folder)) {
          continue;
        }
        if (std::find(folders.begin(), folders.end(), folder) == folders.end()) {
          folders.push_back(std::move(folder));
        }
      }
      return folders;
    }

    void
    scan_steam(const HostContext &context, ScanResult &result) {
      const auto steam_root = find_steam_root(context);
      if (!steam_root) {
        result.warnings.emplace_back("steam.not_found");
        return;
      }

      std::size_t found = 0;
      for (const fs::path &library : steam_library_folders(*steam_root)) {
        const fs::path steamapps = library / "steamapps";
        if (!directory_exists(steamapps)) {
          continue;
        }

        std::error_code ec;
        for (fs::directory_iterator it(steamapps, ec), end; !ec && it != end; it.increment(ec)) {
          const fs::directory_entry &entry = *it;
          if (!entry.is_regular_file(ec)) {
            continue;
          }
          const std::string filename = entry.path().filename().string();
          if (!filename.starts_with("appmanifest_") || !filename.ends_with(".acf")) {
            continue;
          }

          const auto content = read_text_file(entry.path());
          if (!content) {
            continue;
          }
          const auto manifest = parse_steam_manifest(*content);
          if (!manifest) {
            continue;
          }

          const fs::path install_dir = steamapps / "common" / manifest->install_dir_name;
          Game game;
          game.platform = "steam";
          game.name = manifest->name;
          game.id = manifest->app_id;
          game.install_dir = to_utf8(install_dir);
          game.launch = "steam://rungameid/" + manifest->app_id;
          game.cover_hint = std::string(kSteamHeaderPrefix) + manifest->app_id + "/header.jpg";
          if (const auto exe = find_main_exe(install_dir)) {
            game.exe = *exe;
          }
          result.games.push_back(std::move(game));
          ++found;
        }
      }

      if (found == 0) {
        result.warnings.emplace_back("steam.empty");
      }
    }

    void
    scan_epic(const HostContext &context, ScanResult &result) {
      if (context.platform != HostPlatform::windows_host) {
        result.warnings.emplace_back("epic.unsupported");
        return;
      }

      fs::path manifests = context.program_data ? *context.program_data : from_utf8("C:\\ProgramData");
      manifests /= "Epic";
      manifests /= "EpicGamesLauncher";
      manifests /= "Data";
      manifests /= "Manifests";
      if (!directory_exists(manifests)) {
        result.warnings.emplace_back("epic.not_found");
        return;
      }

      std::size_t found = 0;
      std::error_code ec;
      for (fs::directory_iterator it(manifests, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry &entry = *it;
        if (!entry.is_regular_file(ec) || entry.path().extension().string() != ".item") {
          continue;
        }
        const auto content = read_text_file(entry.path());
        if (!content) {
          continue;
        }
        const auto manifest = parse_epic_manifest(*content);
        if (!manifest) {
          continue;
        }

        const fs::path install_dir = from_utf8(manifest->install_location);
        Game game;
        game.platform = "epic";
        game.name = manifest->display_name;
        game.id = manifest->app_name;
        game.install_dir = manifest->install_location;
        game.launch = "com.epicgames.launcher://apps/" + manifest->app_name + "?action=launch&silent=true";
        game.exe = to_utf8(install_dir / from_utf8(manifest->launch_executable));
        result.games.push_back(std::move(game));
        ++found;
      }

      if (found == 0) {
        result.warnings.emplace_back("epic.empty");
      }
    }

    void
    scan_gog(const HostContext &context, ScanResult &result) {
      if (context.platform != HostPlatform::windows_host) {
        result.warnings.emplace_back("gog.unsupported");
        return;
      }
      if (!context.registry) {
        result.warnings.emplace_back("gog.unavailable");
        return;
      }

      const std::string games_key(kGogRegistryKey);
      const std::vector<std::string> ids = context.registry->subkeys(games_key);
      if (ids.empty()) {
        result.warnings.emplace_back("gog.not_found");
        return;
      }

      for (const std::string &id : ids) {
        const std::string key = games_key + '\\' + id;
        const auto name = context.registry->read_string(key, "gameName");
        const auto path = context.registry->read_string(key, "path");
        const auto exe = context.registry->read_string(key, "exe");
        if (!name || name->empty() || !path || path->empty()) {
          continue;
        }

        std::string exe_path = exe.value_or(std::string {});
        if (exe_path.empty()) {
          exe_path = find_main_exe(from_utf8(*path)).value_or(std::string {});
        }

        Game game;
        game.platform = "gog";
        game.name = *name;
        game.id = id;
        game.install_dir = *path;
        game.exe = exe_path;
        game.launch = exe_path.empty() ? "goggalaxy://openGameView/" + id : '"' + exe_path + '"';
        result.games.push_back(std::move(game));
      }
    }

    bool
    is_within(const fs::path &candidate, const fs::path &root) {
      auto candidate_it = candidate.begin();
      for (auto root_it = root.begin(); root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *candidate_it != *root_it) {
          return false;
        }
      }
      return true;
    }

#ifdef _WIN32
    // ms-win-shell-links and ms-win-ishelllink-getpath.
    std::optional<std::string>
    resolve_lnk_target(const fs::path &lnk_path) {
      const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
      const bool needs_uninitialize = SUCCEEDED(init);

      IShellLinkW *link = nullptr;
      IPersistFile *persist = nullptr;
      const auto release = [&]() {
        if (persist) {
          persist->Release();
          persist = nullptr;
        }
        if (link) {
          link->Release();
          link = nullptr;
        }
        if (needs_uninitialize) {
          CoUninitialize();
        }
      };

      std::optional<std::string> result;
      do {
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
              IID_IShellLinkW, reinterpret_cast<void **>(&link))) ||
            !link) {
          break;
        }
        if (FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&persist))) ||
            !persist) {
          break;
        }
        if (FAILED(persist->Load(lnk_path.wstring().c_str(), STGM_READ))) {
          break;
        }

        std::array<wchar_t, 4096> target {};
        WIN32_FIND_DATAW find_data {};
        if (FAILED(link->GetPath(target.data(), static_cast<int>(target.size()), &find_data, SLGP_RAWPATH))) {
          break;
        }
        const std::wstring_view target_view(target.data());
        if (target_view.empty()) {
          break;
        }
        result = to_utf8(fs::path(std::wstring(target_view)));
      } while (false);

      release();
      return result;
    }
#endif

    struct WalkedFile {
      std::string name;
      std::string launch;
      std::string working_dir;
      std::optional<std::string> target;
      std::string extension;
    };

    std::optional<WalkedFile>
    inspect_file(const fs::path &path, const std::string &extension) {
      WalkedFile file;
      file.extension = extension;

      const std::string stem = path.stem().string();
      const std::string native_path = to_utf8(path);

      if (extension == ".lnk") {
#ifdef _WIN32
        const auto target = resolve_lnk_target(path);
        if (!target) {
          return std::nullopt;
        }
        file.name = stem;
        file.launch = '"' + native_path + '"';
        file.target = *target;
        return file;
#else
        return std::nullopt;
#endif
      }

      const fs::path parent = path.parent_path();
      if (extension == ".exe") {
        file.name = stem;
        file.launch = '"' + native_path + '"';
        file.working_dir = to_utf8(parent);
        file.target = native_path;
        return file;
      }
      if (extension == ".bat" || extension == ".cmd") {
        file.name = stem;
        file.launch = "cmd /c \"" + native_path + "\"";
        file.working_dir = to_utf8(parent);
        return file;
      }
      if (extension == ".url") {
        file.name = stem;
        file.launch = "start \"\" \"" + native_path + "\"";
        return file;
      }
      return std::nullopt;
    }

    void
    scan_directory(const fs::path &root, const DirectoryLimits &limits, ScanResult &result) {
      struct Pending {
        fs::path path;
        int depth;
      };

      const auto start = std::chrono::steady_clock::now();
      const auto deadline = start + limits.time_limit;
      bool depth_warned = false;
      bool symlink_warned = false;
      bool time_warned = false;
      bool read_warned = false;

      const auto warn_once = [&](bool &flag, std::string_view message) {
        if (!flag) {
          flag = true;
          result.warnings.emplace_back(message);
        }
      };

      std::error_code ec;
      fs::path canonical_root = fs::weakly_canonical(root, ec);
      if (ec) {
        canonical_root = root;
      }

      std::vector<Pending> pending { Pending { root, 0 } };
      std::size_t examined = 0;
      while (!pending.empty()) {
        if (std::chrono::steady_clock::now() >= deadline) {
          warn_once(time_warned, "directory.time");
          break;
        }

        Pending current = std::move(pending.back());
        pending.pop_back();

        fs::directory_iterator it(current.path, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
          warn_once(read_warned, "directory.unreadable");
          continue;
        }

        for (const fs::directory_entry &entry : it) {
          if ((++examined & 0x3f) == 0 && std::chrono::steady_clock::now() >= deadline) {
            warn_once(time_warned, "directory: stopped after the scan time limit");
            pending.clear();
            break;
          }

          std::error_code entry_ec;
          if (entry.is_symlink(entry_ec)) {
            warn_once(symlink_warned, "directory.symlinks");
            continue;
          }

          if (entry.is_directory(entry_ec)) {
            const std::string name = entry.path().filename().string();
            if (name.starts_with('.') || equals_ignore_case(name, "$RECYCLE.BIN") ||
                equals_ignore_case(name, "System Volume Information")) {
              continue;
            }
            const int next_depth = current.depth + 1;
            if (next_depth > limits.max_depth) {
              warn_once(depth_warned, "directory.depth");
              continue;
            }
            // cpp-fs-symlink-status and cpp-fs-weakly-canonical keep the walk
            // inside the canonical root.
            std::error_code canonical_ec;
            const fs::path canonical = fs::weakly_canonical(entry.path(), canonical_ec);
            if (canonical_ec || !is_within(canonical, canonical_root)) {
              warn_once(symlink_warned, "directory: skipped symbolic links");
              continue;
            }
            pending.push_back(Pending { entry.path(), next_depth });
            continue;
          }

          if (!entry.is_regular_file(entry_ec)) {
            continue;
          }

          const std::string extension = ascii_lower(entry.path().extension().string());
          if (extension != ".lnk" && extension != ".exe" && extension != ".bat" &&
              extension != ".cmd" && extension != ".url") {
            continue;
          }

          const auto file = inspect_file(entry.path(), extension);
          if (!file) {
            continue;
          }
          const std::string source = to_utf8(entry.path());
          if (!detect_if_game(source, file->name, file->target)) {
            continue;
          }

          Game game;
          game.platform = "directory";
          game.name = file->name;
          game.id = source;
          game.install_dir = file->working_dir;
          game.launch = file->launch;
          if (file->extension == ".exe") {
            game.exe = source;
          }
          else if (file->target) {
            game.exe = *file->target;
          }
          result.games.push_back(std::move(game));
        }
      }
    }

    std::string
    normalised_path(const std::string &path) {
      std::string result = path;
      std::replace(result.begin(), result.end(), '/', '\\');
      return result;
    }

  }  // namespace

  HostPlatform
  detect_host_platform() {
#if defined(_WIN32)
    return HostPlatform::windows_host;
#elif defined(__APPLE__)
    return HostPlatform::macos_host;
#else
    return HostPlatform::linux_host;
#endif
  }

  std::unique_ptr<RegistryView>
  make_native_registry() {
#ifdef _WIN32
    class WindowsRegistryView final: public RegistryView {
    public:
      std::optional<std::string>
      read_string(const std::string &key, const std::string &value) const override {
        // ms-win-reggetvalue: two-call size query with RRF_RT_REG_SZ.
        const std::wstring wide_key = utf8_to_wide(key);
        const std::wstring wide_value = utf8_to_wide(value);
        DWORD type = 0;
        DWORD size = 0;
        LONG status = RegGetValueW(HKEY_LOCAL_MACHINE, wide_key.c_str(), wide_value.c_str(),
          RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, nullptr, &size);
        if (status != ERROR_SUCCESS || size < sizeof(wchar_t)) {
          return std::nullopt;
        }
        std::wstring buffer(size / sizeof(wchar_t), L'\0');
        status = RegGetValueW(HKEY_LOCAL_MACHINE, wide_key.c_str(), wide_value.c_str(),
          RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, buffer.data(), &size);
        if (status != ERROR_SUCCESS) {
          return std::nullopt;
        }
        const size_t length = buffer.find(L'\0');
        if (length == std::wstring::npos) {
          buffer.resize(size / sizeof(wchar_t));
        }
        else {
          buffer.resize(length);
        }
        if (buffer.empty()) {
          return std::nullopt;
        }
        return wide_to_utf8(buffer);
      }

      std::vector<std::string>
      subkeys(const std::string &key) const override {
        std::vector<std::string> result;
        HKEY handle = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, utf8_to_wide(key).c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) {
          return result;
        }
        for (DWORD index = 0;; ++index) {
          std::array<wchar_t, 256> name {};
          DWORD length = static_cast<DWORD>(name.size());
          const LONG status = RegEnumKeyExW(handle, index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
          if (status != ERROR_SUCCESS) {
            break;
          }
          result.emplace_back(wide_to_utf8(std::wstring(name.data(), length)));
        }
        RegCloseKey(handle);
        return result;
      }

      std::optional<std::string>
      read_user_string(const std::string &key, const std::string &value) const override {
        HKEY user = nullptr;
        if (RegOpenCurrentUser(KEY_READ, &user) != ERROR_SUCCESS) {
          return std::nullopt;
        }
        const auto result = read_at(user, key, value);
        RegCloseKey(user);
        return result;
      }

      std::vector<std::string>
      user_subkeys(const std::string &key) const override {
        std::vector<std::string> result;
        HKEY user = nullptr;
        if (RegOpenCurrentUser(KEY_READ, &user) != ERROR_SUCCESS) {
          return result;
        }
        HKEY handle = nullptr;
        if (RegOpenKeyExW(user, utf8_to_wide(key).c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) {
          RegCloseKey(user);
          return result;
        }
        for (DWORD index = 0;; ++index) {
          std::array<wchar_t, 256> name {};
          DWORD length = static_cast<DWORD>(name.size());
          const LONG status = RegEnumKeyExW(handle, index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
          if (status != ERROR_SUCCESS) {
            break;
          }
          result.emplace_back(wide_to_utf8(std::wstring(name.data(), length)));
        }
        RegCloseKey(handle);
        RegCloseKey(user);
        return result;
      }

    private:
      static std::optional<std::string>
      read_at(HKEY root, const std::string &key, const std::string &value) {
        const std::wstring wide_key = utf8_to_wide(key);
        const std::wstring wide_value = utf8_to_wide(value);
        DWORD type = 0;
        DWORD size = 0;
        LONG status = RegGetValueW(root, wide_key.c_str(), wide_value.c_str(),
          RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, nullptr, &size);
        if (status != ERROR_SUCCESS || size < sizeof(wchar_t)) {
          return std::nullopt;
        }
        std::wstring buffer(size / sizeof(wchar_t), L'\0');
        status = RegGetValueW(root, wide_key.c_str(), wide_value.c_str(),
          RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, buffer.data(), &size);
        if (status != ERROR_SUCCESS) {
          return std::nullopt;
        }
        const size_t length = buffer.find(L'\0');
        if (length == std::wstring::npos) {
          buffer.resize(size / sizeof(wchar_t));
        }
        else {
          buffer.resize(length);
        }
        if (buffer.empty()) {
          return std::nullopt;
        }
        return wide_to_utf8(buffer);
      }

      static std::wstring
      utf8_to_wide(const std::string &input) {
        if (input.empty()) {
          return {};
        }
        const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
          static_cast<int>(input.size()), nullptr, 0);
        if (needed <= 0) {
          return {};
        }
        std::wstring output(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
          static_cast<int>(input.size()), output.data(), needed);
        return output;
      }

      static std::string
      wide_to_utf8(const std::wstring &input) {
        if (input.empty()) {
          return {};
        }
        const int needed = WideCharToMultiByte(CP_UTF8, 0, input.data(),
          static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
        if (needed <= 0) {
          return {};
        }
        std::string output(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
          output.data(), needed, nullptr, nullptr);
        return output;
      }
    };

    return std::make_unique<WindowsRegistryView>();
#else
    return nullptr;
#endif
  }

  const char *
  source_name(Source source) {
    switch (source) {
      case Source::steam:
        return "steam";
      case Source::epic:
        return "epic";
      case Source::gog:
        return "gog";
      case Source::xbox:
        return "xbox";
      case Source::ea:
        return "ea";
      case Source::ubisoft:
        return "ubisoft";
      case Source::battlenet:
        return "battlenet";
      case Source::riot:
        return "riot";
      case Source::hoyoplay:
        return "hoyoplay";
      case Source::wegame:
        return "wegame";
      case Source::sparxie:
        return "sparxie";
    }
    return "unknown";
  }

  bool
  parse_source(std::string_view name, Source &source) {
    if (name == "steam") {
      source = Source::steam;
      return true;
    }
    if (name == "epic") {
      source = Source::epic;
      return true;
    }
    if (name == "gog") {
      source = Source::gog;
      return true;
    }
    if (name == "xbox") {
      source = Source::xbox;
      return true;
    }
    if (name == "ea") {
      source = Source::ea;
      return true;
    }
    if (name == "ubisoft") {
      source = Source::ubisoft;
      return true;
    }
    if (name == "battlenet") {
      source = Source::battlenet;
      return true;
    }
    if (name == "riot") {
      source = Source::riot;
      return true;
    }
    if (name == "hoyoplay") {
      source = Source::hoyoplay;
      return true;
    }
    if (name == "wegame") {
      source = Source::wegame;
      return true;
    }
    if (name == "sparxie") {
      source = Source::sparxie;
      return true;
    }
    return false;
  }

  std::string
  to_utf8(const fs::path &path) {
#ifdef _WIN32
    const std::u8string value = path.u8string();
    return std::string(value.begin(), value.end());
#else
    return path.string();
#endif
  }

  fs::path
  from_utf8(const std::string &text) {
#ifdef _WIN32
    return fs::path(std::u8string(text.begin(), text.end()));
#else
    return fs::path(text);
#endif
  }

  std::optional<SteamManifest>
  parse_steam_manifest(std::string_view content) {
    const auto app_id = extract_vdf_value(content, "appid");
    const auto name = extract_vdf_value(content, "name");
    const auto install_dir = extract_vdf_value(content, "installdir");
    if (!app_id || !name || !install_dir) {
      return std::nullopt;
    }

    std::uint64_t numeric_id = 0;
    const auto parsed = std::from_chars(app_id->data(), app_id->data() + app_id->size(), numeric_id);
    if (parsed.ec != std::errc {} || parsed.ptr != app_id->data() + app_id->size()) {
      numeric_id = 0;
    }
    if (numeric_id < 10) {
      return std::nullopt;
    }

    if (manifest_name_excluded(*name)) {
      return std::nullopt;
    }

    const std::string app_type = extract_vdf_value(content, "apptype").value_or(std::string {});
    if (manifest_type_excluded(app_type)) {
      return std::nullopt;
    }

    SteamManifest manifest;
    manifest.app_id = *app_id;
    manifest.name = *name;
    manifest.install_dir_name = *install_dir;
    if (const auto size = extract_vdf_value(content, "SizeOnDisk")) {
      std::uint64_t value = 0;
      const auto size_parsed = std::from_chars(size->data(), size->data() + size->size(), value);
      if (size_parsed.ec == std::errc {} && size_parsed.ptr == size->data() + size->size()) {
        manifest.size_on_disk = value;
      }
    }
    return manifest;
  }

  std::vector<std::string>
  parse_library_folder_paths(std::string_view content) {
    std::vector<std::string> paths;
    size_t offset = 0;
    while (offset <= content.size()) {
      const size_t line_end = content.find('\n', offset);
      const size_t end = line_end == std::string_view::npos ? content.size() : line_end;
      const std::string line = trimmed(content.substr(offset, end - offset));
      if (line.starts_with("\"path\"")) {
        std::string rest = trimmed(std::string_view(line).substr(6));
        size_t begin = 0;
        size_t finish = rest.size();
        while (begin < finish && rest[begin] == '"') {
          ++begin;
        }
        while (finish > begin && rest[finish - 1] == '"') {
          --finish;
        }
        std::string value = vdf_unescape(std::string_view(rest).substr(begin, finish - begin));
        if (!value.empty()) {
          paths.push_back(std::move(value));
        }
      }
      if (line_end == std::string_view::npos) {
        break;
      }
      offset = line_end + 1;
    }
    return paths;
  }

  std::optional<EpicManifest>
  parse_epic_manifest(std::string_view content) {
    // nlohmann-json-parse: a discarded value means malformed input.
    const json document = json::parse(content.begin(), content.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
      return std::nullopt;
    }

    const auto string_field = [&](const char *name) -> std::optional<std::string> {
      const auto it = document.find(name);
      if (it == document.end() || !it->is_string()) {
        return std::nullopt;
      }
      return it->get<std::string>();
    };

    const auto display_name = string_field("DisplayName");
    const auto app_name = string_field("AppName");
    const auto install_location = string_field("InstallLocation");
    const auto launch_executable = string_field("LaunchExecutable");
    if (!display_name || !app_name || !install_location || !launch_executable) {
      return std::nullopt;
    }

    EpicManifest manifest;
    manifest.display_name = *display_name;
    manifest.app_name = *app_name;
    manifest.install_location = *install_location;
    manifest.launch_executable = *launch_executable;
    const auto size = document.find("InstallSize");
    if (size != document.end() && size->is_number_unsigned()) {
      manifest.install_size = size->get<std::uint64_t>();
    }
    return manifest;
  }

  bool
  detect_if_game(const std::string &file_path, const std::string &name, const std::optional<std::string> &target_path) {
    const std::string path_lower = ascii_lower(normalised_path(file_path));
    const std::string name_lower = ascii_lower(name);
    const std::string target_lower = target_path ? ascii_lower(normalised_path(*target_path)) : std::string {};

    const bool is_exe = path_lower.ends_with(".exe") || target_lower.ends_with(".exe") ||
                        (path_lower.ends_with(".lnk") && target_lower.ends_with(".exe"));
    if (!is_exe && !path_lower.ends_with(".lnk")) {
      return false;
    }
    if (path_lower.ends_with(".lnk") && !target_lower.empty() && !target_lower.ends_with(".exe")) {
      return false;
    }

    static constexpr std::array<std::string_view, 93> exclude_keywords {
      "uninstall",
      "卸载",
      "setup",
      "安装",
      "installer",
      "update",
      "更新",
      "updater",
      "patch",
      "config",
      "配置",
      "settings",
      "设置",
      "crash",
      "崩溃",
      "reporter",
      "report",
      "helper",
      "service",
      "daemon",
      "redist",
      "redistributable",
      "vcredist",
      "directx",
      "launcher_helper",
      "bootstrapper",
      "ue4prereqsetup",
      "dxsetup",
      "dotnet",
      "chrome",
      "firefox",
      "edge",
      "opera",
      "brave",
      "word",
      "excel",
      "powerpoint",
      "outlook",
      "onenote",
      "access",
      "visual studio",
      "vscode",
      "code",
      "notepad",
      "sublime",
      "git",
      "node",
      "python",
      "java",
      "ruby",
      "adobe",
      "photoshop",
      "illustrator",
      "premiere",
      "after effects",
      "spotify",
      "discord",
      "telegram",
      "wechat",
      "微信",
      "qq",
      "obs",
      "vlc",
      "potplayer",
      "media player",
      "7-zip",
      "winrar",
      "bandizip",
      "driver",
      "nvidia",
      "amd ",
      "intel",
      "antivirus",
      "defender",
      "kaspersky",
      "avast",
      "office",
      "onedrive",
      "teams",
      "terminal",
      "powershell",
      "cmd",
      "control panel",
      "控制面板",
      "explorer",
      "task manager",
      "任务管理器",
      "calculator",
      "计算器",
      "paint",
      "画图",
      "snipping",
      "截图",
    };

    for (std::string_view keyword : exclude_keywords) {
      const std::string suffix = "\\" + std::string(keyword) + ".exe";
      if (name_lower.find(keyword) != std::string::npos || path_lower.ends_with(suffix)) {
        return false;
      }
    }

    static constexpr std::array<std::string_view, 12> high_confidence {
      "\\steamapps\\common\\",
      "\\steam\\steamapps\\common\\",
      "\\epic games\\",
      "\\gog galaxy\\games\\",
      "\\gog games\\",
      "\\ubisoft\\ubisoft game launcher\\games\\",
      "\\origin games\\",
      "\\ea games\\",
      "\\battle.net\\",
      "\\riot games\\",
      "\\xbox games\\",
      "\\playnite\\",
    };
    for (std::string_view keyword : high_confidence) {
      if (path_lower.find(keyword) != std::string::npos || target_lower.find(keyword) != std::string::npos) {
        return true;
      }
    }

    static constexpr std::array<std::string_view, 4> medium_confidence {
      "\\program files\\games\\",
      "\\program files\\game\\",
      "\\program files (x86)\\games\\",
      "\\program files (x86)\\game\\",
    };
    for (std::string_view keyword : medium_confidence) {
      if (path_lower.find(keyword) != std::string::npos || target_lower.find(keyword) != std::string::npos) {
        static constexpr std::array<std::string_view, 8> tool_indicators {
          "tool",
          "editor",
          "sdk",
          "dev",
          "debug",
          "server",
          "manager",
          "launcher",
        };
        const bool is_tool = std::any_of(tool_indicators.begin(), tool_indicators.end(),
          [&](std::string_view indicator) { return name_lower.find(indicator) != std::string::npos; });
        if (!is_tool) {
          return true;
        }
      }
    }

    if (path_lower.find("\\start menu\\programs\\games\\") != std::string::npos ||
        path_lower.find("\\开始菜单\\程序\\游戏\\") != std::string::npos) {
      return true;
    }

    return false;
  }

  std::optional<std::string>
  validate_scan_root(const fs::path &root) {
    const std::string text = to_utf8(root);
    if (text.empty()) {
      return "directory path is empty";
    }
    // ms-win-naming-files: UNC names and the \\.\ and \\?\ namespaces.
    const std::string lowered = ascii_lower(text);
    if (lowered.starts_with("\\\\.\\") || lowered.starts_with("\\\\?\\") ||
        lowered.starts_with("\\device\\") || lowered.starts_with("\\??\\")) {
      return "device paths are not allowed";
    }
    if (lowered == "/dev" || lowered.starts_with("/dev/") ||
        lowered == "/proc" || lowered.starts_with("/proc/") ||
        lowered == "/sys" || lowered.starts_with("/sys/")) {
      return "device paths are not allowed";
    }
    if (text.starts_with("\\\\") || text.starts_with("//")) {
      return "UNC paths are not allowed";
    }

    std::error_code ec;
    if (!fs::exists(root, ec)) {
      return "directory does not exist";
    }
    if (!fs::is_directory(root, ec)) {
      return "path is not a directory";
    }
    return std::nullopt;
  }

  std::string
  mask_user_path(const std::string &path, const std::optional<fs::path> &home) {
    if (!home || path.empty()) {
      return path;
    }
    const std::string home_text = to_utf8(*home);
    if (home_text.empty() || path.size() < home_text.size()) {
      return path;
    }

    const bool matches = [&]() {
      const std::string prefix = path.substr(0, home_text.size());
#ifdef _WIN32
      return equals_ignore_case(prefix, home_text);
#else
      return prefix == home_text;
#endif
    }();
    if (!matches) {
      return path;
    }
    if (path.size() == home_text.size()) {
      return "~";
    }
    const char separator = path[home_text.size()];
    if (separator != '/' && separator != '\\') {
      return path;
    }
    return "~" + path.substr(home_text.size());
  }

  ScanResult
  scan(const ScanRequest &request, const HostContext &context) {
    ScanResult result;

    const bool has_steam = std::find(request.sources.begin(), request.sources.end(), Source::steam) != request.sources.end();
    const bool has_epic = std::find(request.sources.begin(), request.sources.end(), Source::epic) != request.sources.end();
    const bool has_gog = std::find(request.sources.begin(), request.sources.end(), Source::gog) != request.sources.end();

    if (has_steam) {
      scan_steam(context, result);
    }
    if (has_epic) {
      scan_epic(context, result);
    }
    if (has_gog) {
      scan_gog(context, result);
    }
    if (request.directory) {
      scan_directory(*request.directory, request.directory_limits, result);
    }
    scan_libraries(request, context, result);
    dedupe_games(result.games);
    return result;
  }
}  // namespace app_scan

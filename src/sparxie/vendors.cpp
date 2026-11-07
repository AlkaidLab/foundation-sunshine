#include "src/sparxie/vendors.h"

#include "src/sparxie/region.h"

#include <algorithm>
#include <fstream>

namespace sparxie {
  namespace {
    bool
    file_exists(const fs::path &path) {
      std::error_code ec;
      return fs::is_regular_file(path, ec);
    }

    bool
    dir_exists(const fs::path &path) {
      std::error_code ec;
      return fs::is_directory(path, ec);
    }

    std::vector<std::string>
    dir_names(const fs::path &path) {
      std::vector<std::string> names;
      std::error_code ec;
      for (const auto &entry : fs::directory_iterator(path, ec))
        names.push_back(ascii_lower(entry.path().filename().string()));
      return names;
    }

    bool
    has_name(const std::vector<std::string> &names, std::string_view prefix) {
      for (const auto &name : names)
        if (name.compare(0, prefix.size(), prefix) == 0) return true;
      return false;
    }

    std::string
    read_all(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) return {};
      return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // The appId value of Kuro's launcherDownloadConfig.json. The file also
    // tracks the installed packs, so its presence is the launcher's own
    // record of the install.
    std::string
    wuwa_app_id(const fs::path &root) {
      const std::string text = read_all(root / "launcherDownloadConfig.json");
      if (text.empty()) return {};
      const auto at = text.find("\"appId\"");
      if (at == std::string::npos) return {};
      const auto colon = text.find(':', at);
      const auto quote = text.find('"', colon + 1);
      if (colon == std::string::npos || quote == std::string::npos) return {};
      const auto end = text.find('"', quote + 1);
      if (end == std::string::npos) return {};
      return text.substr(quote + 1, end - quote - 1);
    }

    // [UPDATE_CONFIG] Branch of NTE's launcher Config.ini (publish_ob is the
    // official channel, publish_ob_B the bilibili one).
    std::string
    nte_branch(const fs::path &config) {
      const std::string text = read_all(config);
      if (text.empty()) return {};
      bool update_config = false;
      std::size_t at = 0;
      while (at < text.size()) {
        const auto eol = text.find('\n', at);
        const std::string line = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        at = eol == std::string::npos ? text.size() : eol + 1;
        std::string trimmed = line;
        while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == ' ')) trimmed.pop_back();
        if (!trimmed.empty() && trimmed.front() == '[') {
          update_config = ascii_lower(trimmed).find("[update_config]") != std::string::npos;
          continue;
        }
        if (!update_config) continue;
        const auto eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = ascii_lower(trimmed.substr(0, eq));
        if (key == "branch") return ascii_lower(trimmed.substr(eq + 1));
      }
      return {};
    }

    // The tag of the Yostar launcher's game-launcher-config.json (for example
    // Arknights_EN), or the folder name when the file is missing.
    std::string
    arknights_tag(const fs::path &root) {
      const std::string text = read_all(root / "game-launcher-config.json");
      const auto at = text.find("\"tag\"");
      if (at != std::string::npos) {
        const auto colon = text.find(':', at);
        const auto quote = colon == std::string::npos ? std::string::npos : text.find('"', colon + 1);
        const auto end = quote == std::string::npos ? std::string::npos : text.find('"', quote + 1);
        if (quote != std::string::npos && end != std::string::npos) return ascii_lower(text.substr(quote + 1, end - quote - 1));
      }
      return ascii_lower(root.filename().string());
    }

    void
    narrow(VendorAnswer &answer, const char *region) {
      if (region) answer.region = region;
    }
  }  // namespace

  VendorAnswer
  wuwa_vendor(const fs::path &root) {
    VendorAnswer answer;
    const std::string app = wuwa_app_id(root);
    if (!app.empty()) {
      answer.launcher_record = true;
      answer.markers.push_back("launcher-config");
      if (app == "10003")
        narrow(answer, "cn");
      else if (app == "10004")
        narrow(answer, "bili");
      else if (app == "50004")
        narrow(answer, "global");
      return answer;
    }
    const fs::path third_party = root / "Client" / "Binaries" / "Win64" / "ThirdParty";
    const bool mainland = dir_exists(third_party / "KrPcSdk_Mainland");
    const bool global = dir_exists(third_party / "KrPcSdk_Global");
    if (mainland && dir_exists(third_party / "KrPcSdk_Mainland" / "KRSDKRes" / "Bilibili")) {
      answer.markers.push_back("sdk");
      narrow(answer, "bili");
    }
    else if (global && !mainland) {
      answer.markers.push_back("sdk");
      narrow(answer, "global");
    }
    else if (mainland && !global) {
      answer.markers.push_back("sdk");
      narrow(answer, "cn");
    }
    return answer;
  }

  VendorAnswer
  nte_vendor(const fs::path &root) {
    VendorAnswer answer;
    if (dir_exists(root / "NTEGlobal")) {
      answer.launcher_record = true;
      answer.markers.push_back("launcher-config");
      narrow(answer, "global");
      return answer;
    }
    if (dir_exists(root / "NTELauncher")) {
      answer.launcher_record = true;
      answer.markers.push_back("launcher-config");
      const std::string branch = nte_branch(root / "NTELauncher" / "Config" / "Config.ini");
      if (branch.find("_b") != std::string::npos)
        narrow(answer, "bili");
      else
        narrow(answer, "cn");
    }
    return answer;
  }

  VendorAnswer
  arknights_vendor(const fs::path &root) {
    VendorAnswer answer;
    const std::string tag = arknights_tag(root);
    const bool record = file_exists(root / "game-launcher-config.json");
    if (tag.find("arknights_en") != std::string::npos || tag.find("arknights_jp") != std::string::npos || tag.find("arknights_kr") != std::string::npos) {
      if (record) answer.markers.push_back("launcher-config");
      answer.launcher_record = record;
      narrow(answer, "global");
    }
    return answer;
  }

  VendorAnswer
  endfield_vendor(const fs::path &root) {
    VendorAnswer answer;
    std::vector<std::string> sdk;
    if (dir_exists(root / "sdkdata")) {
      answer.launcher_record = true;
      answer.markers.push_back("launcher-config");
      sdk = dir_names(root / "sdkdata");
    }
    const bool bilibili = file_exists(root / "PCGameSDK.dll") || has_name(sdk, "blplatform64");
    const bool cn = has_name(sdk, "sdk_hgsdk_config");
    const bool global = has_name(sdk, "sdk_glsdk_config");
    if (bilibili && !cn && !global)
      narrow(answer, "bili");
    else if (cn && !global)
      narrow(answer, "cn");
    else if (global && !cn)
      narrow(answer, "global");
    return answer;
  }

  fs::path
  hypergryph_launcher_exe(const std::vector<fs::path> &roots) {
    for (const fs::path &base : roots) {
      std::vector<fs::path> versions;
      std::error_code ec;
      for (const auto &entry : fs::directory_iterator(base, ec))
        if (entry.is_directory()) versions.push_back(entry.path());
      std::sort(versions.begin(), versions.end(), [](const fs::path &a, const fs::path &b) { return a.filename().string() > b.filename().string(); });
      for (const fs::path &version : versions) {
        const fs::path exe = version / "Launcher.exe";
        if (file_exists(exe)) return exe;
      }
      // A launcher root without version folders still holds Launcher.exe.
      const fs::path flat = base / "Launcher.exe";
      if (file_exists(flat)) return flat;
    }
    return {};
  }

  namespace {
    // The uninstall records of the official Hypergryph launchers: the key
    // names are the launchers' own and InstallLocation names the launcher
    // folder (docs/sparxie-games.md, launcher uninstall keys).
    const std::vector<std::pair<const char *, const char *>> &
    hypergryph_launcher_table() {
      static const std::vector<std::pair<const char *, const char *>> table = {
        { "33a0a6296a20400d503c59ac0fd6341e", "cn" },
        { "d9f71f86325c5b9733268efda40413c8", "global" },
        { "d957df99efbdf610162fd47c00ceaba7", "bili" },
      };
      return table;
    }
  }  // namespace

  std::vector<std::string>
  hypergryph_uninstall_keys() {
    std::vector<std::string> keys;
    for (const auto &row : hypergryph_launcher_table()) keys.emplace_back(row.first);
    return keys;
  }

  std::vector<LauncherInstall>
  hypergryph_launchers_from_records(const std::vector<fs::path> &locations) {
    std::vector<LauncherInstall> out;
    for (std::size_t i = 0; i < locations.size() && i < hypergryph_launcher_table().size(); ++i) {
      std::error_code ec;
      if (locations[i].empty() || !fs::is_directory(locations[i], ec)) continue;
      bool known = false;
      for (const auto &launcher : out)
        if (paths_equal(launcher.dir, locations[i])) known = true;
      if (!known) out.push_back({ locations[i], hypergryph_launcher_table()[i].second });
    }
    return out;
  }

  std::vector<LauncherGame>
  hypergryph_launcher_games(const std::vector<LauncherInstall> &launchers) {
    // Every game sits one level below the launcher's games folder (games\
    // Endfield Game\Endfield.exe, games\Arknights Endfield\Endfield.exe,
    // games\Arknights\Arknights.exe), so each games folder is listed once
    // and every known game executable directly inside a title folder is a row.
    std::vector<LauncherGame> out;
    for (const auto &launcher : launchers) {
      std::error_code ec;
      for (const auto &entry : fs::directory_iterator(launcher.dir, ec)) {
        if (!entry.is_directory(ec)) continue;
        if (ascii_lower(entry.path().filename().string()) != "games") continue;
        for (const auto &title : fs::directory_iterator(entry.path(), ec)) {
          if (!title.is_directory(ec)) continue;
          for (const char *exe : { "Endfield.exe", "Arknights.exe" }) {
            if (!fs::is_regular_file(title.path() / exe, ec)) continue;
            bool known = false;
            for (const auto &row : out)
              if (paths_equal(row.dir, title.path())) known = true;
            if (!known) out.push_back({ game_of_executable(exe), title.path(), launcher.channel });
          }
        }
      }
    }
    return out;
  }
}  // namespace sparxie

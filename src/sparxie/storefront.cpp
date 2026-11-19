#include "src/sparxie/storefront.h"

#include "src/sparxie/region.h"

#include <algorithm>
#include <fstream>

namespace sparxie {
  namespace {
    std::string
    read_text(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) return {};
      return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    bool
    is_space(unsigned char c) {
      return c == ' ' || c == 9 || c == 10 || c == 13;
    }

    struct VdfCursor {
      const char *at;
      const char *end;

      void
      skip_space() {
        while (at < end) {
          if (is_space(static_cast<unsigned char>(*at))) {
            ++at;
            continue;
          }
          if (at + 1 < end && at[0] == '/' && at[1] == '/') {
            while (at < end && *at != 10) ++at;
            continue;
          }
          break;
        }
      }

      // One quoted (with escape sequences) or bare token, or {}.
      std::optional<std::string>
      token() {
        skip_space();
        if (at >= end) return std::nullopt;
        if (*at == '{' || *at == '}') return std::string(1, *at++);
        std::string out;
        if (*at == '"') {
          ++at;
          while (at < end && *at != '"') {
            char c = *at++;
            if (c == '\\' && at < end) {
              const char next = *at++;
              if (next == 'n')
                c = 10;
              else if (next == 't')
                c = 9;
              else if (next == 'r')
                c = 13;
              else
                c = next;  // \\ and \" and anything else keep the letter
            }
            out.push_back(c);
          }
          if (at < end && *at == '"') ++at;
          return out;
        }
        while (at < end && !is_space(static_cast<unsigned char>(*at)) && *at != '{' && *at != '}' && *at != '"') out.push_back(*at++);
        if (out.empty()) return std::nullopt;
        return out;
      }
    };

    bool
    parse_node(VdfCursor &cursor, VdfNode &out);

    // The children of a block; the cursor sits just after its opening brace.
    bool
    parse_block(VdfCursor &cursor, VdfNode &out) {
      while (true) {
        cursor.skip_space();
        if (cursor.at >= cursor.end) return false;
        if (*cursor.at == '}') {
          ++cursor.at;
          return true;
        }
        const auto child_key = cursor.token();
        if (!child_key || *child_key == "{" || *child_key == "}") return false;
        VdfNode child;
        if (!parse_node(cursor, child)) return false;
        out.children.emplace_back(*child_key, std::move(child));
      }
    }

    bool
    parse_node(VdfCursor &cursor, VdfNode &out) {
      const auto value = cursor.token();
      if (!value || *value == "}") return false;
      if (*value == "{") return parse_block(cursor, out);
      out.value = *value;
      return true;
    }

    void
    add_once(std::vector<fs::path> &list, const fs::path &path) {
      for (const auto &known : list)
        if (paths_equal(known, path)) return;
      list.push_back(path);
    }

    void
    add_once(std::vector<EpicRecord> &list, const EpicRecord &record) {
      for (const auto &known : list)
        if (paths_equal(known.root, record.root)) return;
      list.push_back(record);
    }
  }  // namespace

  std::optional<VdfNode>
  parse_vdf(std::string_view text) {
    VdfCursor cursor { text.data(), text.data() + text.size() };
    const auto key = cursor.token();
    if (!key || *key == "{" || *key == "}") return std::nullopt;
    VdfNode root;
    if (!parse_node(cursor, root)) return std::nullopt;
    cursor.skip_space();
    if (cursor.at != cursor.end) return std::nullopt;
    return root;
  }

  std::vector<fs::path>
  steam_libraries(const fs::path &steam_root) {
    std::vector<fs::path> list;
    if (steam_root.empty()) return list;
    add_once(list, steam_root);
    const std::string text = read_text(steam_root / "steamapps" / "libraryfolders.vdf");
    if (text.empty()) return list;
    const auto parsed = parse_vdf(text);
    if (!parsed) return list;
    for (const auto &entry : parsed->children) {
      const VdfNode *path = entry.second.find("path");
      if (!path || path->value.empty()) continue;
      add_once(list, fs::path(path->value));
    }
    return list;
  }

  std::optional<fs::path>
  steam_install_dir(const fs::path &steam_root, int app_id) {
    for (const auto &library : steam_libraries(steam_root)) {
      const fs::path manifest = library / "steamapps" / ("appmanifest_" + std::to_string(app_id) + ".acf");
      const auto parsed = parse_vdf(read_text(manifest));
      if (!parsed) continue;
      const VdfNode *dir = parsed->find("installdir");
      if (!dir || dir->value.empty()) continue;
      const fs::path install = library / "steamapps" / "common" / dir->value;
      std::error_code ec;
      if (fs::is_directory(install, ec)) return install;
    }
    return std::nullopt;
  }

  std::vector<fs::path>
  epic_install_roots(const fs::path &manifests_dir) {
    std::vector<fs::path> roots;
    for (const auto &record : epic_records(manifests_dir))
      add_once(roots, record.root);
    return roots;
  }

  std::vector<EpicRecord>
  epic_records(const fs::path &manifests_dir) {
    std::vector<EpicRecord> records;
    if (manifests_dir.empty()) return records;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(manifests_dir, ec)) {
      if (!entry.is_regular_file(ec)) continue;
      if (ascii_lower(entry.path().extension().string()) != ".item") continue;
      json document;
      try {
        document = json::parse(read_text(entry.path()));
      }
      catch (const std::exception &) {
        continue;
      }
      if (!document.is_object()) continue;
      const std::string location = document.value("InstallLocation", std::string());
      if (location.empty()) continue;
      EpicRecord record;
      record.root = fs::path(location);
      record.app = document.value("AppName", std::string());
      add_once(records, record);
    }
    return records;
  }

  // The known game executables of one game inside a store folder, at most
  // three levels down (Wuthering Waves keeps its executable under
  // Client\Binaries\Win64).
  std::vector<StorefrontInstall>
  store_exes(const std::string &source, const std::string &game, const fs::path &dir, const std::string &app = {}) {
    std::vector<StorefrontInstall> found;
    std::error_code ec;
    for (const auto &file : fs::directory_iterator(dir, ec)) {
      if (!file.is_regular_file(ec)) continue;
      const std::string name = file.path().filename().string();
      if (game_of_executable(name) != game) continue;
      found.push_back({ source, game, "global", dir, name, app });
    }
    if (!found.empty()) return found;
    ScanOptions options;
    options.max_depth = 3;
    options.limit = std::chrono::milliseconds(1500);
    options.max_folders = 2000;
    const ScanResult scanned = scan_for_games(dir, options);
    for (const auto &match : scanned.matches) {
      const std::string name = match.exe.filename().string();
      if (game_of_executable(name) != game) continue;
      found.push_back({ source, game, "global", dir, name, app });
    }
    return found;
  }

  int
  steam_app_id(std::string_view game) {
    if (game == "zzz") return kSteamZzzAppId;
    if (game == "hi3") return 1671200;  // Honkai Impact 3rd, 2021-10-21
    if (game == "wuwa") return 3513350;  // Wuthering Waves, 2025-04-28
    if (game == "pgr") return 4125930;  // Punishing: Gray Raven, 2026-02-02
    if (game == "endfield") return 4732690;  // Arknights: Endfield, 2026-10-14
    if (game == "nte") return 4508340;  // NTE: Neverness to Everness, 2026-07-08
    return 0;
  }

  std::vector<StorefrontInstall>
  find_storefront_installs(const StorefrontEnv &env) {
    std::vector<StorefrontInstall> found;
    // Steam: every game the store holds. The store builds are the
    // international clients, and their executables keep the plain names.
    for (const auto game : kGames) {
      const int id = steam_app_id(game);
      if (!id) continue;
      if (const auto dir = steam_install_dir(env.steam_root, id)) {
        for (const auto &install : store_exes("steam", std::string(game), *dir)) found.push_back(install);
      }
    }
    // Epic: every manifest whose InstallLocation holds a known game
    // executable, at most three levels down, with the AppName the store's
    // own launch link needs.
    for (const auto &record : epic_records(env.epic_manifests)) {
      ScanOptions options;
      options.max_depth = 3;
      options.limit = std::chrono::milliseconds(1500);
      options.max_folders = 2000;
      const ScanResult scanned = scan_for_games(record.root, options);
      for (const auto &match : scanned.matches) {
        const std::string name = match.exe.filename().string();
        const std::string game = game_of_executable(name);
        if (game.empty()) continue;
        found.push_back({ "epic", game, "global", match.exe.parent_path(), name, record.app });
      }
    }
    return found;
  }

  bool
  dir_at_or_below(const fs::path &dir, const fs::path &root) {
    if (root.empty()) return false;
    const std::string base = norm_path(root);
    const std::string text = norm_path(dir);
    if (text == base) return true;
    return text.rfind(base + "/", 0) == 0;
  }
}  // namespace sparxie

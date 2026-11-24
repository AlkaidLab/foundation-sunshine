#include "src/sparxie/scan.h"
#include "src/sparxie/region.h"
#include <algorithm>

namespace sparxie {
  namespace {
    bool
    skipped_name(const std::string &name) {
      const std::string lower = ascii_lower(name);
      return lower.empty() || lower[0] == '$' || lower == "system volume information" || lower == "windows" || lower == ".git" || lower == "node_modules";
    }
    bool
    in_skip_list(const fs::path &path, const std::vector<fs::path> &skip) {
      for (const auto &blocked : skip)
        if (!blocked.empty() && paths_equal(path, blocked)) return true;
      return false;
    }
    bool
    wanted(const ScanOptions &options, std::string_view file) {
      const std::string game = game_of_executable(file);
      if (game.empty()) return false;
      if (options.game.empty()) return true;
      if (game != options.game) return false;
      const auto names = executables_for(options.game, options.region);
      for (const auto &name : names)
        if (ascii_lower(name) == ascii_lower(file)) return true;
      return false;
    }
  }  // namespace

  ScanResult
  scan_for_games(const fs::path &root, const ScanOptions &options) {
    ScanResult result;
    const auto clock = options.now ? options.now : std::function<std::chrono::steady_clock::time_point()>([] { return std::chrono::steady_clock::now(); });
    const auto deadline = clock() + options.limit;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return result;
    std::vector<fs::path> level { root };
    for (int depth = 0; depth <= options.max_depth && !level.empty(); ++depth) {
      result.levels = depth + 1;
      std::vector<fs::path> next;
      for (const auto &dir : level) {
        if (clock() >= deadline) {
          result.timed_out = true;
          return result;
        }
        if (result.folders >= options.max_folders) {
          result.truncated = true;
          return result;
        }
        ++result.folders;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
          ec.clear();
          continue;
        }
        std::vector<fs::path> files;
        std::vector<fs::path> folders;
        for (; it != fs::directory_iterator(); it.increment(ec)) {
          if (ec) {
            ec.clear();
            break;
          }
          const fs::directory_entry &entry = *it;
          std::error_code kind;
          if (entry.is_symlink(kind)) continue;
          if (entry.is_regular_file(kind))
            files.push_back(entry.path());
          else if (entry.is_directory(kind))
            folders.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto &file : files)
          if (wanted(options, file.filename().string())) result.matches.push_back({ file, depth });
        for (const auto &folder : folders) {
          if (skipped_name(folder.filename().string()) || in_skip_list(folder, options.skip)) continue;
          next.push_back(folder);
        }
      }
      // The nearest install wins: stop at the first level that has one.
      if (!result.matches.empty()) return result;
      std::sort(next.begin(), next.end());
      level = std::move(next);
    }
    return result;
  }

  BrowseResult
  browse_folder(const fs::path &path, bool executables, const std::function<std::vector<BrowseEntry>()> &roots, std::size_t limit) {
    BrowseResult result;
    if (path.empty()) {
      if (roots) result.entries = roots();
      return result;
    }
    std::error_code ec;
    if (!path.is_absolute()) {
      result.status = Status::fail(400, "invalid_input", "The path must be absolute");
      return result;
    }
    if (!fs::is_directory(path, ec)) {
      result.status = Status::fail(404, "not_found", "That folder does not exist");
      return result;
    }
    result.path = path.string();
    const fs::path parent = path.parent_path();
    result.parent = parent == path ? std::string() : parent.string();
    fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
      result.status = Status::fail(409, "not_readable", "That folder cannot be read");
      return result;
    }
    std::vector<BrowseEntry> dirs;
    std::vector<BrowseEntry> files;
    for (; it != fs::directory_iterator(); it.increment(ec)) {
      if (ec) {
        ec.clear();
        break;
      }
      const fs::directory_entry &entry = *it;
      const std::string name = entry.path().filename().string();
      if (name.empty() || name[0] == '$' || ascii_lower(name) == "system volume information") continue;
      std::error_code kind;
      if (entry.is_directory(kind)) {
        dirs.push_back({ name, entry.path().string(), "dir", false });
      }
      else if (executables && entry.is_regular_file(kind) && ascii_lower(entry.path().extension().string()) == ".exe") {
        files.push_back({ name, entry.path().string(), "file", !game_of_executable(name).empty() });
      }
    }
    auto by_name = [](const BrowseEntry &a, const BrowseEntry &b) { return ascii_lower(a.name) < ascii_lower(b.name); };
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(files.begin(), files.end(), [&](const BrowseEntry &a, const BrowseEntry &b) { return a.known != b.known ? a.known : by_name(a, b); });
    for (auto *group : { &dirs, &files }) {
      for (auto &entry : *group) {
        if (result.entries.size() >= limit) {
          result.truncated = true;
          break;
        }
        result.entries.push_back(std::move(entry));
      }
    }
    return result;
  }

  json
  browse_json(const BrowseResult &result) {
    json entries = json::array();
    for (const auto &entry : result.entries) {
      json row = { { "name", entry.name }, { "path", entry.path }, { "kind", entry.kind } };
      if (entry.known) row["known"] = true;
      entries.push_back(row);
    }
    return { { "ok", true }, { "path", result.path }, { "parent", result.parent }, { "entries", entries }, { "truncated", result.truncated } };
  }
}  // namespace sparxie

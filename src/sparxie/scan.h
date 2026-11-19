#pragma once
#include "src/sparxie/model.h"
#include <chrono>
#include <functional>

namespace sparxie {
  // Folder search for a game install. The chosen folder is level 0; its
  // sub-folders are level 1, and so on. A level is searched completely before
  // the next one starts, so the install nearest to the chosen folder wins, and
  // the search stops at the first level that holds a known game executable.
  struct ScanOptions {
    int max_depth = 4;
    std::chrono::milliseconds limit { 8000 };
    std::size_t max_folders = 20000;
    // Only the executables of this game; empty means every known game.
    std::string game;
    std::string region;
    // Folders that are never entered (the Sparxie install itself).
    std::vector<fs::path> skip;
    // Replaceable clock, for tests.
    std::function<std::chrono::steady_clock::time_point()> now;
  };

  struct ScanMatch {
    fs::path exe;
    int depth = 0;
  };

  struct ScanResult {
    std::vector<ScanMatch> matches;
    bool timed_out = false;
    // The folder limit was reached before every level was searched.
    bool truncated = false;
    int levels = 0;
    std::size_t folders = 0;
  };

  ScanResult
  scan_for_games(const fs::path &root, const ScanOptions &options);

  // One entry of a folder listing for the browse sheet.
  struct BrowseEntry {
    std::string name;
    std::string path;
    std::string kind;  // "drive", "dir" or "file"
    bool known = false;  // a file that is a known game executable
  };

  struct BrowseResult {
    Status status;
    std::string path;
    std::string parent;
    std::vector<BrowseEntry> entries;
    bool truncated = false;
  };

  // Lists one folder, read-only. `roots` supplies the drives for an empty
  // path. Folders come first, then (with `executables`) the .exe files, each
  // group sorted without regard to case; known game executables lead the files.
  BrowseResult
  browse_folder(const fs::path &path, bool executables, const std::function<std::vector<BrowseEntry>()> &roots, std::size_t limit = 3000);

  json
  browse_json(const BrowseResult &result);
}  // namespace sparxie

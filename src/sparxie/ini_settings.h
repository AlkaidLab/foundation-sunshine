#pragma once
#include "src/sparxie/model.h"
#include "src/sparxie/settings.h"
#include <optional>
#include <string>
#include <vector>
namespace sparxie::ini_settings {
  struct File {
    fs::path path;
    std::vector<std::string> lines;
  };
  std::optional<File>
  game_file(const Install &install);
  std::string
  get(const File &file, const char *key);
  bool
  set(File &file, const char *key, const std::string &value);
  fs::path
  key(const char *field);
  bool
  write(const File &file);
  bool
  number(const std::string &text, double &value);
  bool
  has_game_section(const File &file);
  SettingsResult
  write_game_file(const Install &install, const GameValues &values, const std::vector<std::string> &supported, const std::set<std::string> &fields, const fs::path &backup_dir);
}  // namespace sparxie::ini_settings

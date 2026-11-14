#pragma once

#include "src/sparxie/settings.h"

#include <set>
#include <string>
#include <string_view>

namespace sparxie {
  // miHoYo PC settings ported from Collapse Launcher 1.84 (d478585, MIT,
  // Copyright (c) neon-nyan). Registry names, JSON keys, domains and defaults
  // follow GameSettings/{Genshin,StarRail,Zenless,Honkai,Universal}.
  bool
  hoyo_is_field(std::string_view key);
  json
  hoyo_catalog(std::string_view game);
  const char *const *
  hoyo_registry_names(std::size_t &count);
  Status
  hoyo_validate(const Install &install, const GameValues &values, const std::set<std::string> &fields);
  void
  hoyo_read(const Install &install, Registry &registry, SettingsResult &result);
  void
  hoyo_write(const Install &install, Registry &registry, const GameValues &values, const std::set<std::string> &fields, SettingsResult &result);
  json
  game_launch_options(const json &settings, std::string_view game);
  std::string
  game_launch_command(std::string command, const Install &install, const json &options);
  // 提升游戏优先级 (Collapse's UseGameBoost) is kept by the host, not written to the game: the host raises the priority of the game process after it starts.
  // It counts only when the application holds it as one of its changed items (an application keeps only the items the person changed).
  bool
  game_boost_on(const json &settings);
  // The Sparxie profile options of an application with the boost folded in: a priority below "aboveNormal" becomes "aboveNormal", so Sparxie's own
  // runtime does not put the game back to normal. `options` stay as they are when the boost is off or the priority is already higher.
  json
  options_with_boost(json options, bool boost);
  // Why custom launch arguments are refused, empty when they are fine (the same rule hoyo_validate applies).
  std::string
  custom_args_problem(std::string_view raw);
}  // namespace sparxie

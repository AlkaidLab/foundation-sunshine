/**
 * @file src/default_apps.h
 * @brief Default application entries that an existing apps.json receives once.
 *
 * The first entry is Big Screen: it opens Foundation Desktop (the couch interface of the control panel) on the
 * host when a client launches it. The logic is header-only and works on a property tree, so that it can be
 * tested without the rest of the host.
 *
 * A new install gets the entry from the bundled apps.json. An install that already has an apps.json gets it once:
 * the id of every default entry that was offered is recorded in the top-level "default-apps-seen" array of the same
 * file, and an id that is recorded is never offered again, so an entry that the person deleted stays deleted.
 * The Web UI keeps the keys of the file that it does not edit, so the record survives its saves.
 */
#pragma once

// standard includes
#include <string>
#include <string_view>

// lib includes
#include <boost/property_tree/ptree.hpp>

// local includes
#include "default_app_names.h"

namespace default_apps {
  using namespace std::literals;

  /// The top-level key of apps.json that lists the ids of the default entries already offered.
  inline constexpr auto seen_key = "default-apps-seen"sv;
  /// The id of the Big Screen entry in that list.
  inline constexpr auto big_screen_id = "big-screen"sv;
  /// The part of a command that opens the big screen; a command that has it is the entry (or the person's own copy of it).
  inline constexpr auto big_screen_flag = "--desktop"sv;
  /// The command that opens the big screen (second instances focus the window that is there). The path is the one of the menu commands of the Desktop entry.
  inline constexpr auto big_screen_open_cmd = ".\\assets\\gui\\sunshine-gui.exe --desktop"sv;
  /// The command that closes it when the stream ends.
  inline constexpr auto big_screen_close_cmd = ".\\assets\\gui\\sunshine-gui.exe --close-desktop"sv;
  /// The cover of the entry, in the assets folder.
  inline constexpr auto big_screen_cover = "bigscreen.png"sv;
  /// The landscape cover of the entry (the `hero` of the entry, served as /appasset?type=hero).
  inline constexpr auto big_screen_hero = "bigscreen-hero.png"sv;
  /// The key of an app that names which default entry it is ("desktop", "steam", "xbox", "big-screen"). While the name still is one of that entry's names it follows the language; a name the person wrote stays.
  inline constexpr auto default_id_key = "default-id"sv;
  inline constexpr auto desktop_id = "desktop"sv;
  inline constexpr auto steam_id = "steam"sv;
  inline constexpr auto xbox_id = "xbox"sv;

  /**
   * @brief The name of the entry in a language: 大屏 for Chinese, Big Screen otherwise. The name is kept as written, like the other default entries.
   */
  inline std::string
  big_screen_name(bool zh) {
    return zh ? "\xE5\xA4\xA7\xE5\xB1\x8F" : "Big Screen";
  }

  /// Whether the locale setting of the host names Chinese ("zh", "zh_CN", "zh-Hans"...).
  inline bool
  locale_is_zh(std::string_view locale) {
    return locale.size() >= 2 && (locale[0] == 'z' || locale[0] == 'Z') && (locale[1] == 'h' || locale[1] == 'H');
  }

  /// Whether the id is in the record of the file.
  inline bool
  was_offered(const boost::property_tree::ptree &tree, std::string_view id) {
    const auto seen = tree.get_child_optional(std::string { seen_key });
    if (!seen) {
      return false;
    }
    for (const auto &[_, node] : *seen) {
      if (node.get_value<std::string>() == id) {
        return true;
      }
    }
    return false;
  }

  /// Puts the id in the record of the file (once).
  inline void
  record_offered(boost::property_tree::ptree &tree, std::string_view id) {
    if (was_offered(tree, id)) {
      return;
    }
    boost::property_tree::ptree item;
    item.put_value(std::string { id });
    if (auto seen = tree.get_child_optional(std::string { seen_key })) {
      seen->push_back({ "", item });
    }
    else {
      boost::property_tree::ptree list;
      list.push_back({ "", item });
      tree.add_child(std::string { seen_key }, list);
    }
  }

  /// The entry as a node of the "apps" array.
  inline boost::property_tree::ptree
  big_screen_app(bool zh) {
    namespace pt = boost::property_tree;
    pt::ptree app;
    app.put("name", big_screen_name(zh));
    app.put("image-path", std::string { big_screen_cover });
    app.put("hero", std::string { big_screen_hero });
    app.put(std::string { default_id_key }, std::string { big_screen_id });
    app.put("fs-kind", "bigscreen");
    app.put("fs-default", "true");
    // A desktop stream (no command of its own), as the Desktop entry, with the big screen opened beside it.
    // `detached` is not waited for: the first start of the panel may stay running, a second start only focuses.
    app.put("exclude-global-prep-cmd", "false");
    app.put("auto-detach", "true");
    app.put("wait-all", "true");
    app.put("exit-timeout", "5");
    pt::ptree detached;
    pt::ptree open;
    open.put_value(std::string { big_screen_open_cmd });
    detached.push_back({ "", open });
    app.add_child("detached", detached);
    // The undo command runs when the stream ends: the big screen closes and does not stay on the host.
    pt::ptree prep;
    prep.put("do", "");
    prep.put("undo", std::string { big_screen_close_cmd });
    prep.put("elevated", "false");
    pt::ptree prep_list;
    prep_list.push_back({ "", prep });
    app.add_child("prep-cmd", prep_list);
    return app;
  }

  /// Whether an app node already opens the big screen (the entry itself, or a copy the person made).
  inline bool
  opens_big_screen(const boost::property_tree::ptree &app) {
    const auto has_flag = [](const std::string &cmd) {
      return cmd.find(big_screen_flag) != std::string::npos;
    };
    if (const auto cmd = app.get_optional<std::string>("cmd"); cmd && has_flag(*cmd)) {
      return true;
    }
    if (const auto detached = app.get_child_optional("detached")) {
      for (const auto &[_, node] : *detached) {
        if (has_flag(node.get_value<std::string>())) {
          return true;
        }
      }
    }
    return false;
  }

  /// The landscape cover that a default entry ships.
  inline std::string
  default_hero(std::string_view id) {
    if (id == desktop_id) return "desktop-hero.png";
    if (id == steam_id) return "steam-hero.png";
    if (id == xbox_id) return "box-hero.png";
    if (id == big_screen_id) return std::string { big_screen_hero };
    return {};
  }

  /// Which default entry an app is, by what it opens ("" when it is none of them).
  inline std::string_view
  signature_of(const boost::property_tree::ptree &app) {
    if (opens_big_screen(app)) return big_screen_id;
    const auto cmd = app.get<std::string>("cmd", "");
    if (cmd.find("steam://open/bigpicture") != std::string::npos) return steam_id;
    if (cmd.find("start xbox:") != std::string::npos) return xbox_id;
    if (cmd.empty() && app.get<std::string>("image-path", "") == "desktop") return desktop_id;
    return {};
  }

  /**
   * @brief Marks the default entries of a file that has no marks yet (an apps.json of an older install).
   * @return ``true`` when the tree changed.
   * @details An app gets `default-id` when what it opens is a default entry AND its name is one of that entry's names: a name the person
   * wrote is not marked and never follows the language. The landscape cover of the entry is set once with the mark (an app that was marked
   * keeps whatever `hero` it has, also none).
   */
  inline bool
  stamp_defaults(boost::property_tree::ptree &tree) {
    auto apps = tree.get_child_optional("apps");
    if (!apps) {
      return false;
    }
    bool changed = false;
    for (auto &[_, app] : *apps) {
      if (app.get_optional<std::string>(std::string { default_id_key })) {
        continue;
      }
      const auto id = signature_of(app);
      if (id.empty() || !names::is_known(id, app.get<std::string>("name", ""))) {
        continue;
      }
      app.put(std::string { default_id_key }, std::string { id });
      if (!app.get_optional<std::string>("hero")) {
        if (const auto hero = default_hero(id); !hero.empty()) {
          app.put("hero", hero);
        }
      }
      changed = true;
    }
    return changed;
  }

  /**
   * @brief Shows the names of the default entries in a language.
   * @return ``true`` when a name changed.
   * @details Only a marked app whose name still is one of its entry's names changes (also the Touch Keyboard / Desktop Pet commands of the
   * Desktop entry, by their id). A renamed one keeps the person's name, in every later change of language too.
   */
  inline bool
  retitle(boost::property_tree::ptree &tree, std::string_view locale) {
    auto apps = tree.get_child_optional("apps");
    if (!apps) {
      return false;
    }
    bool changed = false;
    for (auto &[_, app] : *apps) {
      const auto id = app.get_optional<std::string>(std::string { default_id_key });
      if (!id) {
        continue;
      }
      const auto target = names::name_in(*id, locale);
      const auto name = app.get<std::string>("name", "");
      if (!target.empty() && name != target && names::is_known(*id, name)) {
        app.put("name", target);
        changed = true;
      }
      if (auto menu = app.get_child_optional("menu-cmd")) {
        for (auto &[__, command] : *menu) {
          const auto command_id = command.get_optional<std::string>("id");
          if (!command_id || !names::find(*command_id)) {
            continue;
          }
          const auto command_target = names::name_in(*command_id, locale);
          const auto command_name = command.get<std::string>("name", "");
          if (command_name != command_target && names::is_known(*command_id, command_name)) {
            command.put("name", command_target);
            changed = true;
          }
        }
      }
    }
    return changed;
  }

  /// The name that the id of an application is made from: the English name of a default entry that still has one of its names, so the id stays when the language changes.
  inline std::string
  id_name(const boost::property_tree::ptree &app, const std::string &name) {
    if (const auto id = app.get_optional<std::string>(std::string { default_id_key }); id && names::is_known(*id, name)) {
      return names::canonical_name(*id);
    }
    return name;
  }

  /**
   * @brief Offers the Big Screen entry to a file that has not been offered it yet.
   * @param tree The parsed apps.json.
   * @param zh Whether the host's locale is Chinese (the name of the entry).
   * @return ``true`` when the tree changed and has to be written.
   * @details Recorded: nothing happens, whether the entry is in the list or not. Not recorded: the entry is added at the end
   * unless an app already opens the big screen, and the id is recorded either way.
   */
  inline bool
  offer_big_screen(boost::property_tree::ptree &tree, bool zh) {
    if (was_offered(tree, big_screen_id)) {
      return false;
    }
    auto apps = tree.get_child_optional("apps");
    if (!apps) {
      // not an apps file: leave it alone, the parser reports it
      return false;
    }
    bool present = false;
    for (const auto &[_, app] : *apps) {
      if (opens_big_screen(app)) {
        present = true;
        break;
      }
    }
    if (!present) {
      apps->push_back({ "", big_screen_app(zh) });
    }
    record_offered(tree, big_screen_id);
    return true;
  }

  /**
   * @brief Runs the offers on the apps file of the host (Windows hosts only; the panel is a Windows component).
   * @param file_name The apps.json path in UTF-8.
   * @param locale The locale setting of the host.
   * @details A file that cannot be read or written is left as it is, with a line in the log: the host starts either way.
   */
  void
  migrate_file(const std::string &file_name, std::string_view locale);

  /**
   * @brief Shows the names of the default entries of the apps file in the language of the host (the language was changed).
   * @return ``true`` when the file was written.
   */
  bool
  retitle_file(const std::string &file_name, std::string_view locale);
}  // namespace default_apps

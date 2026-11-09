#include "src/sparxie/host.h"
#include "src/sparxie/hoyo_settings.h"
#include "src/sparxie/install.h"
#include "src/sparxie/platform.h"
#include "src/sparxie/region.h"
#include "src/sparxie/vendors.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <atomic>
#include <iterator>
#include <mutex>
#include <thread>
#include <optional>
namespace sparxie {
  namespace {
    json
    read_apps(const fs::path &path) {
      json body = { { "apps", json::array() } };
      if (path.empty() || !fs::exists(path)) return body;
      std::ifstream in(path);
      try {
        json parsed;
        in >> parsed;
        if (parsed.is_object()) body = parsed;
      }
      catch (const std::exception &) {
      }
      if (!body.contains("apps") || !body["apps"].is_array()) body["apps"] = json::array();
      // the generic apps writer stores every scalar of a sparxie block as a string: read it back as what it is
      normalize_apps(body);
      return body;
    }
    // true when the whole file reached the disk under its name
    bool
    write_apps(const fs::path &path, const json &body) {
      if (path.empty()) return true;
      std::error_code ec;
      fs::create_directories(path.parent_path(), ec);
      const std::string text = body.dump(2);
      const fs::path tmp = path.string() + ".tmp";
      {
        std::ofstream out(tmp, std::ios::binary);
        out << text;
        out.flush();
        if (!out) return false;
      }
      fs::rename(tmp, path, ec);
      if (ec) {
        std::ofstream out(path, std::ios::binary);
        out << text;
        out.flush();
        fs::remove(tmp, ec);
        return static_cast<bool>(out);
      }
      return true;
    }
    // Every change of apps.json goes through here: the host keeps the application list in memory, so a request that changed the file asks it to reload (confighttp reads apps_writes after the request)
    bool
    commit_apps(HostEnv &env, const json &body) {
      if (!write_apps(env.apps_file, body)) return false;
      if (!env.apps_file.empty()) ++env.apps_writes;
      return true;
    }
    // the newest official cover of an install already on disk (the refresh writes covers/sparxie-<install>-<version>.png), or empty
    fs::path
    newest_cover(const HostEnv &env, const std::string &install_id) {
      std::error_code ec;
      fs::path newest;
      fs::file_time_type stamp;
      if (install_id.empty()) return newest;
      for (const auto &entry : fs::directory_iterator(env.covers_dir, ec)) {
        if (entry.path().filename().string().rfind("sparxie-" + install_id + "-", 0) != 0) continue;
        if (entry.path().stem().string().find("-badge") != std::string::npos) continue;
        if (newest.empty() || entry.last_write_time() > stamp) {
          newest = entry.path();
          stamp = entry.last_write_time();
        }
      }
      return newest;
    }
    // the landscape poster of an install the refresh composed from the official background and logo (posters/<install>/hero.jpg, 16:9, JPEG), or empty: the 大图 layouts
    // of the clients ask for it as the hero. banner.png (the old 6:1 strip) is no hero any more.
    fs::path
    official_hero(const HostEnv &env, const std::string &install_id) {
      if (install_id.empty()) return {};
      std::error_code ec;
      const fs::path hero = env.host_data / "posters" / install_id / "hero.jpg";
      return fs::is_regular_file(hero, ec) ? hero : fs::path {};
    }

    // a hero the host itself made (empty, the old banner strip or the landscape poster of some install under posters/): the refresh may replace it. Any other path or
    // address is a person's own image and stays.
    bool
    hero_is_host_made(const HostEnv &env, const json &app) {
      if (!app.contains("hero") || !app["hero"].is_string() || app["hero"].get<std::string>().empty()) return true;
      const fs::path hero = fs::path(app["hero"].get<std::string>());
      const std::string name = hero.filename().string();
      if (name != "banner.png" && name != "hero.png" && name != "hero.jpg" && name != "hero-badge.jpg") return false;
      std::error_code ec;
      const fs::path posters = (env.host_data / "posters").lexically_normal();
      return hero.parent_path().parent_path().lexically_normal() == posters;
    }

    // A landscape poster of the install that is blank (hero.jpg or hero-badge.jpg, composed from a background that decoded as zeros) is removed, and the host-made heroes that
    // pointed at it are cleared, so the clients show the cover instead of a black picture; true when it removed one. The badged hero is drawn from the plain one: a blank
    // hero.jpg takes hero-badge.jpg with it (a black picture with a badge in the corner is as blank as the black one).
    bool
    drop_blank_hero(const HostEnv &env, json &apps, const std::string &install_id) {
      std::error_code ec;
      const fs::path dir = env.host_data / "posters" / install_id;
      const auto drop = [&](const fs::path &file, bool force) {
        if (!fs::is_regular_file(file, ec)) return false;
        std::ifstream in(file, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        // only a picture that decodes and is blank: a file that is no JPEG at all is not this problem (and is left to whoever put it there)
        const bool jpeg = bytes.size() >= 4 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xD8;
        if (!force && !(jpeg && jpeg_is_blank(bytes))) return false;
        if (env.warn) env.warn("the landscape poster " + file.filename().string() + " of " + install_id + " is blank: it is removed and recomposed from the cached pictures");
        fs::remove(file, ec);
        for (auto &app : apps["apps"]) {
          if (!app.contains("sparxie") || !app["sparxie"].is_object() || app["sparxie"].value("install", std::string()) != install_id) continue;
          if (app.contains("hero") && app["hero"].is_string() && fs::path(app["hero"].get<std::string>()) == file) app.erase("hero");
        }
        return true;
      };
      const bool plain = drop(dir / "hero.jpg", false);
      const bool badged = drop(dir / "hero-badge.jpg", plain);
      return plain || badged;
    }

    // ---- the Sparxie badge on the posters ----------------------------------------------------------------------------------------------------------------------
    // A touch application shows the Sparxie mark on its poster. The mark is drawn into the image, because the clients only show the picture they are given: the host keeps
    // a plain and a badged variant of every official poster (covers/sparxie-<install>-<version>[-badge<rev>].png, posters/<install>/hero[-badge].jpg), and an application
    // points at the one its `badge` setting asks for. The setting is stored in the sparxie block: absent, it is on for an official cover and off for a cover of the
    // person's own; an application that launches the game directly has no badge. The badge says "this application runs Sparxie's touch interface", so it follows the
    // input: an application with keyboard, mouse and gamepad (also through Sparxie's PC-interface session) and Endfield's automatic input have the plain posters, whatever
    // was stored in `badge`; the person's own on/off wins over the default only for a touch input.
    const Rgba &
    sparxie_mark() {
      static const Rgba mark = badge_mark();
      return mark;
    }

    std::string
    string_at(const json &object, const char *key) {
      return object.is_object() && object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : std::string();
    }

    bool
    badge_wanted(const json &block) {
      if (!block.is_object() || string_at(block, "mode") != "touch") return false;
      std::string input = block.contains("options") ? string_at(block["options"], "input") : std::string();
      if (input.empty()) input = "touch";
      if (input != "touch") return false;
      const bool fallback = string_at(block, "cover") != "custom";
      if (!block.contains("badge")) return fallback;
      const json &flag = block["badge"];
      if (flag.is_boolean()) return flag.get<bool>();
      if (flag.is_string()) return flag.get<std::string>() == "true";
      return fallback;
    }

    // the badged sibling of an official cover file
    fs::path
    badged_cover_of(const fs::path &plain) {
      return plain.parent_path() / (plain.stem().string() + "-badge" + std::to_string(kBadgeRevision) + ".png");
    }

    struct Posters {
      fs::path cover;
      fs::path hero;
    };

    Posters
    official_posters(const HostEnv &env, const std::string &install_id, bool badge) {
      Posters posters { newest_cover(env, install_id), official_hero(env, install_id) };
      if (!badge) return posters;
      std::error_code ec;
      if (!posters.cover.empty()) {
        const fs::path badged = badged_cover_of(posters.cover);
        if (fs::is_regular_file(badged, ec)) posters.cover = badged;
      }
      if (!install_id.empty()) {
        const fs::path hero = env.host_data / "posters" / install_id / "hero-badge.jpg";
        if (fs::is_regular_file(hero, ec)) posters.hero = hero;
      }
      return posters;
    }

    // a cover the host itself made (covers/sparxie-*): the refresh may replace it
    bool
    cover_is_host_made(const HostEnv &env, const std::string &path) {
      const fs::path cover(path);
      return paths_equal(cover.parent_path(), env.covers_dir) && cover.filename().string().rfind("sparxie-", 0) == 0;
    }

    // Points an official-cover application at the posters its badge setting asks for. The cover is replaced when it is empty or the host's own (always with `force_cover`),
    // the hero when it is empty or the host's own. Returns true when the application changed.
    bool
    reconcile_official(const HostEnv &env, json &app, bool force_cover) {
      const json &block = app["sparxie"];
      const Posters posters = official_posters(env, string_at(block, "install"), badge_wanted(block));
      bool changed = false;
      const std::string cover_now = string_at(app, "image-path");
      if (!posters.cover.empty() && cover_now != posters.cover.string() && (force_cover || cover_now.empty() || cover_is_host_made(env, cover_now))) {
        app["image-path"] = posters.cover.string();
        changed = true;
      }
      const std::string hero_now = string_at(app, "hero");
      if (!posters.hero.empty() && hero_now != posters.hero.string() && hero_is_host_made(env, app)) {
        app["hero"] = posters.hero.string();
        changed = true;
      }
      return changed;
    }

    // The installer's image migration (migrate-images.ps1) copies every poster that is not a bare file name into covers/app_<hash>_<time>.png, the official covers of an
    // application included, and the host then took the copy for the person's own picture and never gave it the badged or plain variant again. A copy whose bytes are one of
    // the install's official covers (covers/sparxie-<install>-*.png, plain or badged) is the official cover again: the image path is cleared and the reconcile picks the
    // poster the badge rule asks for. A cover the person chose ("custom") and any other picture stay as they are. True when the path was cleared.
    bool
    release_migrated_official_cover(const HostEnv &env, json &app) {
      const json &block = app["sparxie"];
      if (string_at(block, "cover") == "custom") return false;
      const std::string install_id = string_at(block, "install");
      const std::string current = string_at(app, "image-path");
      if (install_id.empty() || current.empty()) return false;
      fs::path file(current);
      if (file.is_relative()) file = env.covers_dir / file;
      if (!paths_equal(file.parent_path(), env.covers_dir) || file.filename().string().rfind("app_", 0) != 0) return false;
      std::error_code ec;
      if (!fs::is_regular_file(file, ec)) return false;
      const auto size = fs::file_size(file, ec);
      if (ec || size == 0) return false;
      const auto read_all = [](const fs::path &path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      };
      std::string mine;
      const std::string prefix = "sparxie-" + install_id + "-";
      for (const auto &entry : fs::directory_iterator(env.covers_dir, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().filename().string().rfind(prefix, 0) != 0) continue;
        if (entry.file_size(ec) != size) continue;
        if (mine.empty()) mine = read_all(file);
        if (!mine.empty() && read_all(entry.path()) == mine) {
          app.erase("image-path");
          return true;
        }
      }
      return false;
    }

    fs::path
    custom_badge_dir(const HostEnv &env) {
      return env.host_data / "posters" / "custom";
    }

    // true for any poster file the host wrote itself (official covers and heroes, and the badged copies of a person's own images)
    bool
    is_host_poster(const HostEnv &env, const std::string &path) {
      if (cover_is_host_made(env, path)) return true;
      const fs::path parent = fs::path(path).parent_path();
      const fs::path posters = env.host_data / "posters";
      return paths_equal(parent, posters) || paths_equal(parent.parent_path(), posters);
    }

    // A badged copy of a picture of the person's own (a local PNG or JPEG file): posters/custom/<hash of the path>-<size>-<time>-r<revision>-badge.<ext>. The same source and
    // badge drawing give the same file, which is then left alone; an older copy of the same source goes. Empty when the source is an address or cannot be read as a PNG or JPEG.
    std::string
    badged_copy_of(const HostEnv &env, const std::string &source) {
      if (source.empty() || source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0) return {};
      std::error_code ec;
      fs::path file(source);
      if (!fs::is_regular_file(file, ec)) {
        file = env.covers_dir / source;
        if (!fs::is_regular_file(file, ec)) return {};
      }
      const auto size = fs::file_size(file, ec);
      if (ec) return {};
      const auto stamp = fs::last_write_time(file, ec).time_since_epoch().count();
      if (ec) return {};
      std::ifstream in(file, std::ios::binary);
      if (!in) return {};
      const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const bool jpeg = bytes.size() > 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xD8;
      const std::string key = fnv8(norm_path(file));
      const fs::path dir = custom_badge_dir(env);
      const fs::path target = dir / (key + "-" + std::to_string(size) + "-" + std::to_string(stamp) + "-r" + std::to_string(kBadgeRevision) + "-badge" + (jpeg ? ".jpg" : ".png"));
      if (fs::is_regular_file(target, ec)) return target.string();
      const Rgba image = decode_image(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
      if (image.w <= 0) return {};
      const Rgba badged = with_badge(image, sparxie_mark());
      const auto encoded = jpeg ? encode_jpeg(badged, kHeroJpegQuality) : encode_png(badged);
      if (encoded.empty()) return {};
      fs::create_directories(dir, ec);
      for (const auto &entry : fs::directory_iterator(dir, ec))
        if (entry.path().filename().string().rfind(key + "-", 0) == 0) fs::remove(entry.path(), ec);
      std::ofstream out(target, std::ios::binary);
      out.write(reinterpret_cast<const char *>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
      out.close();
      return out ? target.string() : std::string();
    }

    // The badge on the pictures of a custom cover: with the badge on, the application points at a badged copy of the person's own image and the block remembers the original
    // (customImage, customHero); with it off (the default for a custom cover) the original is the poster again. Returns true when the application changed.
    bool
    apply_custom_badge(const HostEnv &env, json &app) {
      json &block = app["sparxie"];
      const bool want = badge_wanted(block);
      const fs::path derived_dir = custom_badge_dir(env);
      bool changed = false;
      const std::pair<const char *, const char *> slots[] = { { "image-path", "customImage" }, { "hero", "customHero" } };
      for (const auto &[key, source_key] : slots) {
        const std::string current = string_at(app, key);
        const bool derived = !current.empty() && paths_equal(fs::path(current).parent_path(), derived_dir);
        std::string source = string_at(block, source_key);
        if (!derived) {
          // the person's own path; the host's own poster files and an empty slot are not theirs
          if (current.empty() || is_host_poster(env, current)) {
            if (block.contains(source_key)) {
              block.erase(source_key);
              changed = true;
            }
            continue;
          }
          source = current;
        }
        else if (source.empty()) {
          continue;
        }
        const std::string made = want ? badged_copy_of(env, source) : std::string();
        if (made.empty()) {
          if (current != source) {
            app[key] = source;
            changed = true;
          }
          if (block.contains(source_key)) {
            block.erase(source_key);
            changed = true;
          }
          continue;
        }
        if (current != made) {
          app[key] = made;
          changed = true;
        }
        if (string_at(block, source_key) != source) {
          block[source_key] = source;
          changed = true;
        }
      }
      return changed;
    }

    // Writes a file unless it already holds exactly these bytes (the modification time and the art version of a poster that comes out the same stay put). False when the write failed.
    bool
    write_if_changed(const fs::path &file, const std::vector<std::uint8_t> &bytes) {
      {
        std::ifstream current(file, std::ios::binary);
        if (current) {
          const std::string had((std::istreambuf_iterator<char>(current)), std::istreambuf_iterator<char>());
          if (had.size() == bytes.size() && std::equal(had.begin(), had.end(), bytes.begin(), [](char a, std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; })) return true;
        }
      }
      std::ofstream out(file, std::ios::binary);
      out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      out.close();
      return static_cast<bool>(out);
    }

    // The badged copy of the newest official cover of an install when it is missing (an install whose posters were made before the badge); true when one was written.
    bool
    ensure_cover_badge(const HostEnv &env, const std::string &install_id) {
      const fs::path plain = newest_cover(env, install_id);
      if (plain.empty()) return false;
      std::error_code ec;
      const fs::path badged = badged_cover_of(plain);
      if (fs::is_regular_file(badged, ec)) return false;
      std::ifstream in(plain, std::ios::binary);
      if (!in) return false;
      const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const Rgba cover = decode_image(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
      if (cover.w <= 0 || image_is_blank(cover)) return false;
      const auto png = encode_png(with_badge(cover, sparxie_mark()));
      return !png.empty() && write_if_changed(badged, png);
    }

    // A badged cover that is blank (drawn from a blank cover) is removed and the host-made covers that pointed at it are cleared (the plain cover, or nothing, is the poster
    // again until a good one is made); true when it removed one.
    bool
    drop_blank_badged_cover(const HostEnv &env, json &apps, const std::string &install_id) {
      const fs::path plain = newest_cover(env, install_id);
      if (plain.empty()) return false;
      std::error_code ec;
      const fs::path badged = badged_cover_of(plain);
      if (!fs::is_regular_file(badged, ec)) return false;
      std::ifstream in(badged, std::ios::binary);
      const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      const Rgba image = decode_image(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
      if (image.w <= 0 || !image_is_blank(image)) return false;
      if (env.warn) env.warn("the badged cover of " + install_id + " is blank: it is removed");
      fs::remove(badged, ec);
      for (auto &app : apps["apps"]) {
        if (!app.contains("sparxie") || !app["sparxie"].is_object() || app["sparxie"].value("install", std::string()) != install_id) continue;
        if (app.contains("image-path") && app["image-path"].is_string() && fs::path(app["image-path"].get<std::string>()) == badged) app.erase("image-path");
      }
      return true;
    }

    // posters/<install>/hero.jpg and hero-badge.jpg from the background and the logo (JPEG: a few hundred KB instead of several MB of PNG, and the files the clients fetch); false
    // when no picture could be composed or written. A file is left alone (same modification time, same art version) when it would come out byte for byte the same. The first-round
    // hero.png of the install goes.
    bool
    write_hero(const HostEnv &env, const std::string &install_id, const Rgba &background, const Rgba &logo) {
      const Rgba hero = compose_hero(background, logo);
      // never publish a blank poster (a background a decoder returned as zeros: the logo alone on black would pass a check of the poster): the previous hero stays. The badged
      // variant is drawn from the hero that passed, so it never is a blank one.
      if (image_is_blank(background) || image_is_blank(hero)) {
        if (env.warn) env.warn("the landscape poster of " + install_id + " came out blank (the background picture is empty or one colour); the previous one is kept");
        return false;
      }
      const auto jpeg = encode_jpeg(hero, kHeroJpegQuality);
      if (jpeg.empty()) return false;
      std::error_code ec;
      const fs::path dir = env.host_data / "posters" / install_id;
      fs::create_directories(dir, ec);
      if (!write_if_changed(dir / "hero.jpg", jpeg)) return false;
      if (const auto badged = encode_jpeg(with_badge(hero, sparxie_mark()), kHeroJpegQuality); !badged.empty()) write_if_changed(dir / "hero-badge.jpg", badged);
      fs::remove(dir / "hero.png", ec);
      return true;
    }
    bool
    names_running(const HostEnv &env, const std::vector<std::string> &names) {
      if (env.store_hooks.running) return true;
      return env.process_running && env.process_running(names);
    }
    bool
    sparxie_open(const HostEnv &env) {
      return names_running(env, { "Sparxie.exe", "Sparxie.Desktop.exe", "Sparxie.Launcher.exe" });
    }
    int
    display_refresh_of(const HostEnv &env) { return env.display_refresh ? env.display_refresh() : 0; }
    std::string
    read_text(const fs::path &path) {
      std::ifstream in(path);
      if (!in) return {};
      return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    std::string
    component_version(const fs::path &exe) {
      if (exe.empty()) return {};
      const fs::path dirs[] = { exe.parent_path(), exe.parent_path().parent_path() };
      for (const auto &dir : dirs) {
        const std::string component = read_text(dir / "component.json");
        if (!component.empty()) {
          try {
            const json body = json::parse(component);
            if (body.contains("version") && body["version"].is_string()) return body["version"].get<std::string>();
          }
          catch (const std::exception &) {
          }
        }
        std::ifstream ini(dir / "version.ini");
        std::string line;
        while (std::getline(ini, line)) {
          if (line.empty()) continue;
          const auto eq = line.find('=');
          return eq == std::string::npos ? line : line.substr(eq + 1);
        }
      }
      return {};
    }
    // The optional features of the installed Sparxie build (app-<version>/capabilities.json, written by its package: "capabilities": ["touch-ui-switch"]).
    // A build without the file (preview.9 and older) has none.
    std::vector<std::string>
    component_capabilities(const fs::path &exe) {
      std::vector<std::string> found;
      if (exe.empty()) return found;
      std::vector<fs::path> dirs = { exe.parent_path() };
      std::error_code ec;
      for (const auto &entry : fs::directory_iterator(exe.parent_path(), ec))
        if (entry.is_directory(ec) && entry.path().filename().string().rfind("app-", 0) == 0) dirs.push_back(entry.path());
      for (const auto &dir : dirs) {
        const std::string text = read_text(dir / "capabilities.json");
        if (text.empty()) continue;
        try {
          const json body = json::parse(text);
          if (!body.contains("capabilities") || !body["capabilities"].is_array()) continue;
          for (const auto &item : body["capabilities"])
            if (item.is_string() && std::find(found.begin(), found.end(), item.get<std::string>()) == found.end()) found.push_back(item.get<std::string>());
        }
        catch (const std::exception &) {
        }
      }
      return found;
    }
    // The installed Sparxie can run a session without its touch interface (HoyoSettings.touch_ui_enabled), so keyboard, mouse and gamepad can use its settings.
    bool
    touch_ui_switch(HostEnv &env) {
      if (env.sparxie_exe.empty() && !env.component_dir.empty()) env.sparxie_exe = find_sparxie_exe(env.component_dir);
      if (env.sparxie_exe.empty()) env.sparxie_exe = find_sparxie_exe(env.install_root);
      const auto caps = component_capabilities(env.sparxie_exe);
      return std::find(caps.begin(), caps.end(), std::string(kCapTouchUiSwitch)) != caps.end();
    }
    bool
    sparxie_installed(HostEnv &env) {
      if (env.sparxie_exe.empty() && !env.component_dir.empty()) env.sparxie_exe = find_sparxie_exe(env.component_dir);
      if (env.sparxie_exe.empty()) env.sparxie_exe = find_sparxie_exe(env.install_root);
      return !env.sparxie_exe.empty() && fs::is_regular_file(env.sparxie_exe);
    }
    // Keyboard, mouse and gamepad is a Sparxie session without the touch interface. An installed Sparxie that cannot start one cannot serve it: the
    // application then starts the game directly (it stays a Sparxie title; nothing is lost when a newer Sparxie is installed and the app is saved again).
    // Without the component at all the caller's mode stands (the pages send a direct start then).
    // The titles whose Sparxie session can run with the keyboard, mouse and gamepad input: Genshin and Star Rail (the PC-interface switch) and Endfield (its own PC input);
    // Zenless Zone Zero has no such session, so its keyboard, mouse and gamepad application starts the game directly.
    bool
    kbm_through_sparxie(const std::string &game) {
      return game == "genshin" || game == "starrail" || game == "endfield";
    }
    std::string
    mode_for_input(HostEnv &env, const std::string &mode, const json &options, const std::string &game) {
      if (mode == "touch" && options.is_object() && options.value("input", std::string("touch")) == "kbm" && sparxie_installed(env) && (!touch_ui_switch(env) || !kbm_through_sparxie(game))) return "pc";
      return mode;
    }
    // What an application's input is: the stored one, else what its launch was (an application of the first builds that starts the game directly is keyboard, mouse and gamepad)
    std::string
    effective_input(const json &block) {
      std::string input = block.is_object() && block.contains("options") ? string_at(block["options"], "input") : std::string();
      if (!input.empty()) return input;
      return block.is_object() && string_at(block, "mode") == "pc" ? "kbm" : "touch";
    }
    json
    warning_body(const HostEnv &env, bool installed) {
      bool acknowledged = !installed;
      const fs::path path = env.host_data / "warning.json";
      try {
        const json body = json::parse(read_text(path));
        if (body.contains("acknowledged") && body["acknowledged"].is_boolean()) acknowledged = body["acknowledged"].get<bool>();
      }
      catch (const std::exception &) {
      }
      return json { { "warned", acknowledged }, { "statement", kStatement }, { "statement_en", kStatementEn } };
    }
    Registry &
    registry_of(HostEnv &env, MemoryRegistry &local) {
      return env.registry ? *env.registry : local;
    }
    fs::path
    config_dir(HostEnv &env) {
      if (!env.data_dir.empty()) return env.data_dir;
      return env.host_data / "sparxie-config";
    }
    std::string
    mode_of(const json &request) {
      const std::string mode = request.value("mode", std::string("touch"));
      // 直接启动 (direct) is stored as pc, the value the first builds used; touch (or its newer name sparxie) launches through Sparxie. It is a launch path: keyboard, mouse, gamepad and touch all work in it
      if (mode == "kb" || mode == "pc" || mode == "direct") return "pc";
      return "touch";
    }
    fs::path
    requested_path(const json &request) {
      if (request.contains("path") && request["path"].is_string() && !request["path"].get<std::string>().empty())
        return request["path"].get<std::string>();
      if (request.contains("install") && request["install"].is_object()) {
        const json &install = request["install"];
        if (install.contains("dir") && install["dir"].is_string()) {
          if (install.contains("exe") && install["exe"].is_string() && !install["exe"].get<std::string>().empty())
            return fs::path(install.value("dir", std::string())) / install.value("exe", std::string());
          return install["dir"].get<std::string>();
        }
      }
      return {};
    }
    // Only what the person chose is stored; a field that is not there follows the game's own value.
    json
    sparse_settings(const json &raw) {
      json out = json::object();
      if (!raw.is_object()) return out;
      const GameValues parsed = values_from_json(raw, GameValues {});
      const json full = values_json(parsed);
      for (const auto &key : chosen_fields(raw))
        if (full.contains(key)) out[key] = full[key];
      return out;
    }
    // The version (content hash) of the official poster each game and region has now: a poster that changes is a new address, so nothing stale stays in a cache.
    json
    art_versions(const HostEnv &env) {
      json out = json::object();
      for (const auto game : kGames) {
        for (const auto region : kRegions) {
          const std::string id = std::string(game) + "-" + std::string(region);
          try {
            const json index = json::parse(read_text(env.host_data / "posters" / id / "index.json"));
            if (index.contains("version") && index["version"].is_string() && !index["version"].get<std::string>().empty()) out[id] = { { "version", index["version"].get<std::string>() } };
          }
          catch (const std::exception &) {
          }
        }
      }
      return out;
    }
    // the index of the new application, or -1 when the list could not be saved
    int
    append_app(HostEnv &env, const json &app) {
      json apps = read_apps(env.apps_file);
      apps["apps"].push_back(app);
      if (!commit_apps(env, apps)) return -1;
      return static_cast<int>(apps["apps"].size()) - 1;
    }
    bool
    profile_taken(const json &apps, const std::string &id) {
      for (const auto &app : apps.value("apps", json::array())) {
        if (app.is_object() && app.contains("sparxie") && app["sparxie"].is_object() && app["sparxie"].value("profile", std::string()) == id) return true;
      }
      return false;
    }
    // The launcher shipped with the host (tools/sparxie-launch.exe, else beside it); empty when this install has none.
    fs::path
    shipped_launcher(const HostEnv &env) {
      fs::path launcher = env.install_root / "tools" / "sparxie-launch.exe";
      if (!fs::is_regular_file(launcher)) launcher = env.install_root / "sparxie-launch.exe";
      return fs::is_regular_file(launcher) ? launcher : fs::path {};
    }
    std::string
    touch_command(const HostEnv &env, const std::string &profile, const Install &install, const std::string &name) {
      const std::string templ = env.launch_template.empty() ? std::string(kDefaultLaunchTemplate) : env.launch_template;
      const std::string sparxie = env.sparxie_exe.empty() ? std::string("Sparxie.exe") : env.sparxie_exe.string();
      const fs::path launcher = shipped_launcher(env);
      const std::string sparxie_launcher = !launcher.empty() ? launcher.string() : "sparxie-launch.exe";
      return apply_template(templ, { { "sparxie", sparxie }, { "sparxie-launch", sparxie_launcher }, { "profile", profile }, { "exe", install.exe_path }, { "dir", install.dir }, { "id", install.id }, { "name", name } });
    }
    // Direct launch (直接启动): a store build starts the way its store wants,
    // an Endfield launcher build starts the way the Collapse Launcher's Endfield
    // plugin starts it (sparxie-launch --endfield-direct: the game is created
    // under a suspended Games.exe parent, because the client checks its parent
    // process; the official launcher is not needed), an Arknights launcher build
    // starts through the official launcher, and everything else starts its own
    // executable.
    json
    launch_options_of(const json &settings, std::string_view game) {
      return game_launch_options(settings, game);
    }

    // The static command of a direct start carries a size only for a fixed display; 跟随串流 resolves it at launch.
    void
    apply_display_to_launch_options(json &options, const json &display, std::string_view game) {
      if (!options.is_object()) options = json::object();
      const DisplayChoice choice = display_choice_of(json { { "display", display } });
      // the display choice owns the size and the window mode: whatever the settings say about them is not used
      for (const char *key : { "resolution", "customResolution", "customWidth", "customHeight", "launchWidth", "launchHeight", "launchFullscreen", "popupWindow", "borderless", "exclusiveFullscreen" })
        options.erase(key);
      int width = 0, height = 0;
      // Zenless Zone Zero's size is a position in its own list, not an argument
      if (choice.follow || game == "zzz" || !parse_resolution(choice.resolution, width, height)) return;
      options["launchWidth"] = width;
      options["launchHeight"] = height;
      options["launchFullscreen"] = choice.window != "windowed";
      if (choice.window == "borderless") options["popupWindow"] = true;
    }

    std::string
    append_launch_options(std::string command, const Install &install, const json &options) {
      return game_launch_command(std::move(command), install, options);
    }

    std::string
    direct_command(const HostEnv &env, const Install &install, const json &launch_options = json::object()) {
      const bool steam = install.source == "steam" || std::find(install.sources.begin(), install.sources.end(), "steam") != install.sources.end();
      const bool epic = install.source == "epic" || std::find(install.sources.begin(), install.sources.end(), "epic") != install.sources.end();
      if (steam && steam_app_id(install.game)) return "steam://rungameid/" + std::to_string(steam_app_id(install.game));
      if (epic && !install.epic_app.empty()) return "com.epicgames.launcher://apps/" + install.epic_app + "?action=launch&silent=true";
      // Endfield: Collapse's decoy-parent method, run by the launcher that ships with the host (the launcher also keeps the app alive while the game runs)
      if (install.game == "endfield" && !install.exe_path.empty()) {
        const fs::path launcher = shipped_launcher(env);
        if (!launcher.empty()) return quote_path(launcher.string()) + " --endfield-direct " + quote_path(install.exe_path);
      }
      if ((install.game == "arknights" || install.game == "endfield") && !env.hypergryph_roots.empty()) {
        const fs::path launcher = hypergryph_launcher_exe(env.hypergryph_roots);
        if (!launcher.empty()) return quote_path(launcher.string());
      }
      return append_launch_options(quote_path(install.exe_path), install, launch_options);
    }
    // Read as the signed-in user: their registry and the folders they can see.
    StorefrontEnv
    storefront_env_of(HostEnv &env, Registry &registry) {
      StorefrontEnv stores;
      stores.steam_root = env.steam_root;
      stores.epic_manifests = env.epic_manifests;
      if (stores.steam_root.empty()) {
        // Steam's install root is a per-user value of the signed-in person.
        const RegValue steam = registry.get("Software\\Valve\\Steam", "SteamPath");
        if (steam.kind == RegValue::Kind::text && !steam.text.empty()) stores.steam_root = fs::path(steam.text);
      }
      if (stores.epic_manifests.empty()) {
        const fs::path program_data = host_program_data();
        if (!program_data.empty()) stores.epic_manifests = program_data / "Epic" / "EpicGamesLauncher" / "Data" / "Manifests";
      }
      return stores;
    }
    IdentifyResult
    identified(HostEnv &env, const fs::path &path, std::string_view fallback_source = "manual") {
      MemoryRegistry local;
      IdentifyResult found;
      const bool signed_in = host_as_user([&] {
        Registry &registry = registry_of(env, local);
        found = identify_install(path, registry, storefront_env_of(env, registry), fallback_source);
      });
      if (!signed_in) found.status = Status::fail(409, "no_user_session", "Nobody is signed in on this PC");
      return found;
    }
    // Sparxie's own program folder is never a game location.
    bool
    is_sparxie_location(const HostEnv &env, const fs::path &path) {
      auto inside = [&](const fs::path &root) {
        if (root.empty()) return false;
        const std::string base = norm_path(root);
        const std::string text = norm_path(path);
        return text == base || text.rfind(base + "/", 0) == 0;
      };
      fs::path program = env.sparxie_exe.empty() ? fs::path {} : env.sparxie_exe.parent_path();
      if (program.filename().string().rfind("app-", 0) == 0) program = program.parent_path();
      return inside(env.component_dir) || inside(program);
    }
    ApiResult
    failure(Status status, json extra = json::object()) {
      ApiResult result;
      result.status = status;
      result.body = std::move(extra);
      result.body["ok"] = false;
      result.body["error"] = status.error;
      result.body["error_code"] = status.error_code;
      return result;
    }
    std::optional<Install>
    install_by_id(HostEnv &env, const std::string &id) {
      const json apps = read_apps(env.apps_file);
      for (const auto &app : apps.value("apps", json::array())) {
        if (!app.contains("sparxie") || !app["sparxie"].is_object()) continue;
        const json &block = app["sparxie"];
        if (block.value("install", std::string()) != id && block.value("profile", std::string()) != id) continue;
        Install install;
        install.id = block.value("install", id);
        install.game = block.value("game", "");
        install.region = block.value("region", "");
        install.dir = block.value("dir", "");
        install.exe_path = block.value("exe", "");
        install.exe = fs::path(install.exe_path).filename().string();
        install.profile = block.value("profile", "");
        return install;
      }
      MemoryRegistry local;
      Registry &registry = registry_of(env, local);
      auto probe_hyp = [&](const HypKey &hyp) -> std::optional<Install> {
        const RegValue value = registry.get(hyp.key, "GameInstallPath");
        if (value.kind != RegValue::Kind::text || value.text.empty() || !fs::is_directory(value.text)) return std::nullopt;
        const IdentifyResult found = identified(env, value.text);
        if (found.status.ok && found.install.id == id) return found.install;
        return std::nullopt;
      };
      for (const auto &hyp : hyp_registry_keys(registry)) {
        if (auto found = probe_hyp(hyp)) return found;
      }
      for (const auto &store : find_storefront_installs(storefront_env_of(env, registry))) {
        const IdentifyResult found = identified(env, store.dir / store.exe);
        if (found.status.ok && found.install.id == id) return found.install;
      }
      return std::nullopt;
    }
  }  // namespace
  fs::path
  find_sparxie_exe(const fs::path &root) {
    if (root.empty()) return {};
    const fs::path candidates[] = {
      root / "Sparxie.exe",
      root / "Sparxie.Launcher.exe",
      root / "components" / "sparxie" / "Sparxie.exe",
      root / "components" / "sparxie" / "Sparxie.Launcher.exe",
    };
    for (const auto &candidate : candidates)
      if (fs::is_regular_file(candidate)) return candidate;
    return {};
  }
  fs::path
  resolve_data_dir(const fs::path &exe, const fs::path &override_dir, const fs::path &fallback) {
    if (!override_dir.empty()) return override_dir;
    if (!exe.empty()) {
      fs::path dir = exe.parent_path();
      if (dir.filename().string().rfind("app-", 0) == 0) dir = dir.parent_path();
      std::error_code ec;
      fs::create_directories(dir, ec);
      const fs::path probe = dir / "sparxie-write-probe";
      std::ofstream out(probe);
      if (out) {
        out.close();
        fs::remove(probe, ec);
        return dir;
      }
    }
    return fallback;
  }
  json
  launch_notice(const HostEnv &env) {
    std::ifstream in(env.host_data / "launch-notice.json", std::ios::binary);
    if (!in) return nullptr;
    try {
      json note;
      in >> note;
      if (!note.is_object() || !note.contains("message") || !note["message"].is_string()) return nullptr;
      return note;
    }
    catch (const std::exception &) {
      return nullptr;
    }
  }
  ApiResult
  component_status(HostEnv &env) {
    if (env.sparxie_exe.empty() && !env.component_dir.empty()) env.sparxie_exe = find_sparxie_exe(env.component_dir);
    if (env.sparxie_exe.empty()) env.sparxie_exe = find_sparxie_exe(env.install_root);
    const bool installed = !env.sparxie_exe.empty() && fs::is_regular_file(env.sparxie_exe);
    const ConfigView store = read_store(config_dir(env));
    std::string state = store.state;
    if (!installed && state == "empty" && !fs::exists(config_dir(env) / "config.json")) state = "unused";
    if (!store.status.ok) state = "unreadable";
    json warning = warning_body(env, installed);
    const std::vector<std::string> capabilities = installed ? component_capabilities(env.sparxie_exe) : std::vector<std::string> {};
    ApiResult result;
    result.body = {
      { "ok", true },
      { "installed", installed },
      { "running", installed && sparxie_open(env) },
      { "version", component_version(env.sparxie_exe) },
      { "protocol", "" },
      { "runtime", { { "hoyo", "" }, { "zzz", "" } } },
      { "capabilities", capabilities },
      // true: keyboard, mouse and gamepad applications run through Sparxie with its settings (fps unlock) and without the touch interface
      { "touchUiSwitch", std::find(capabilities.begin(), capabilities.end(), std::string(kCapTouchUiSwitch)) != capabilities.end() },
      { "exe", installed ? env.sparxie_exe.string() : std::string() },
      { "dataDir", config_dir(env).string() },
      { "dataDirProvisional", env.data_dir_provisional },
      { "launchTemplate", env.launch_template.empty() ? kDefaultLaunchTemplate : env.launch_template },
      { "launchTemplateProvisional", true },
      { "store", { { "state", state }, { "profiles", store.profile_count } } },
      { "warned", warning["warned"] },
      { "statement", warning["statement"] },
      { "statement_en", warning["statement_en"] },
      { "launchNotice", launch_notice(env) },
    };
    const json extra = install_status_fields(env);
    for (auto it = extra.begin(); it != extra.end(); ++it) result.body[it.key()] = it.value();
    return result;
  }
  ApiResult
  identify_path(HostEnv &env, const fs::path &path, const std::string &game, const std::string &region) {
    if (is_sparxie_location(env, path)) return failure(Status::fail(409, "is_sparxie", "That is Sparxie's own location, not a game"));
    const IdentifyResult found = identified(env, path);
    if (!found.status.ok) return failure(found.status, { { "has_game", false } });
    if (!game.empty()) {
      const auto allowed = executables_for(game, region);
      const bool name_fits = std::any_of(allowed.begin(), allowed.end(), [&](const std::string &name) { return ascii_lower(name) == ascii_lower(found.install.exe); });
      if (found.install.game != game || !name_fits) return failure(Status::fail(409, "wrong_game", "That is not the executable of the chosen game"), { { "allowed", allowed }, { "has_game", false } });
    }
    ApiResult result;
    result.status = found.status;
    result.body = { { "ok", true }, { "install", install_json(found.install) }, { "has_game", true } };
    return result;
  }
  ApiResult
  scan_folder(HostEnv &env, const json &request) {
    const std::string text = request.value("path", std::string());
    const std::string game = request.value("game", std::string());
    const std::string region = request.value("region", std::string());
    if (text.empty() || (!game.empty() && !is_game(game)) || (!region.empty() && !is_region(region))) return failure(Status::fail(400, "invalid_input", "A folder path is required"));
    const fs::path root = text;
    if (is_sparxie_location(env, root)) return failure(Status::fail(409, "is_sparxie", "That is Sparxie's own location, not a game"));
    ScanOptions options = env.scan;
    options.game = game;
    options.region = region;
    if (request.contains("depth") && request["depth"].is_number_integer()) options.max_depth = std::clamp(request["depth"].get<int>(), 0, env.scan.max_depth);
    options.skip.push_back(env.component_dir);
    if (!env.sparxie_exe.empty()) options.skip.push_back(env.sparxie_exe.parent_path());
    ScanResult scanned;
    std::error_code ec;
    bool folder = false;
    const bool signed_in = host_as_user([&] {
      folder = fs::is_directory(root, ec);
      if (folder) scanned = scan_for_games(root, options);
    });
    if (!signed_in) return failure(Status::fail(409, "no_user_session", "Nobody is signed in on this PC"));
    if (!folder) return failure(Status::fail(404, "not_found", "That folder does not exist"));
    json installs = json::array();
    for (const auto &match : scanned.matches) {
      const IdentifyResult found = identified(env, match.exe, "folder");
      if (!found.status.ok) continue;
      json row = install_json(found.install);
      row["depth"] = match.depth;
      installs.push_back(row);
    }
    json extra = { { "timed_out", scanned.timed_out }, { "truncated", scanned.truncated }, { "searched", scanned.folders }, { "depth", options.max_depth } };
    if (installs.empty()) return failure(Status::fail(404, "unrecognised", "No known game executable was found in that folder"), extra);
    ApiResult result;
    result.body = extra;
    result.body["ok"] = true;
    result.body["installs"] = installs;
    return result;
  }
  ApiResult
  browse_path(HostEnv &env, const json &request) {
    const std::string text = request.value("path", std::string());
    const json files = request.value("files", json());
    const bool executables = files.is_boolean() ? files.get<bool>() : (files.is_string() && files.get<std::string>() == "exe");
    // A network path would make the host reach out with the person's credentials.
    if (text.rfind("\\\\", 0) == 0 || text.rfind("//", 0) == 0) return failure(Status::fail(400, "invalid_input", "Network paths are not listed"));
    BrowseResult listed;
    const bool signed_in = host_as_user([&] { listed = browse_folder(text, executables, host_list_roots); });
    if (!signed_in) return failure(Status::fail(409, "no_user_session", "Nobody is signed in on this PC"));
    if (!listed.status.ok) return failure(listed.status);
    ApiResult result;
    result.body = browse_json(listed);
    return result;
  }
  ApiResult
  open_component(HostEnv &env) {
    if (env.sparxie_exe.empty() && !env.component_dir.empty()) env.sparxie_exe = find_sparxie_exe(env.component_dir);
    if (env.sparxie_exe.empty()) env.sparxie_exe = find_sparxie_exe(env.install_root);
    if (env.sparxie_exe.empty() || !fs::is_regular_file(env.sparxie_exe)) return failure(Status::fail(409, "absent", "Sparxie is not installed"));
    ApiResult result;
    result.body = { { "ok", true }, { "already_running", false } };
    if (sparxie_open(env)) {
      result.body["already_running"] = true;
      return result;
    }
    if (!env.launch_gui) return failure(Status::fail(409, "launch_unavailable", "This host cannot start Sparxie"));
    const Status started = env.launch_gui(env.sparxie_exe);
    if (!started.ok) return failure(started);
    return result;
  }
  ApiResult
  list_games(HostEnv &env) {
    MemoryRegistry local;
    Registry &registry = registry_of(env, local);
    json detected = json::array();
    // Every HoYoPlay and Bilibili launcher record, then every Steam and Epic
    // install. The same folder twice keeps the store row: the store is where
    // the person got it, the launcher registry only keeps it up to date.
    struct Row {
      fs::path path;
      std::string source;
      int order;
    };
    std::vector<Row> rows;
    auto add_row = [&](const fs::path &path, const std::string &source, int order) {
      for (std::size_t i = 0; i < rows.size(); ++i)
        if (paths_equal(rows[i].path, path)) {
          if (order < rows[i].order) rows[i] = Row { path, source, order };
          return;
        }
      rows.push_back(Row { path, source, order });
    };
    for (const auto &hyp : hyp_registry_keys(registry)) {
      const RegValue value = registry.get(hyp.key, "GameInstallPath");
      if (value.kind != RegValue::Kind::text || value.text.empty()) continue;
      add_row(fs::path(value.text), hyp.source, hyp.source == "bili" ? 3 : 2);
    }
    for (const auto &store : find_storefront_installs(storefront_env_of(env, registry))) add_row(store.dir, store.source, store.source == "steam" ? 0 : 1);
    // The official Hypergryph launchers' own records: their games folders hold
    // Arknights and Endfield. A store install of the same folder keeps its
    // store row (order 4 loses against every store and launcher record).
    // Without a test's own answer the machine's records are read, but only when this environment reads the machine's registry too (a test registry means a test machine).
    std::vector<fs::path> records;
    if (env.uninstall_locations)
      records = env.uninstall_locations(hypergryph_uninstall_keys());
    else if (env.registry == &host_registry())
      records = host_uninstall_locations(hypergryph_uninstall_keys());
    std::vector<LauncherInstall> launchers = hypergryph_launchers_from_records(records);
    for (const auto &root : env.hypergryph_roots) launchers.push_back({ root, "cn" });
    for (const auto &game : hypergryph_launcher_games(launchers)) add_row(game.dir, "official-launcher", 4);
    for (const auto &row : rows) {
      if (!fs::is_directory(row.path)) continue;
      const IdentifyResult found = identified(env, row.path, row.source);
      if (found.status.ok && found.install.source == row.source) detected.push_back(install_json(found.install));
    }
    json stored = json::array();
    const ConfigView store = read_store(config_dir(env));
    if (store.status.ok) {
      for (const auto &profile : store.document.value("profiles", json::array())) {
        const std::string exe = profile.value("executablePath", std::string());
        if (exe.empty()) continue;
        IdentifyResult found = identified(env, exe);
        if (!found.status.ok) continue;
        found.install.profile = profile.value("id", std::string());
        stored.push_back(install_json(found.install));
      }
    }
    const ApiResult status = component_status(env);
    ApiResult result;
    result.body = { { "ok", true }, { "installed", status.body.value("installed", false) }, { "detected", detected }, { "stored", stored }, { "catalog", catalog_json(!env.zzz_magics.empty()) }, { "art", art_versions(env) } };
    return result;
  }
  ApiResult
  list_profiles(HostEnv &env) {
    MemoryRegistry local;
    Registry &registry = registry_of(env, local);
    const ConfigView store = read_store(config_dir(env));
    ApiResult result;
    if (!store.status.ok && fs::exists(config_dir(env) / "config.json")) {
      result.status = store.status;
      result.body = { { "ok", false }, { "error", store.status.error }, { "error_code", store.status.error_code } };
      return result;
    }
    const json apps = read_apps(env.apps_file);
    json profiles = json::array();
    for (const auto &profile : store.document.value("profiles", json::array())) {
      json row = { { "id", profile.value("id", std::string()) }, { "name", profile.value("displayName", std::string()) }, { "game", from_sparxie_game(profile.value("game", std::string())) }, { "taken", profile_taken(apps, profile.value("id", std::string())) }, { "install", nullptr } };
      const std::string exe = profile.value("executablePath", std::string());
      if (!exe.empty()) {
        IdentifyResult found = identified(env, exe);
        if (found.status.ok) {
          found.install.profile = row["id"].get<std::string>();
          row["install"] = install_json(found.install);
          if (row["game"].get<std::string>().empty()) row["game"] = found.install.game;
        }
      }
      profiles.push_back(row);
    }
    result.body = { { "ok", true }, { "installed", component_status(env).body.value("installed", false) }, { "profiles", profiles } };
    return result;
  }
}  // namespace sparxie

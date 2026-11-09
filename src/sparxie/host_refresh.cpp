#include "src/sparxie/art_sources.h"
#include "src/sparxie/host.h"
#include "src/sparxie/icon_file.h"
#include "src/sparxie/svg_path.h"
#include "src/sparxie/region.h"
#include "src/sparxie/zip_extract.h"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
namespace sparxie {
  namespace {
    json
    read_apps_file(const fs::path &path) {
      json body = { { "apps", json::array() } };
      std::ifstream in(path);
      if (!in) return body;
      try {
        json parsed;
        in >> parsed;
        if (parsed.is_object()) body = parsed;
      }
      catch (const std::exception &) {
      }
      if (!body.contains("apps") || !body["apps"].is_array()) body["apps"] = json::array();
      return body;
    }
    void
    keep_bytes(const fs::path &path, const std::string &bytes) {
      std::error_code ec;
      if (fs::exists(path, ec)) return;
      fs::create_directories(path.parent_path(), ec);
      std::ofstream out(path, std::ios::binary);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    // the bytes of a file, read as they are: on Windows a stream opened without std::ios::binary turns CR LF into LF and stops at a 0x1A byte, which cut the cached
    // lossless WebP of Star Rail short (the decoder then returned nothing, and the hero was composed black). The index.json is text and reads the same either way.
    std::string
    file_text(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) return {};
      return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // Bumped when the sources of a title's pictures change (a new logo source, a clean background): a poster cached by an older revision is made again even when
    // the background's own version did not change.
    constexpr int kArtRevision = 3;
    // Bumped when the portrait cover's composition changes (logo size and place): the composed covers are made again from the cached pictures, nothing is fetched.
    constexpr int kCoverRevision = 1;

    // Largest picture the refresh takes from a launcher or a store: a 4K key art is a few MB, so this is far above any real answer and still bounded.
    constexpr std::size_t kMaxPictureBytes = 24u * 1024u * 1024u;

    // A picture's bytes; empty when the request failed, answered nothing or answered an unreasonable amount.
    std::string
    fetch_picture(const std::string &url, const std::function<std::optional<std::string>(const std::string &)> &get_url) {
      if (url.rfind("https://", 0) != 0) return {};
      const auto bytes = get_url(url);
      if (!bytes || bytes->empty() || bytes->size() > kMaxPictureBytes) return {};
      return *bytes;
    }

    // The one picture inside a downloaded zip (the NTE launcher ships its pictures as <name>.zip): written under host-data/tmp, listed, size-checked, extracted by the
    // host's own extractor (names checked, CRC checked) and read back. Empty when it is not a sound zip of one reasonable file.
    std::string
    first_file_of_zip(const HostEnv &env, const std::string &zip_bytes) {
      std::error_code ec;
      static std::atomic<unsigned> counter { 0 };
      const fs::path work = env.host_data / "tmp" / ("art-zip-" + std::to_string(counter.fetch_add(1)) + "-" + std::to_string(zip_bytes.size()));
      fs::remove_all(work, ec);
      fs::create_directories(work / "out", ec);
      std::string picture;
      const fs::path archive = work / "picture.zip";
      {
        std::ofstream out(archive, std::ios::binary);
        out.write(zip_bytes.data(), static_cast<std::streamsize>(zip_bytes.size()));
      }
      const zip::Outcome seen = zip::inspect(archive);
      if (seen.ok && seen.entries == 1 && seen.uncompressed > 0 && seen.uncompressed <= kMaxPictureBytes && zip::extract(archive, work / "out").ok) {
        for (const auto &entry : fs::directory_iterator(work / "out", ec))
          if (entry.is_regular_file(ec)) {
            picture = file_text(entry.path());
            break;
          }
      }
      fs::remove_all(work, ec);
      return picture;
    }

    // the bytes of the newest picture of one kind ("background", "logo") kept in posters/<install>/ by an earlier refresh
    std::optional<std::string>
    cached_picture(const HostEnv &env, const std::string &install_id, const std::string &kind) {
      std::error_code ec;
      const fs::path dir = env.host_data / "posters" / install_id;
      fs::path newest;
      fs::file_time_type stamp;
      const std::string suffix = "-" + kind + ".bin";
      for (const auto &entry : fs::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.size() < suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        if (newest.empty() || entry.last_write_time() > stamp) {
          newest = entry.path();
          stamp = entry.last_write_time();
        }
      }
      if (newest.empty()) return std::nullopt;
      const std::string bytes = file_text(newest);
      if (bytes.empty()) return std::nullopt;
      return bytes;
    }
  }  // namespace
  ApiResult
  refresh_official(HostEnv &env, const std::function<std::optional<std::string>(const std::string &)> &get_url, const std::function<Rgba(std::string_view)> &decode, const std::function<std::optional<std::string>(const std::string &, const std::string &)> &post_url) {
    ApiResult result;
    if (!get_url || !decode) {
      result.body = { { "ok", true }, { "updated", 0 }, { "offline", true } };
      return result;
    }
    struct Item {
      std::string id;
      std::string game;
      std::string region;
      fs::path dir;  // the detected install's folder (empty when none is on this machine)
      std::string exe;  // and its executable's file name
    };
    std::vector<Item> items;
    auto add = [&](const std::string &id, const std::string &game, const std::string &region, const std::string &dir = {}, const std::string &exe = {}) {
      if (id.empty() || game.empty() || region.empty()) return;
      for (auto &item : items)
        if (item.id == id) {
          if (item.dir.empty() && !dir.empty()) {
            item.dir = fs::path(dir);
            item.exe = exe;
          }
          return;
        }
      items.push_back({ id, game, region, dir.empty() ? fs::path() : fs::path(dir), exe });
    };
    for (const auto &install : list_games(env).body.value("detected", json::array()))
      add(install.value("id", std::string()), install.value("game", std::string()), install.value("region", std::string()), install.value("dir", std::string()), install.value("exe", std::string()));
    for (const auto &app : read_apps_file(env.apps_file).value("apps", json::array())) {
      if (!app.contains("sparxie") || !app["sparxie"].is_object()) continue;
      const json &block = app["sparxie"];
      add(block.value("install", std::string()), block.value("game", std::string()), block.value("region", std::string()));
    }
    // The poster of every game and region is public and needs no install: the pages show it on the game tiles of 新建 Sparxie 应用.
    for (const auto game : kGames)
      // Every region a title ships: the HoYoverse titles answer to the
      // launcher's API, the others to their own art sources (or the
      // placeholder, which the loop below publishes the same way).
      for (const auto region : kRegions)
        if (region_row(game, region) || !executables_for(game, region).empty()) add(std::string(game) + "-" + std::string(region), std::string(game), std::string(region));
    // the bilibili launcher's carousel picture already carries its own logo and text, so a bilibili hero is made from the key art of the national install; the national
    // installs go first so that it is cached
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return (a.region == "bili") < (b.region == "bili"); });
    int updated = 0;
    int heroes = 0;
    std::vector<std::string> touched;
    json skipped = json::array();
    json sources = json::object();
    std::map<std::string, std::string> icon_sources_used;
    for (const auto &item : items) {
      ArtUrls urls;
      Rgba background;
      Rgba logo;
      Rgba icon;
      Rgba capsule;
      Rgba placeholder_wide;
      // One cache per install id: the national and bilibili clients share a biz token but have their own launcher posters.
      const fs::path cache = env.host_data / "posters" / item.id;
      const fs::path index = cache / "index.json";
      json previous = json::object();
      try {
        previous = json::parse(file_text(index));
      }
      catch (const std::exception &) {
      }
      const std::string previous_version = previous.is_object() ? previous.value("version", std::string()) : std::string();
      const bool hoyo = region_row(item.game, item.region) != nullptr;
      // the icon (not for HoYoverse titles: their launcher API answers one): the install's own program icon, else the store's or the publisher's, else a monogram
      Rgba icon_now;
      std::string icon_label;
      bool icon_kept = false;
      if (!hoyo) {
        const std::string icon_source_before = previous.is_object() ? previous.value("iconSource", std::string()) : std::string();
        std::error_code icon_ec;
        const bool icon_current = previous.is_object() && previous.value("iconRev", 0) == kIconRevision && fs::is_regular_file(cache / "icon.png", icon_ec);
        const auto program_icon = [&](const fs::path &dir, const std::string &exe_name) {
          if (dir.empty()) return Rgba {};
          // the detected executable first, then the title's other known executables in the same folder (an Unreal title's launcher sits beside the shipping binary)
          std::vector<std::string> names;
          if (!exe_name.empty()) names.push_back(exe_name);
          for (const auto &name : executables_for(item.game, item.region))
            if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
          for (const auto &name : names) {
            const fs::path file = dir / name;
            if (!fs::is_regular_file(file, icon_ec)) continue;
            // judged at the size the program carries: a small icon enlarged is still small
            const Rgba raw = icon_from_pe(file, decode);
            if (icon_is_usable(raw)) return icon_square(raw);
          }
          return Rgba {};
        };
        icon_now = program_icon(item.dir, item.exe);
        if (icon_now.w > 0) icon_label = "exe";
        if (icon_now.w == 0 && icon_current && !icon_source_before.empty() && icon_source_before != "monogram") {
          // a real icon made earlier stays (nothing is fetched again until the revision changes)
          icon_kept = true;
          icon_label = icon_source_before;
        }
        if (icon_now.w == 0 && !icon_kept)
          for (const auto &source : icon_sources(item.game, item.region, get_url)) {
            const std::string url = source.url();
            const std::string bytes = url.empty() ? std::string() : fetch_picture(url, get_url);
            if (bytes.empty()) continue;
            const Rgba raw = icon_from_file_bytes(bytes, decode);
            if (!icon_is_usable(raw)) continue;
            icon_now = icon_square(raw);
            icon_label = source.label;
            break;
          }
        if (icon_now.w == 0 && !icon_kept) {
          // another region of the same title that is installed here: the same game's program icon
          for (const auto &other : items)
            if (other.game == item.game && !other.dir.empty() && other.id != item.id) {
              icon_now = program_icon(other.dir, other.exe);
              if (icon_now.w > 0) {
                icon_label = "exe-sibling";
                break;
              }
            }
        }
        if (icon_now.w == 0 && !icon_kept) {
          icon_now = placeholder_icon(item.game);
          icon_label = "monogram";
        }
      }
      // what an item that is otherwise left alone still gets: its icon and the record of how it was made
      const auto settle_icon = [&](const std::string &art_version, const std::string &art_url) {
        if (hoyo) return;
        if (!icon_kept) publish_icon(env, item.id, icon_now);
        std::error_code mk;
        fs::create_directories(cache, mk);
        json record = previous.is_object() ? previous : json::object();
        if (!art_version.empty()) {
          record["version"] = art_version;
          record["url"] = art_url;
        }
        record["artRev"] = kArtRevision;
        record["iconRev"] = kIconRevision;
        record["iconSource"] = icon_label;
        std::ofstream out(index);
        out << record.dump(2);
        icon_sources_used[item.id] = icon_label;
      };
      if (!hoyo) {
        // The other publishers' art: their launcher's content API, the store's
        // public answer, or the game's own site: the sources in order, the first
        // whose picture downloads and decodes. With none left the title's own
        // placeholder card is published the same way (never the generic box art).
        const auto sourced = vendor_sources(item.game, item.region, get_url, post_url);
        bool unchanged = false;
        std::string label;
        for (const auto &source : sourced) {
          ArtUrls candidate = source.resolve();
          if (candidate.background.empty()) continue;
          // a title whose cached art is of this very version (and was made by this revision of the sources) is left alone, as for the HoYoverse posters
          if (!candidate.version.empty() && previous_version == candidate.version && previous.value("artRev", 0) == kArtRevision) {
            unchanged = true;
            break;
          }
          std::string bytes = fetch_picture(candidate.background, get_url);
          if (bytes.empty()) continue;
          if (art_background_zipped(candidate)) bytes = first_file_of_zip(env, bytes);
          Rgba decoded = bytes.empty() ? Rgba {} : decode(std::string_view(bytes));
          // a decoder that answered nothing, or one colour, is a failed decode: the next source or the placeholder (the same guard as every title)
          if (image_is_blank(decoded)) continue;
          keep_bytes(cache / (candidate.version + "-background.bin"), bytes);
          background = std::move(decoded);
          if (candidate.logo.empty() && !candidate.logo_svg_path.empty()) {
            // a wordmark drawn from the site's inline SVG: rasterised here (white on transparent, 1016 px wide) and kept as a PNG like any other logo
            const Rgba drawn = rasterize_svg_path(candidate.logo_svg_path, candidate.logo_svg_w, candidate.logo_svg_h, 1016);
            const auto drawn_png = encode_png(drawn);
            if (!drawn_png.empty()) {
              logo = drawn;
              keep_bytes(cache / (candidate.version + "-logo.bin"), std::string(drawn_png.begin(), drawn_png.end()));
            }
          }
          if (!candidate.logo.empty()) {
            const std::string logo_bytes = fetch_picture(candidate.logo, get_url);
            if (!logo_bytes.empty()) {
              logo = decode(std::string_view(logo_bytes));
              if (logo.w > 0) keep_bytes(cache / (candidate.version + "-logo.bin"), logo_bytes);
            }
          }
          if (!candidate.cover.empty()) {
            const std::string cover_bytes = fetch_picture(candidate.cover, get_url);
            if (!cover_bytes.empty()) capsule = decode(std::string_view(cover_bytes));
          }
          urls = std::move(candidate);
          label = source.label;
          break;
        }
        if (unchanged) {
          settle_icon({}, {});
          continue;
        }
        if (background.w == 0) {
          // A real picture published earlier is never replaced by the placeholder (the sources did not answer this time).
          if (!previous_version.empty()) {
            if (previous_version != "placeholder" || sourced.empty()) {
              if (!sourced.empty()) skipped.push_back(item.id);
              settle_icon({}, {});
              continue;
            }
          }
          background = placeholder_background(item.game);
          placeholder_wide = placeholder_hero(item.game);
          urls = {};
          urls.version = "placeholder";
          if (previous_version == urls.version && previous.value("artRev", 0) == kArtRevision) {
            settle_icon({}, {});
            continue;
          }
          if (!sourced.empty()) skipped.push_back(item.id);
          label = "placeholder";
        }
        sources[item.id] = label;
        icon_sources_used[item.id] = icon_label;
      }
      if (hoyo) {
        std::string host = "https://hyp-api.mihoyo.com";
        std::string launcher = "jGHBHlcOq1";
        std::string language = "zh-cn";
        if (item.region == "global") {
          host = "https://sg-hyp-api.hoyoverse.com";
          launcher = "VYTpXlbWo8";
          language = "en-us";
        }
        else if (item.region == "bili" && item.game == "starrail")
          launcher = "6P5gHMNyK3";
        else if (item.region == "bili" && item.game == "zzz")
          launcher = "xV0f4r1GT0";
        else if (item.region == "bili")
          launcher = "umfgRO5gh5";
        const std::string games_url = host + "/hyp/hyp-connect/api/getGames?launcher_id=" + launcher + "&language=" + language;
        const std::string basic_url = host + "/hyp/hyp-connect/api/getAllGameBasicInfo?launcher_id=" + launcher + "&language=" + language;
        const auto games_body = get_url(games_url);
        if (!games_body) {
          skipped.push_back(item.id);
          continue;
        }
        json games_json;
        json basic_json;
        try {
          games_json = json::parse(*games_body);
        }
        catch (const std::exception &) {
          skipped.push_back(item.id);
          continue;
        }
        if (const auto basic_body = get_url(basic_url)) {
          try {
            basic_json = json::parse(*basic_body);
          }
          catch (const std::exception &) {
          }
        }
        const std::string biz = biz_for(item.game, item.region);
        urls = art_urls_from_payload(games_json, basic_json, biz, item.region == "bili");
        // an install cached before the larger launcher logo was kept is fetched again once (its version did not change, its hero logo was the small one)
        if (!urls.version.empty() && previous_version == urls.version && previous.value("heroLogo", 0) == 1) continue;
        if (urls.background.empty()) {
          skipped.push_back(item.id);
          continue;
        }
        const auto background_bytes = get_url(urls.background);
        if (!background_bytes) {
          skipped.push_back(item.id);
          continue;
        }
        background = decode(std::string_view(*background_bytes));
        if (image_is_blank(background)) {
          // the decoder gave nothing (or one colour) for this picture: nothing is published or cached from it, the poster and cover on disk stay
          if (env.warn) env.warn("the official background of " + item.id + " decoded blank (" + std::to_string(background_bytes->size()) + " bytes from " + urls.background + ")");
          skipped.push_back(item.id);
          continue;
        }
        keep_bytes(cache / (urls.version + "-background.bin"), *background_bytes);
        if (!urls.logo.empty()) {
          if (const auto logo_bytes = get_url(urls.logo)) {
            logo = decode(std::string_view(*logo_bytes));
            keep_bytes(cache / (urls.version + "-logo.bin"), *logo_bytes);
          }
        }
        if (!urls.logo_large.empty()) {
          if (const auto large_bytes = get_url(urls.logo_large)) keep_bytes(cache / (urls.version + "-herologo.bin"), *large_bytes);
        }
        if (!urls.icon.empty()) {
          if (const auto icon_bytes = get_url(urls.icon)) {
            icon = decode(std::string_view(*icon_bytes));
            keep_bytes(cache / (urls.version + "-icon.bin"), *icon_bytes);
          }
        }
      }
      const ApiResult published = publish_art(env, item.id, urls, background, urls.logo_cover_only ? Rgba {} : logo, hoyo ? icon : (icon_kept ? Rgba {} : icon_now), capsule.w > 0 ? &capsule : nullptr);
      if (!published.status.ok) {
        skipped.push_back(item.id);
        continue;
      }
      touched.push_back(item.id);
      // a placeholder has no key art to build the hero from: its own landscape card is the hero
      if (placeholder_wide.w > 0) publish_hero(env, item.id, placeholder_wide, {});
      std::error_code ec;
      fs::create_directories(index.parent_path(), ec);
      if (published.body.contains("path") && published.body["path"].is_string()) {
        std::error_code ec;
        const fs::path composed = published.body["path"].get<std::string>();
        if (fs::is_regular_file(composed)) {
          fs::copy_file(composed, cache / (urls.version + "-cover.png"), fs::copy_options::skip_existing, ec);
        }
      }
      std::ofstream out(index);
      // the cover is the store's finished capsule, a placeholder card, or composed here from the key art and the logo (only a composed one is made again when the composition changes)
      const std::string cover_kind = capsule.w > 0 ? "capsule" : urls.version == "placeholder" ? "placeholder" : "composed";
      json record = { { "version", urls.version }, { "url", urls.background }, { "heroLogo", 1 }, { "coverKind", cover_kind }, { "coverRev", cover_kind == "composed" ? 0 : kCoverRevision }, { "logoCoverOnly", urls.logo_cover_only } };
      if (!hoyo) {
        record["artRev"] = kArtRevision;
        record["iconRev"] = kIconRevision;
        record["iconSource"] = icon_label;
      }
      out << record.dump(2);
      ++updated;
    }
    // The heroes: from the pictures kept in posters/<install>/, for an install fetched this run, for the national or bilibili sibling of one (they share a logo and a key art),
    // and for one whose hero.jpg is missing (a cache made before the landscape poster, or its PNG round). Nothing is fetched here, and a hero that comes out the same is not rewritten.
    const auto cached_logo = [&](const std::string &id) {
      for (const char *kind : { "herologo", "logo" })
        if (const auto bytes = cached_picture(env, id, kind)) {
          Rgba logo = decode(std::string_view(*bytes));
          if (logo.w > 0 && logo.h > 0) return logo;
        }
      return Rgba {};
    };
    // a plate logo (old zzz) loses to a transparent one; otherwise the logo with the most visible pixels is the sharper one
    const auto logo_score = [](const Rgba &logo) { return logo_visible_area(logo) / (logo_has_plate(logo) ? 4 : 1); };
    const auto is_touched = [&](const std::string &id) { return std::find(touched.begin(), touched.end(), id) != touched.end(); };
    for (const auto &item : items) {
      const std::string sibling = item.region == "cn" ? item.game + "-bili" : item.region == "bili" ? item.game + "-cn" : std::string();
      std::error_code exists_ec;
      const fs::path hero_dir = env.host_data / "posters" / item.id;
      // the plain and the badged hero must both be there and show a picture (a hero.jpg without its hero-badge.jpg is a poster made before the Sparxie badge); else composed again
      bool hero_ok = true;
      for (const char *name : { "hero.jpg", "hero-badge.jpg" }) {
        const fs::path hero_file = hero_dir / name;
        if (!fs::is_regular_file(hero_file, exists_ec)) {
          hero_ok = false;
          continue;
        }
        std::ifstream in(hero_file, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (jpeg_is_blank(bytes)) {
          hero_ok = false;
          if (env.warn) env.warn("the landscape poster " + std::string(name) + " of " + item.id + " is blank: composing it again");
        }
      }
      // a composed cover made by an older composition is made again from the cached pictures
      json index_now = json::object();
      try {
        index_now = json::parse(file_text(hero_dir / "index.json"));
      }
      catch (const std::exception &) {
      }
      const std::string index_cover_kind = index_now.is_object() ? index_now.value("coverKind", std::string()) : std::string();
      const bool cover_stale = index_now.is_object() && index_now.value("coverRev", 0) != kCoverRevision && (index_cover_kind == "composed" || (index_cover_kind.empty() && region_row(item.game, item.region) != nullptr && !index_now.value("version", std::string()).empty()));
      if (!is_touched(item.id) && !(!sibling.empty() && is_touched(sibling)) && hero_ok && !cover_stale) continue;
      auto background_bytes = cached_picture(env, item.id, "background");
      if (item.region == "bili")
        if (const auto national = cached_picture(env, item.game + "-cn", "background")) background_bytes = national;
      if (!background_bytes) continue;
      const Rgba background = decode(std::string_view(*background_bytes));
      if (image_is_blank(background)) continue;
      Rgba logo = cached_logo(item.id);
      if (!sibling.empty()) {
        const Rgba other = cached_logo(sibling);
        if (other.w > 0 && logo_score(other) > logo_score(logo)) logo = other;
      }
      // a title whose key art shows its painted title where the hero is cut gets its logo on the portrait cover only
      const bool cover_only = index_now.is_object() && index_now.value("logoCoverOnly", false);
      if (publish_hero(env, item.id, background, cover_only ? Rgba {} : logo) >= 0) ++heroes;
      // the portrait cover carries the very logo of the hero, laid on by the same routine
      if (index_cover_kind != "capsule" && index_cover_kind != "placeholder" && !index_now.value("version", std::string()).empty() && index_now.value("version", std::string()) != "placeholder") {
        publish_cover(env, item.id, index_now.value("version", std::string()), background, logo);
        index_now["coverKind"] = "composed";
        index_now["coverRev"] = kCoverRevision;
        std::ofstream index_out(hero_dir / "index.json");
        index_out << index_now.dump(2);
      }
    }
    // the badged covers of the installs whose cover was made before the badge (nothing is fetched or composed for the others)
    for (const auto &item : items) publish_cover_badge(env, item.id);
    result.body = { { "ok", true }, { "updated", updated }, { "heroes", heroes }, { "skipped", skipped }, { "sources", sources }, { "icons", icon_sources_used } };
    return result;
  }
}  // namespace sparxie

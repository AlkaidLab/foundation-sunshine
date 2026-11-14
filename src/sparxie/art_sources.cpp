#include "src/sparxie/art_sources.h"

#include "src/sparxie/sha256.h"
#include "src/sparxie/svg_path.h"

#include <cctype>

namespace sparxie {
  namespace {
    constexpr const char *kBatchProxyUrl = "https://launcher.hypergryph.com/api/proxy/web/batch_proxy";
    constexpr const char *kKuroCnHost = "https://prod-cn-alicdn-gamestarter.kurogame.com";
    constexpr const char *kKuroGlobalHost = "https://prod-alicdn-gamestarter.kurogame.com";
    constexpr const char *kSteamAssets = "https://shared.akamai.steamstatic.com/store_item_assets/";
    constexpr const char *kSteamCommunityIcons = "https://shared.akamai.steamstatic.com/community_assets/images/apps/";
    constexpr const char *kNteVersionIni = "https://yhcdn1.wmupd.com/hd/publish_ob/launcher/Version.ini";
    constexpr const char *kNteSite = "https://yh.wanmei.com/";
    // The national Arknights site draws its wordmark (明日方舟 / ARKNIGHTS) from an inline SVG definition in its home page: the only official logo of the title
    // that is public (Arknights is not on Steam); viewBox 0 0 254 119.
    constexpr const char *kArknightsSite = "https://ak.hypergryph.com/";
    constexpr const char *kArknightsLogoId = "svg_def-title_arknights";
    constexpr const char *kYostarArknightsIcon = "https://webusstatic.yo-star.com/arknights-us/arknights-us-website/main/h5/favicon.ico";
    // the launcher's own resource folder of the background picture in NTE's file list
    constexpr const char *kNtePicturePath = "/ResFilesM/1289/bgimgs/bg_0.png";

    // One row per title and region that has official art. A region that is
    // not listed (arknights global) has no sourced official art: the
    // placeholder covers it. Every value is documented in docs/sparxie-games.md.
    enum class Family { hypergryph, kuro, steam, nte_launcher };

    struct SourceRow {
      const char *game;
      const char *region;
      Family family;
      const char *appcode;  // hypergryph: the launcher's game code
      const char *kuro_host;  // kuro: the launcher CDN of the region
      const char *kuro_key;  // kuro: <id>_<key>
      const char *kuro_app;  // kuro: the app code (G152...)
      const char *kuro_language;
      int steam_id;  // the store build: the icon source of a kuro row, the second choice of a title whose launcher picture is a webp, and its own row
    };


    const std::vector<SourceRow> &
    source_table() {
      static const std::vector<SourceRow> table = {
        { "endfield", "cn", Family::hypergryph, "6LL0KJuqHBVz33WK", "", "", "", "", 4732690 },
        { "endfield", "bili", Family::hypergryph, "6LL0KJuqHBVz33WK", "", "", "", "", 4732690 },
        { "endfield", "global", Family::steam, "", "", "", "", "", 4732690 },
        { "arknights", "cn", Family::hypergryph, "GzD1CpaWgmSq1wew", "", "", "", "", 0 },
        { "arknights", "bili", Family::hypergryph, "GzD1CpaWgmSq1wew", "", "", "", "", 0 },
        { "wuwa", "cn", Family::kuro, "", kKuroCnHost, "10003_Y8xXrXk65DqFHEDgApn3cpK5lfczpFx5", "G152", "zh-Hans", 0 },
        { "wuwa", "bili", Family::kuro, "", kKuroCnHost, "10003_Y8xXrXk65DqFHEDgApn3cpK5lfczpFx5", "G152", "zh-Hans", 0 },
        { "wuwa", "global", Family::kuro, "", kKuroGlobalHost, "50004_obOHXFrFanqsaIEOmuKroCcbZkQRBC7c", "G153", "en", 3513350 },
        { "pgr", "cn", Family::kuro, "", kKuroCnHost, "10011_qYQv6TyyyhCKD3ox3gssyolNPwMoCPZt", "G148", "zh-Hans", 0 },
        { "pgr", "global", Family::kuro, "", kKuroGlobalHost, "50015_LWdk9D2Ep9mpJmqBZZkcPBU2YNraEWBQ", "G143", "en", 4125930 },
        { "nte", "cn", Family::nte_launcher, "", "", "", "", "", 0 },
        { "nte", "bili", Family::nte_launcher, "", "", "", "", "", 0 },
        { "nte", "global", Family::steam, "", "", "", "", "", 4508340 },
      };
      return table;
    }

    std::string
    attribute_of(const std::string &tag, const char *name) {
      std::string marker(name);
      marker += "=\"";
      auto at = tag.find(marker);
      if (at == std::string::npos) return {};
      const auto end = tag.find('"', at + marker.size());
      if (end == std::string::npos) return {};
      return tag.substr(at + marker.size(), end - at - marker.size());
    }

    std::string
    url_encoded(const std::string &text) {
      static const char *hex = "0123456789ABCDEF";
      std::string out;
      for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
          out += static_cast<char>(c);
        else {
          out += '%';
          out += hex[c >> 4];
          out += hex[c & 15];
        }
      }
      return out;
    }

    // The first path segment after the app folder of a store asset ("<hash>/file").
    std::string
    steam_hash_of(const std::string &asset) {
      const auto slash = asset.find('/');
      return slash == std::string::npos ? std::string() : asset.substr(0, slash);
    }

    // The Steam app that carries a title's art whatever the region (the national clients have no store page, but the game, its logo and its icon are the same).
    int
    steam_app_of(const SourceRow &row) {
      if (row.steam_id) return row.steam_id;
      const std::string game = row.game;
      if (game == "endfield") return 4732690;
      if (game == "wuwa") return 3513350;
      if (game == "pgr") return 4125930;
      if (game == "nte") return 4508340;
      return 0;
    }

    // The mirror's answer for the title's Steam app; empty fields when it did not answer (the launcher sources carry on without a Steam logo).
    SteamPics
    pics_of(const SourceRow &row, const GetUrlFn &get_url) {
      const int app = steam_app_of(row);
      if (!app || !get_url) return {};
      const auto body = get_url(steam_pics_url(app));
      if (!body) return {};
      return steam_pics_art(*body, app, row.region);
    }

    // A logo is part of the picture's identity: a new logo on the same background is a new version.
    void
    add_logo(ArtUrls &urls, const SteamPics &pics) {
      if (pics.logo.empty()) return;
      urls.logo = pics.logo;
      urls.version += "-" + pics.logo_version;
    }

    // The clean variant of a title whose launcher picture has the title painted in: Steam's library hero (key art without the title) with the library logo to
    // lay over it, and the finished library capsule of the language as the cover.
    ArtUrls
    resolve_steam_clean(const SourceRow &row, const GetUrlFn &get_url) {
      const SteamPics pics = pics_of(row, get_url);
      if (pics.hero.empty() || pics.logo.empty()) return {};
      ArtUrls urls;
      urls.background = pics.hero;
      urls.logo = pics.logo;
      urls.cover = pics.capsule;
      urls.version = pics.version;
      return urls;
    }

    ArtUrls
    resolve_hypergryph(const SourceRow &row, const GetUrlFn &get_url, const PostUrlFn &post_url) {
      if (!post_url) return {};
      const auto body = post_url(kBatchProxyUrl, batch_proxy_body(row.appcode));
      if (!body) return {};
      ArtUrls urls = art_from_batch_proxy(*body);
      if (urls.background.empty()) return {};
      // Arknights: the launcher's picture has the title painted in, but the portrait cover crops that part out, so the site's own wordmark is laid on every poster
      if (std::string(row.game) == "arknights" && get_url)
        if (const auto page = get_url(kArknightsSite)) {
          const std::string path = svg_path_of_page(*page, kArknightsLogoId);
          if (!path.empty()) {
            urls.logo_svg_path = path;
            urls.logo_svg_w = 254;
            urls.logo_svg_h = 119;
            urls.logo_cover_only = true;
            urls.version += "-" + sha256::hash(path).substr(0, 8);
          }
        }
      // Endfield: the launcher's picture has the title painted in and Steam's clean variant leads, so no logo is laid over this one
      return urls;
    }

    ArtUrls
    resolve_steam(const SourceRow &row, const GetUrlFn &get_url) {
      if (!get_url || !row.steam_id) return {};
      const auto body = get_url(steam_items_url(row.steam_id));
      if (!body) return {};
      ArtUrls urls = art_from_steam(*body);
      if (urls.background.empty()) return {};
      // the store capsule carries its own title; the hero gets the library logo laid over the page art
      urls.icon.clear();
      add_logo(urls, pics_of(row, get_url));
      return urls;
    }

    ArtUrls
    resolve_kuro(const SourceRow &row, const GetUrlFn &get_url) {
      if (!get_url) return {};
      const std::string base = std::string(row.kuro_host) + "/launcher/";
      const auto index = get_url(base + "launcher/" + row.kuro_key + "/" + row.kuro_app + "/index.json");
      if (!index) return {};
      const std::string id = kuro_background_id(*index);
      if (id.empty()) return {};
      const auto background = get_url(base + row.kuro_key + "/" + row.kuro_app + "/background/" + id + "/" + row.kuro_language + ".json");
      if (!background) return {};
      ArtUrls urls = art_from_kuro_background(*background);
      if (urls.background.empty()) return {};
      urls.icon.clear();
      // the launcher's slogan is the current version banner, not the game's name: Steam's library logo is the mark of the title (the slogan stays when Steam did not answer)
      const SteamPics pics = pics_of(row, get_url);
      if (!pics.logo.empty()) {
        urls.logo.clear();
        urls.version = art_version_of(urls.background);
        add_logo(urls, pics);
      }
      return urls;
    }

    ArtUrls
    resolve_nte_launcher(const GetUrlFn &get_url) {
      if (!get_url) return {};
      const auto ini = get_url(kNteVersionIni);
      if (!ini) return {};
      const std::string list_url = ini_value(*ini, "FileListURL");
      if (list_url.rfind("https://", 0) != 0) return {};
      const auto list = get_url(list_url);
      if (!list) return {};
      ArtUrls urls = art_from_nte_file_list(*list);
      if (urls.background.empty()) return {};
      // the wordmark comes from the game's own site
      if (const auto page = get_url(kNteSite)) urls.logo = page_image_url(*page, "logo.png");
      return urls;
    }
  }  // namespace

  std::string
  batch_proxy_body(const std::string &appcode) {
    // The body the official launcher itself sends (kind get_main_bg_image;
    // the answer's main_bg_image.url is the launcher's current background).
    const json request = { { "proxy_reqs", json::array({ { { "kind", "get_main_bg_image" }, { "get_main_bg_image_req", { { "appcode", appcode }, { "language", "zh-cn" }, { "channel", "1" }, { "sub_channel", "1" }, { "platform", "Windows" }, { "source", "launcher" } } } } }) } };
    return request.dump();
  }

  ArtUrls
  art_from_batch_proxy(const std::string &body) {
    ArtUrls urls;
    try {
      const json parsed = json::parse(body);
      for (const auto &rsp : parsed.value("proxy_rsps", json::array())) {
        if (rsp.value("kind", std::string()) != "get_main_bg_image") continue;
        const json image = rsp.value("get_main_bg_image_rsp", json::object()).value("main_bg_image", json::object());
        urls.background = image.value("url", std::string());
        break;
      }
    }
    catch (const std::exception &) {
      return {};
    }
    urls.version = art_version_of(urls.background);
    return urls;
  }

  std::string
  kuro_background_id(const std::string &index_json) {
    try {
      const json parsed = json::parse(index_json);
      const json code = parsed.value("functionCode", json::object());
      return code.value("background", std::string());
    }
    catch (const std::exception &) {
      return {};
    }
  }

  ArtUrls
  art_from_kuro_background(const std::string &background_json) {
    ArtUrls urls;
    try {
      const json parsed = json::parse(background_json);
      if (parsed.value("functionSwitch", 1) == 0) return {};
      urls.background = parsed.value("firstFrameImage", std::string());
      urls.logo = parsed.value("slogan", std::string());
    }
    catch (const std::exception &) {
      return {};
    }
    // a picture is only taken from the launcher's own CDN over TLS
    if (urls.background.rfind("https://", 0) != 0) return {};
    if (urls.logo.rfind("https://", 0) != 0) urls.logo.clear();
    // the logo is part of the picture's identity: a new slogan on the same background is a new version
    urls.version = art_version_of(urls.background);
    if (!urls.logo.empty()) urls.version += "-" + art_version_of(urls.logo);
    return urls;
  }

  std::string
  steam_pics_url(int app_id) {
    return "https://api.steamcmd.net/v1/info/" + std::to_string(app_id);
  }

  SteamPics
  steam_pics_art(const std::string &body, int app_id, std::string_view region) {
    SteamPics pics;
    try {
      const json parsed = json::parse(body);
      const std::string key = std::to_string(app_id);
      const json data = parsed.value("data", json::object());
      if (!data.is_object() || !data.contains(key) || !data[key].is_object()) return {};
      const json common = data[key].value("common", json::object());
      const json full = common.value("library_assets_full", json::object());
      const bool chinese = region == "cn" || region == "bili";
      const std::vector<std::string> languages = chinese ? std::vector<std::string> { "schinese", "english" } : std::vector<std::string> { "english" };
      // "<40 hex>/<file>": the Steam CDN's content-addressed path; anything else in the answer is not used
      const auto safe = [](const std::string &file) {
        if (file.size() < 43 || file[40] != '/') return false;
        for (std::size_t i = 0; i < 40; ++i)
          if (!std::isxdigit(static_cast<unsigned char>(file[i]))) return false;
        for (std::size_t i = 41; i < file.size(); ++i)
          if (!std::isalnum(static_cast<unsigned char>(file[i])) && file[i] != '_' && file[i] != '.' && file[i] != '-') return false;
        return true;
      };
      const auto asset_url = [&](const std::string &file) { return safe(file) ? std::string(kSteamAssets) + "steam/apps/" + key + "/" + file : std::string(); };
      const auto pick = [&](const char *asset, const char *image, const std::vector<std::string> &order) {
        const json images = full.value(asset, json::object()).value(image, json::object());
        for (const auto &language : order)
          if (images.is_object() && images.contains(language) && images[language].is_string()) {
            const std::string url = asset_url(images[language].get<std::string>());
            if (!url.empty()) return url;
          }
        return std::string();
      };
      const std::string icon = common.value("clienticon", std::string());
      if (icon.size() == 40 && safe(icon + "/icon")) pics.icon = std::string(kSteamCommunityIcons) + key + "/" + icon + ".ico";
      pics.logo = pick("library_logo", "image", languages);
      pics.hero = pick("library_hero", "image", { "english" });
      pics.capsule = pick("library_capsule", "image2x", languages);
      if (pics.capsule.empty()) pics.capsule = pick("library_capsule", "image", languages);
      const auto hash_of = [](const std::string &url) {
        const auto at = url.find("/steam/apps/");
        if (at == std::string::npos) return std::string();
        const auto slash = url.find('/', at + 12);
        return slash == std::string::npos ? std::string() : url.substr(slash + 1, 12);
      };
      pics.logo_version = hash_of(pics.logo);
      pics.version = hash_of(pics.hero) + "-" + pics.logo_version;
    }
    catch (const std::exception &) {
      return {};
    }
    return pics;
  }

  std::vector<IconSource>
  icon_sources(std::string_view game, std::string_view, const GetUrlFn &get_url) {
    std::vector<IconSource> sources;
    for (const auto &row : source_table()) {
      if (row.game != game) continue;
      if (!steam_app_of(row)) continue;
      const int app = steam_app_of(row);
      sources.push_back({ "steam-clienticon", [app, get_url] {
                           if (!get_url) return std::string();
                           const auto body = get_url(steam_pics_url(app));
                           return body ? steam_pics_art(*body, app, "global").icon : std::string();
                         } });
      break;
    }
    // Arknights has no store page: the Yostar site's own icon is the game's (a PNG, not the Hypergryph company favicon the national sites carry)
    if (game == "arknights") sources.push_back({ "official-site", [] { return std::string(kYostarArknightsIcon); } });
    return sources;
  }

  std::string
  steam_items_url(int app_id) {
    const json input = { { "ids", json::array({ { { "appid", app_id } } }) }, { "context", { { "language", "english" }, { "country_code", "US" } } }, { "data_request", { { "include_assets", true } } } };
    return "https://api.steampowered.com/IStoreBrowseService/GetItems/v1?input_json=" + url_encoded(input.dump());
  }

  ArtUrls
  art_from_steam(const std::string &body) {
    ArtUrls urls;
    try {
      const json parsed = json::parse(body);
      const json items = parsed.value("response", json::object()).value("store_items", json::array());
      if (!items.is_array() || items.empty() || !items[0].is_object()) return {};
      const json &item = items[0];
      const json assets = item.value("assets", json::object());
      std::string format = assets.value("asset_url_format", std::string());
      const auto placeholder = format.find("${FILENAME}");
      if (placeholder == std::string::npos) return {};
      const auto resolved = [&](const std::string &file) {
        if (file.empty()) return std::string();
        std::string url = format;
        url.replace(url.find("${FILENAME}"), 11, file);
        return std::string(kSteamAssets) + url;
      };
      const std::string background = assets.value("raw_page_background", std::string());
      urls.background = resolved(background);
      urls.cover = resolved(assets.value("library_capsule_2x", assets.value("library_capsule", std::string())));
      const std::string icon = assets.value("community_icon", std::string());
      if (!icon.empty() && item.contains("appid") && item["appid"].is_number_integer())
        urls.icon = std::string(kSteamCommunityIcons) + std::to_string(item["appid"].get<long long>()) + "/" + icon + ".jpg";
      // a store page without the raw art keeps the 600x900 capsule as its only picture
      if (urls.background.empty()) return {};
      urls.version = steam_hash_of(background).substr(0, 16);
      const std::string capsule = assets.value("library_capsule_2x", assets.value("library_capsule", std::string()));
      if (!steam_hash_of(capsule).empty()) urls.version += "-" + steam_hash_of(capsule).substr(0, 12);
    }
    catch (const std::exception &) {
      return {};
    }
    return urls;
  }

  std::string
  ini_value(const std::string &text, const std::string &key) {
    std::size_t at = 0;
    while (at < text.size()) {
      const auto eol = text.find('\n', at);
      std::string line = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
      at = eol == std::string::npos ? text.size() : eol + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '=') return line.substr(key.size() + 1);
    }
    return {};
  }

  ArtUrls
  art_from_nte_file_list(const std::string &xml) {
    ArtUrls urls;
    // <Url BaseUrl="…"/> <ProductVersion Version="…"/> <File Checksum="…" Path="/ResFilesM/1289/bgimgs/bg_0.png" …/>
    std::string base;
    std::string version;
    std::string checksum;
    std::size_t at = 0;
    while ((at = xml.find('<', at)) != std::string::npos) {
      const auto end = xml.find('>', at);
      if (end == std::string::npos) break;
      const std::string tag = xml.substr(at, end - at);
      at = end;
      if (tag.rfind("<Url ", 0) == 0)
        base = attribute_of(tag, "BaseUrl");
      else if (tag.rfind("<ProductVersion ", 0) == 0)
        version = attribute_of(tag, "Version");
      else if (tag.rfind("<File ", 0) == 0 && attribute_of(tag, "Path") == kNtePicturePath)
        checksum = attribute_of(tag, "Checksum");
    }
    if (base.rfind("https://", 0) != 0 || version.empty() || checksum.empty()) return {};
    while (!base.empty() && base.back() == '/') base.pop_back();
    urls.background = base + "/" + version + kNtePicturePath + ".zip";
    urls.version = checksum;
    return urls;
  }

  bool
  art_background_zipped(const ArtUrls &urls) {
    const std::string &url = urls.background;
    return url.size() > 4 && url.compare(url.size() - 4, 4, ".zip") == 0;
  }

  std::string
  page_image_url(const std::string &page, std::string_view marker) {
    std::size_t at = 0;
    while ((at = page.find("<img", at)) != std::string::npos) {
      const auto end = page.find('>', at);
      if (end == std::string::npos) return {};
      const std::string url = attribute_of(page.substr(at, end - at), "src");
      if (url.rfind("https://", 0) == 0 && url.find(marker) != std::string::npos) return url;
      at = end;
    }
    return {};
  }

  std::vector<VendorSource>
  vendor_sources(std::string_view game, std::string_view region, const GetUrlFn &get_url, const PostUrlFn &post_url) {
    std::vector<VendorSource> sources;
    for (const auto &row : source_table()) {
      if (row.game != game || row.region != region) continue;
      switch (row.family) {
        case Family::hypergryph:
          // a clean variant first (the title is laid over key art that has none painted in), then the launcher's own picture (title painted in, no logo over it)
          if (steam_app_of(row)) sources.push_back({ "steam-clean", [row, get_url] { return resolve_steam_clean(row, get_url); } });
          sources.push_back({ "hypergryph", [row, get_url, post_url] { return resolve_hypergryph(row, get_url, post_url); } });
          // the launcher's picture is a webp for Endfield: the store build's art is the third choice when the machine cannot decode it
          if (row.steam_id) sources.push_back({ "steam", [row, get_url] { return resolve_steam(row, get_url); } });
          break;
        case Family::kuro:
          sources.push_back({ "kuro", [row, get_url] { return resolve_kuro(row, get_url); } });
          // the store build is the second choice (a webp the machine cannot decode, an answer that did not come)
          if (row.steam_id) sources.push_back({ "steam", [row, get_url] { return resolve_steam(row, get_url); } });
          break;
        case Family::steam:
          sources.push_back({ "steam", [row, get_url] { return resolve_steam(row, get_url); } });
          break;
        case Family::nte_launcher:
          sources.push_back({ "nte-launcher", [get_url] { return resolve_nte_launcher(get_url); } });
          break;
      }
    }
    return sources;
  }
}  // namespace sparxie

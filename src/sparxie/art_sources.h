#pragma once
#include "src/sparxie/art.h"
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sparxie {
  // One HTTP round trip of the art refresh: a GET, and a POST of one JSON
  // body (the Hypergryph launchers' own content API is POST-only).
  using GetUrlFn = std::function<std::optional<std::string>(const std::string &)>;
  using PostUrlFn = std::function<std::optional<std::string>(const std::string &, const std::string &)>;

  // One place the official art of a title and region can be taken from.
  // resolve() asks the source (a few requests) and answers the picture URLs;
  // an empty background means the source did not answer. The refresh tries
  // the sources of a title in order and takes the first whose picture
  // downloads and decodes; with none left it draws the placeholder.
  struct VendorSource {
    std::string label;  // the source family, for the refresh report
    std::function<ArtUrls()> resolve;
  };

  // The sources of one non-HoYoverse title and region, best first. Empty when
  // the title has no sourced official art there (the caller draws the
  // placeholder without a round trip). Every source is named in
  // docs/sparxie-games.md with its URL pattern.
  std::vector<VendorSource>
  vendor_sources(std::string_view game, std::string_view region, const GetUrlFn &get_url, const PostUrlFn &post_url);

  // True when a downloaded background is a zip holding the picture (the NTE
  // launcher ships its pictures as <name>.zip).
  bool
  art_background_zipped(const ArtUrls &urls);

  // The request body of the Hypergryph launchers' public content API for one
  // game code, and the art of its answer (get_main_bg_image; the background
  // URL carries the content hash, which becomes the cache version).
  std::string
  batch_proxy_body(const std::string &appcode);
  ArtUrls
  art_from_batch_proxy(const std::string &body);

  // Kuro's launcher: <host>/launcher/launcher/<id>_<key>/<app>/index.json
  // names the background configuration (functionCode.background), whose
  // answer holds the first frame of the launcher's background video and the
  // slogan (the wordmark).
  std::string
  kuro_background_id(const std::string &index_json);
  ArtUrls
  art_from_kuro_background(const std::string &background_json);

  // The store's public answer (IStoreBrowseService/GetItems with assets):
  // the page's raw 16:9 art, the 600x900 library capsule and the community
  // icon. The version is the content hash of the page art.
  ArtUrls
  art_from_steam(const std::string &body);
  std::string
  steam_items_url(int app_id);

  // NTE's national launcher: Version.ini names AllFiles.xml; the xml names the
  // base URL, the build and the zipped launcher picture with its checksum.
  std::string
  ini_value(const std::string &text, const std::string &key);
  ArtUrls
  art_from_nte_file_list(const std::string &xml);

  // Steam's own app record as the steamcmd.net mirror serves it (the PICS data the Steam client reads): the program icon (clienticon, an .ico with a 256 px
  // PNG entry), the library logo (a transparent PNG per language), the clean library hero (key art without the title) and the finished library capsule, all of
  // the Steam CDN. The region picks the language: cn and bilibili take schinese (then english), the rest english.
  struct SteamPics {
    std::string icon;
    std::string logo;
    std::string hero;
    std::string capsule;
    std::string logo_version;  // the content hash of the logo (12 characters)
    std::string version;  // the content hashes of the hero and the logo
  };
  std::string
  steam_pics_url(int app_id);
  SteamPics
  steam_pics_art(const std::string &body, int app_id, std::string_view region);

  // One place the program icon of a title can come from (after the install's own executable), best first. url() answers the address of an icon file, or nothing.
  struct IconSource {
    std::string label;
    std::function<std::string()> url;
  };
  std::vector<IconSource>
  icon_sources(std::string_view game, std::string_view region, const GetUrlFn &get_url);

  // The first <img> of a page whose src contains the marker (NTE's site names
  // its logo that way).
  std::string
  page_image_url(const std::string &page, std::string_view marker);
}  // namespace sparxie

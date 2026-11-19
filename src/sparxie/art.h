#pragma once
#include "src/sparxie/model.h"
#include <chrono>
#include <string_view>
namespace sparxie {
  struct Rgba {
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> px;
  };
  struct ArtSchedule {
    std::chrono::system_clock::time_point last_fetch {};
    std::chrono::system_clock::time_point last_check {};
    std::chrono::minutes jitter { 0 };
    bool host_started = false;
  };
  enum class ArtReason { host_start,
    interval,
    app_list,
    manual };
  struct ArtUrls {
    std::string icon;
    std::string logo;
    // the launcher's larger transparent logo (top_left_logo, about 420x320 with the mark filling it): the hero prefers it to `logo`, which is a small 416x234 canvas
    std::string logo_large;
    // the path data of an inline SVG logo the site draws its wordmark from (drawn white on transparent at logo_svg_w x logo_svg_h units), when the title has no logo file
    std::string logo_svg_path;
    double logo_svg_w = 0;
    double logo_svg_h = 0;
    // the key art already shows the title painted in where the landscape hero is cut (the portrait cover crops it out): the logo is laid on the portrait cover only
    bool logo_cover_only = false;
    std::string background;
    // a finished portrait cover of the title (the store's 600x900 capsule), used as the cover as it is instead of composing one from the background
    std::string cover;
    std::string version;
  };
  std::string
  art_version_of(std::string url);
  int
  cover_window_x(const Rgba &background);
  // The portrait cover (600x900): a window of the key art with the logo laid on by the same routine as the hero's (the plate cut-out, the contrast dimming behind
  // the logo, the soft shadow), large and centred in the lower third: cover_logo_box() says where.
  Rgba
  compose_cover(const Rgba &background, const Rgba &logo);
  // A finished portrait cover (the store's capsule) as the 600x900 cover the clients expect: untouched at that size, scaled otherwise; empty when there is no picture.
  Rgba
  fit_cover(const Rgba &finished);
  // Top strip of the background (about 16 percent down, about 6:1). Kept for the clients that still ask for the banner slot; the 大图 hero is compose_hero.
  Rgba
  crop_banner(const Rgba &background);
  // The part of the hero the clients keep clear of their bottom title scrim, as fractions of the height: the logo sits inside it.
  inline constexpr double kHeroSafeTop = 0.06;
  inline constexpr double kHeroSafeBottom = 0.68;
  // Where the visible part of the logo lands in a hero of the given size (w == 0 when the logo has nothing visible).
  struct HeroBox {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
  };
  HeroBox
  hero_logo_box(int hero_w, int hero_h, const Rgba &logo);
  // Where the visible mark of the logo (plate removed) lands on a cover of the given size (w == 0 when the logo has nothing visible).
  HeroBox
  cover_logo_box(int cover_w, int cover_h, const Rgba &logo);
  // The landscape poster (hero) of an install: the largest 16:9 window of the background (the whole frame of a 16:9 background; otherwise the window with the
  // most detail, where the characters are), scaled to 1920x1080 (or to the window's own size when that is smaller), with the official logo large and centred
  // above the client's title scrim. A backdrop too close in tone to the logo is dimmed (lifted for a dark logo) softly, only behind the logo.
  Rgba
  compose_hero(const Rgba &background, const Rgba &logo);
  // True when a picture is blank: (nearly) black, or (nearly) one colour. A background that a decoder returned as zeros, or a composition built from one, is blank; real art never is.
  // The alpha channel is not looked at: art is opaque.
  bool
  image_is_blank(const Rgba &image);
  // The same for JPEG bytes (a hero file); bytes that do not decode count as blank.
  bool
  jpeg_is_blank(std::string_view bytes);
  // A logo drawn on its own opaque black plate (the old zzz logo) is turned into the logo alone: the plate becomes transparent (matte removal: alpha from the
  // brightest channel, colour restored). Any other logo is returned as it is.
  Rgba
  logo_without_plate(const Rgba &logo);
  // True when the logo is drawn on an opaque plate that logo_without_plate would have to remove (so a sibling's transparent logo is the better one).
  bool
  logo_has_plate(const Rgba &logo);
  // How many pixels the visible mark of the logo (plate removed) covers; 0 for no logo. The bigger the source, the sharper the hero.
  int
  logo_visible_area(const Rgba &logo);
  // The hero as a JPEG (quality above 90: 4:4:4, no chroma subsampling, so the thin logo strokes keep their colour); empty when there is no picture.
  std::vector<std::uint8_t>
  encode_jpeg(const Rgba &image, int quality);
  inline constexpr int kHeroJpegQuality = 91;
  // A title's own placeholder card (600x900) and monogram icon (256x256: the brand colour with the title's initial; the last choice, after the install's
  // own program icon and the store's or publisher's): the brand colour of the game with its Latin name drawn on it, for a title whose
  // official art source did not answer. The card is sized so compose_cover()
  // returns it unchanged, and the generic box art is never used for it.
  Rgba
  placeholder_background(std::string_view game);
  Rgba
  placeholder_icon(std::string_view game);
  // The 16:9 landscape card of the same placeholder (1920x1080): the brand colour with the Latin name large and centred inside the safe band of the hero.
  Rgba
  placeholder_hero(std::string_view game);
  std::vector<std::uint8_t>
  encode_png(const Rgba &image);
  // The Sparxie mark badge on a poster of a touch application (top-right, on a smoked-glass rounded plate, so that it reads on a phone grid and stays clear of the
  // centred game logo and of the clients' bottom title scrim). with_badge returns the picture with the badge drawn in; an empty or invalid picture comes back as it is.
  struct BadgeBox {
    int x = 0;
    int y = 0;
    int size = 0;
  };
  // The plate of the badge on a w x h poster: its side is the smaller of 26 percent of the width and 25 percent of the height (a 16:9 hero 14 percent wide, a 2:3 cover 26 percent).
  BadgeBox
  badge_box(int w, int h);
  // Bumped when the badge is redrawn: files made for an older drawing are made again.
  inline constexpr int kBadgeRevision = 1;
  // The Sparxie mark that ships in the host (decoded from the embedded PNG).
  Rgba
  badge_mark();
  Rgba
  with_badge(const Rgba &image, const Rgba &mark);
  // A PNG or JPEG held in memory (a poster the host wrote, a cover of the person's own); an empty picture when it is neither or too large to handle.
  Rgba
  decode_image(const std::uint8_t *data, std::size_t size);
  bool
  art_due(ArtSchedule &schedule, std::chrono::system_clock::time_point now, ArtReason reason);
  ArtUrls
  art_urls_from_payload(const json &games_payload, const json &basic_payload, const std::string &biz, bool prefer_carousel);
}  // namespace sparxie

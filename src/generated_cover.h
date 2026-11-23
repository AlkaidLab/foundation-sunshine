/**
 * @file src/generated_cover.h
 * @brief Deterministic host-generated application covers.
 *
 * Windows owns the renderer. Other platforms keep their existing application
 * art behaviour until their host renderer is added.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace generated_cover {

  /// FNV-1a (32 bit) over the UTF-8 bytes of an application name: the hue of its cover is the hash modulo 360 and the shapes of the geometric style follow its bits.
  std::uint32_t
  name_hash(std::string_view name) noexcept;

  /// The letters on the chip and in the big letter of a cover: the first character of a Chinese, Japanese or Korean name, otherwise the capitals of the first letters of its first
  /// two words ("Steam Big Picture" is SB, "Google Chrome" GC, "X" is X); pictographs and punctuation do not count.
  std::string
  monogram(std::string_view name);

  inline constexpr std::string_view generator_version = "fs-generated-cover-5";

  enum class style_e : std::uint8_t {
    tonal,
    monogram,
    icon,
    editorial,
    glass,
    geometric,
  };

  enum class orientation_e : std::uint8_t {
    portrait,
    landscape,
  };

  enum class source_e : std::uint8_t {
    poster,
    generated,
    xbox,
  };

  struct app_t {
    std::string id;
    std::string name;
    std::string image_path;
    std::string poster_path;
    std::string hero_path;
    std::string command;
    std::string working_dir;
    std::vector<std::string> detached;
    std::string cover_style;
    /// The host's own placeholder picture (assets/box.png), the file the poster lookup falls back to; empty when the host has none to name.
    std::string placeholder_path;
    bool xbox_app = false;
    /// The poster is an address that has not been downloaded yet (the list does not download it): the application has a poster of its own, the cover is not drawn for it.
    bool poster_pending = false;
  };

  struct asset_t {
    std::filesystem::path path;
    std::string url;
    std::string content_type;
    std::string hash;
  };

  struct cover_t {
    source_e source = source_e::generated;
    style_e style = style_e::tonal;
    std::string style_name;
    std::string source_name;
    asset_t portrait;
    asset_t landscape;
  };

  bool
  is_style_name(std::string_view value) noexcept;

  style_e
  style_from_name(std::string_view value) noexcept;

  std::string_view
  style_name(style_e style) noexcept;

  std::string_view
  source_name(source_e source) noexcept;

  style_e
  effective_style(const app_t &app, std::string_view global_style) noexcept;

  /** Stable SHA-256 key for the cache and for regression tests. */
  std::string
  cache_hash(std::string_view style, orientation_e orientation, std::string_view app_name,
    std::string_view icon_path, std::int64_t icon_mtime);

  /**
   * Resolve the normal app cover. A poster is preserved when it is valid. The
   * default Xbox entry is identified by its command and keeps box.png.
   */
  cover_t
  describe(const app_t &app, std::string_view global_style, const std::filesystem::path &appdata, bool draw = true);

  /**
   * Whether the application shows the host's Xbox placeholder today: no `image-path`, a path to a file that is gone, the bare name
   * `box.png` (also `./assets/box.png`) or a path to the host's own assets/box.png, as the default Xbox entry and older versions wrote
   * them. A picture of the person's that merely is called box.png is not the placeholder. It is a read-time rule: apps.json is not changed.
   */
  bool
  shows_placeholder(const app_t &app);

  /**
   * The part of the art version that the generated cover contributes: empty while the application shows a poster (or the Xbox art), otherwise
   * the cache keys of both generated covers (generator, style, name and program icon). A client that keeps art by version sees a new version
   * the moment an application switches from the placeholder to a generated cover, and again when the style or the cover design changes.
   */
  std::string
  art_fact(const app_t &app, std::string_view global_style);

  /** Render a requested style, used by the authenticated picker preview. */
  asset_t
  preview(const app_t &app, style_e style, orientation_e orientation, const std::filesystem::path &appdata);

  std::string
  orientation_name(orientation_e orientation) noexcept;

}  // namespace generated_cover

/**
 * @file src/platform/windows/nvprefs/undo_data.cpp
 * @brief Definitions for undoing changes to nvidia preferences.
 */
// external includes
#include <nlohmann/json.hpp>

// local includes
#include "nvprefs_common.h"
#include "undo_data.h"

using json = nlohmann::json;

// Separate namespace for ADL, otherwise we need to define json
// functions in the same namespace as our types
namespace nlohmann {
  using data_t = nvprefs::undo_data_t::data_t;
  using opengl_swapchain_t = data_t::opengl_swapchain_t;
  using setting_undo_t = data_t::setting_undo_t;
  using game_profile_t = data_t::game_profile_t;
  using base_extras_t = data_t::base_extras_t;

  template <typename T>
  struct adl_serializer<std::optional<T>> {
    static void
    to_json(json &j, const std::optional<T> &opt) {
      if (opt == std::nullopt) {
        j = nullptr;
      }
      else {
        j = *opt;
      }
    }

    static void
    from_json(const json &j, std::optional<T> &opt) {
      if (j.is_null()) {
        opt = std::nullopt;
      }
      else {
        opt = j.template get<T>();
      }
    }
  };

  template <>
  struct adl_serializer<data_t> {
    static void
    to_json(json &j, const data_t &data) {
      j = json {
        { "opengl_swapchain", data.opengl_swapchain },
        { "game_profile", data.game_profile },
        { "base_extras", data.base_extras },
      };
    }

    static void
    from_json(const json &j, data_t &data) {
      j.at("opengl_swapchain").get_to(data.opengl_swapchain);
      // game_profile / base_extras are new fields, missing in legacy undo files.
      if (j.contains("game_profile")) {
        j.at("game_profile").get_to(data.game_profile);
      }
      if (j.contains("base_extras")) {
        j.at("base_extras").get_to(data.base_extras);
      }
    }
  };

  template <>
  struct adl_serializer<opengl_swapchain_t> {
    static void
    to_json(json &j, const opengl_swapchain_t &opengl_swapchain) {
      j = json {
        { "our_value", opengl_swapchain.our_value },
        { "undo_value", opengl_swapchain.undo_value }
      };
    }

    static void
    from_json(const json &j, opengl_swapchain_t &opengl_swapchain) {
      j.at("our_value").get_to(opengl_swapchain.our_value);
      j.at("undo_value").get_to(opengl_swapchain.undo_value);
    }
  };

  template <>
  struct adl_serializer<setting_undo_t> {
    static void
    to_json(json &j, const setting_undo_t &s) {
      j = json {
        { "our_value", s.our_value },
        { "undo_value", s.undo_value }
      };
    }

    static void
    from_json(const json &j, setting_undo_t &s) {
      j.at("our_value").get_to(s.our_value);
      j.at("undo_value").get_to(s.undo_value);
    }
  };

  template <>
  struct adl_serializer<game_profile_t> {
    static void
    to_json(json &j, const game_profile_t &g) {
      j = json {
        { "profile_name", g.profile_name },
        { "exe_path", g.exe_path },
        { "profile_was_created", g.profile_was_created },
        { "application_was_added", g.application_was_added },
        { "vsync", g.vsync },
        { "frl", g.frl },
        { "pstate", g.pstate },
        { "prerender", g.prerender },
      };
    }

    static void
    from_json(const json &j, game_profile_t &g) {
      j.at("profile_name").get_to(g.profile_name);
      j.at("exe_path").get_to(g.exe_path);
      j.at("profile_was_created").get_to(g.profile_was_created);
      j.at("application_was_added").get_to(g.application_was_added);
      if (j.contains("vsync")) j.at("vsync").get_to(g.vsync);
      if (j.contains("frl")) j.at("frl").get_to(g.frl);
      if (j.contains("pstate")) j.at("pstate").get_to(g.pstate);
      if (j.contains("prerender")) j.at("prerender").get_to(g.prerender);
    }
  };

  template <>
  struct adl_serializer<base_extras_t> {
    static void
    to_json(json &j, const base_extras_t &b) {
      j = json {
        { "vsync", b.vsync },
        { "frl", b.frl },
        { "pstate", b.pstate },
        { "prerender", b.prerender },
      };
    }

    static void
    from_json(const json &j, base_extras_t &b) {
      if (j.contains("vsync")) j.at("vsync").get_to(b.vsync);
      if (j.contains("frl")) j.at("frl").get_to(b.frl);
      if (j.contains("pstate")) j.at("pstate").get_to(b.pstate);
      if (j.contains("prerender")) j.at("prerender").get_to(b.prerender);
    }
  };
}  // namespace nlohmann

namespace nvprefs {

  void
  undo_data_t::set_opengl_swapchain(uint32_t our_value, std::optional<uint32_t> undo_value) {
    data.opengl_swapchain = data_t::opengl_swapchain_t {
      our_value,
      undo_value
    };
  }

  std::optional<undo_data_t::data_t::opengl_swapchain_t>
  undo_data_t::get_opengl_swapchain() const {
    return data.opengl_swapchain;
  }

  void
  undo_data_t::set_game_profile(const data_t::game_profile_t &game_profile) {
    data.game_profile = game_profile;
  }

  std::optional<undo_data_t::data_t::game_profile_t>
  undo_data_t::get_game_profile() const {
    return data.game_profile;
  }

  void
  undo_data_t::clear_game_profile() {
    data.game_profile = std::nullopt;
  }

  void
  undo_data_t::set_base_extras(const data_t::base_extras_t &base_extras) {
    data.base_extras = base_extras;
  }

  std::optional<undo_data_t::data_t::base_extras_t>
  undo_data_t::get_base_extras() const {
    return data.base_extras;
  }

  void
  undo_data_t::clear_base_extras() {
    data.base_extras = std::nullopt;
  }

  std::string
  undo_data_t::write() const {
    try {
      // Keep this assignment otherwise data will be treated as an array due to
      // initializer list shenanigangs.
      const json json_data = data;
      return json_data.dump();
    }
    catch (const std::exception &err) {
      error_message(std::string { "failed to serialize json data" });
      return {};
    }
  }

  void
  undo_data_t::read(const std::vector<char> &buffer) {
    try {
      data = json::parse(std::begin(buffer), std::end(buffer));
    }
    catch (const std::exception &err) {
      error_message(std::string { "failed to parse json data: " } + err.what());
      data = {};
    }
  }

  void
  undo_data_t::merge(const undo_data_t &newer_data) {
    const auto &swapchain_data = newer_data.get_opengl_swapchain();
    if (swapchain_data) {
      set_opengl_swapchain(swapchain_data->our_value, swapchain_data->undo_value);
    }
    const auto &game = newer_data.get_game_profile();
    if (game) {
      set_game_profile(*game);
    }
    const auto &base = newer_data.get_base_extras();
    if (base) {
      set_base_extras(*base);
    }
  }

}  // namespace nvprefs

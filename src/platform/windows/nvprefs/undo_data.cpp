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
  using setting_t = data_t::setting_t;

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
      j = json { { "opengl_swapchain", data.opengl_swapchain }, { "vsync", data.vsync } };
    }

    static void
    from_json(const json &j, data_t &data) {
      j.at("opengl_swapchain").get_to(data.opengl_swapchain);
      // Undo files from older Sunshine versions contain only the swapchain entry.
      data.vsync = j.value("vsync", std::optional<setting_t> {});
    }
  };

  template <>
  struct adl_serializer<setting_t> {
    static void
    to_json(json &j, const setting_t &opengl_swapchain) {
      j = json {
        { "our_value", opengl_swapchain.our_value },
        { "undo_value", opengl_swapchain.undo_value }
      };
    }

    static void
    from_json(const json &j, setting_t &opengl_swapchain) {
      j.at("our_value").get_to(opengl_swapchain.our_value);
      j.at("undo_value").get_to(opengl_swapchain.undo_value);
    }
  };
}  // namespace nlohmann

namespace nvprefs {

  void
  undo_data_t::set_opengl_swapchain(uint32_t our_value, std::optional<uint32_t> undo_value) {
    data.opengl_swapchain = data_t::setting_t {
      our_value,
      undo_value
    };
  }

  std::optional<undo_data_t::data_t::setting_t>
  undo_data_t::get_opengl_swapchain() const {
    return data.opengl_swapchain;
  }

  void
  undo_data_t::set_vsync(uint32_t our_value, std::optional<uint32_t> undo_value) {
    data.vsync = data_t::setting_t { our_value, undo_value };
  }

  std::optional<undo_data_t::data_t::setting_t>
  undo_data_t::get_vsync() const {
    return data.vsync;
  }

  void
  undo_data_t::clear_vsync() {
    data.vsync.reset();
  }

  bool
  undo_data_t::empty() const {
    return !data.opengl_swapchain && !data.vsync;
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

  bool
  undo_data_t::read(const std::vector<char> &buffer) {
    try {
      data = json::parse(std::begin(buffer), std::end(buffer));
      return true;
    }
    catch (const std::exception &err) {
      error_message(std::string { "failed to parse json data: " } + err.what());
      data = {};
      return false;
    }
  }

  void
  undo_data_t::merge(const undo_data_t &newer_data) {
    if (const auto vsync = newer_data.get_vsync()) {
      set_vsync(vsync->our_value, vsync->undo_value);
    }
    const auto &swapchain_data = newer_data.get_opengl_swapchain();
    if (swapchain_data) {
      set_opengl_swapchain(swapchain_data->our_value, swapchain_data->undo_value);
    }
  }

}  // namespace nvprefs

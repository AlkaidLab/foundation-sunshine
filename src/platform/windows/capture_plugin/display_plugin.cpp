/**
 * @file src/platform/windows/capture_plugin/display_plugin.cpp
 * @brief Implementation of the plugin-backed display adapter.
 */
#include "display_plugin.h"

#include <chrono>
#include <thread>

#include "src/logging.h"
#include "src/video.h"

using namespace std::literals;

namespace platf::capture_plugin {

  namespace {

    sunshine_mem_type_e
    to_plugin_mem_type(mem_type_e type) {
      switch (type) {
        case mem_type_e::system:
          return SUNSHINE_MEM_SYSTEM;
        case mem_type_e::dxgi:
          return SUNSHINE_MEM_DXGI;
        case mem_type_e::cuda:
          return SUNSHINE_MEM_CUDA;
        default:
          return SUNSHINE_MEM_SYSTEM;
      }
    }

  }  // namespace

  display_plugin_t::display_plugin_t(loaded_plugin_t *plugin, mem_type_e mem_type):
      plugin_(plugin), mem_type_(mem_type) {
  }

  display_plugin_t::~display_plugin_t() {
    stop_flag_ = true;

    if (session_) {
      if (plugin_->vtable.interrupt) {
        plugin_->vtable.interrupt(session_);
      }
      plugin_->vtable.destroy_session(session_);
      session_ = nullptr;
    }
  }

  int
  display_plugin_t::init(const ::video::config_t &config, const std::string &display_name) {
    sunshine_video_config_t plugin_config {};
    plugin_config.width = config.width;
    plugin_config.height = config.height;
    plugin_config.framerate = config.framerate;
    plugin_config.dynamic_range = config.dynamicRange;

    framerate_ = config.framerate;

    width = config.width;
    height = config.height;
    env_width = config.width;
    env_height = config.height;

    auto result = plugin_->vtable.create_session(
      to_plugin_mem_type(mem_type_),
      display_name.c_str(),
      &plugin_config,
      &session_);

    if (result != 0 || !session_) {
      BOOST_LOG(error) << "Plugin " << plugin_->name << " failed to create capture session";
      return -1;
    }

    BOOST_LOG(info) << "Plugin " << plugin_->name << " capture session created ("
                    << config.width << "x" << config.height << "@" << config.framerate << ")";
    return 0;
  }

  capture_e
  display_plugin_t::capture(
    const push_captured_image_cb_t &push_captured_image_cb,
    const pull_free_image_cb_t &pull_free_image_cb,
    bool *cursor) {
    if (!session_) return capture_e::error;

    stop_flag_ = false;
    auto frame_interval = std::chrono::nanoseconds(1'000'000'000 / framerate_);
    auto next_frame_time = std::chrono::steady_clock::now();

    while (!stop_flag_) {
      // Wait for frame timing
      auto now = std::chrono::steady_clock::now();
      if (now < next_frame_time) {
        std::this_thread::sleep_for(next_frame_time - now);
      }
      next_frame_time += frame_interval;

      // Get a free image from the pool
      std::shared_ptr<img_t> img;
      if (!pull_free_image_cb(img) || !img) {
        return capture_e::interrupted;
      }

      // Capture next frame from plugin
      auto plugin_img = std::static_pointer_cast<plugin_img_t>(img);
      plugin_img->release();  // Release any previous frame data

      sunshine_frame_t frame {};
      auto result = plugin_->vtable.next_frame(session_, &frame, 100);

      if (result == SUNSHINE_CAPTURE_OK) {
        // Copy frame data into img
        plugin_img->data = frame.data;
        plugin_img->width = frame.width;
        plugin_img->height = frame.height;
        plugin_img->pixel_pitch = frame.pixel_pitch;
        plugin_img->row_pitch = frame.row_pitch;
        plugin_img->plugin_frame = frame;
        plugin_img->session = session_;
        plugin_img->vtable = &plugin_->vtable;
        plugin_img->needs_release = true;

        if (frame.timestamp_ns > 0) {
          plugin_img->frame_timestamp = std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(frame.timestamp_ns));
        }
        else {
          plugin_img->frame_timestamp = std::chrono::steady_clock::now();
        }

        if (!push_captured_image_cb(std::move(img), true)) {
          return capture_e::ok;
        }
      }
      else if (result == SUNSHINE_CAPTURE_TIMEOUT) {
        // No new frame, push empty
        if (!push_captured_image_cb(std::move(img), false)) {
          return capture_e::ok;
        }
      }
      else if (result == SUNSHINE_CAPTURE_REINIT) {
        return capture_e::reinit;
      }
      else if (result == SUNSHINE_CAPTURE_INTERRUPTED) {
        return capture_e::interrupted;
      }
      else {
        BOOST_LOG(error) << "Plugin " << plugin_->name << " capture error";
        return capture_e::error;
      }
    }

    return capture_e::ok;
  }

  std::shared_ptr<img_t>
  display_plugin_t::alloc_img() {
    auto img = std::make_shared<plugin_img_t>();
    img->width = width;
    img->height = height;
    img->pixel_pitch = 4;  // Default BGRA
    img->row_pitch = img->width * img->pixel_pitch;
    return img;
  }

  int
  display_plugin_t::dummy_img(img_t *img) {
    // Fill with black
    if (img->data && img->row_pitch > 0 && img->height > 0) {
      std::memset(img->data, 0, img->row_pitch * img->height);
    }
    return 0;
  }

  bool
  display_plugin_t::is_hdr() {
    if (session_ && plugin_->vtable.is_hdr) {
      return plugin_->vtable.is_hdr(session_) != 0;
    }
    return false;
  }

  bool
  display_plugin_t::is_codec_supported(std::string_view name, const ::video::config_t &config) {
    // Plugin backends support all codecs by default
    return true;
  }

}  // namespace platf::capture_plugin

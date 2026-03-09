/**
 * @file src/platform/windows/capture_plugin/display_plugin.h
 * @brief Plugin-backed display adapter that bridges capture_plugin_api to display_t.
 */
#pragma once

#include <atomic>
#include <memory>

#include "capture_plugin_api.h"
#include "capture_plugin_loader.h"
#include "src/platform/common.h"

namespace platf::capture_plugin {

  /**
   * @brief Image type that wraps plugin frame data.
   */
  struct plugin_img_t: public img_t {
    /** Plugin session that owns this frame (needed for release). */
    sunshine_capture_session_t session = nullptr;

    /** Plugin vtable for calling release_frame. */
    const sunshine_capture_plugin_vtable_t *vtable = nullptr;

    /** The raw frame data from the plugin. */
    sunshine_frame_t plugin_frame {};

    /** Whether this frame needs to be released back to the plugin. */
    bool needs_release = false;

    ~plugin_img_t() override {
      release();
    }

    void
    release() {
      if (needs_release && vtable && vtable->release_frame && session) {
        vtable->release_frame(session, &plugin_frame);
        needs_release = false;
      }
    }
  };

  /**
   * @brief Display backend that delegates capture to an external plugin DLL.
   *
   * This adapter implements platf::display_t by forwarding calls to the
   * plugin's C ABI functions. It handles the translation between Sunshine's
   * internal types and the plugin's simplified types.
   */
  class display_plugin_t: public display_t {
  public:
    explicit display_plugin_t(loaded_plugin_t *plugin, mem_type_e mem_type);
    ~display_plugin_t() override;

    /**
     * @brief Initialize the plugin capture session.
     */
    int
    init(const ::video::config_t &config, const std::string &display_name);

    // display_t interface
    capture_e
    capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override;

    std::shared_ptr<img_t>
    alloc_img() override;

    int
    dummy_img(img_t *img) override;

    bool
    is_hdr() override;

    bool
    is_codec_supported(std::string_view name, const ::video::config_t &config) override;

  private:
    /** The loaded plugin providing capture functionality. */
    loaded_plugin_t *plugin_;

    /** Active capture session. */
    sunshine_capture_session_t session_ = nullptr;

    /** Memory type requested by Sunshine. */
    mem_type_e mem_type_;

    /** Stop flag for the capture loop. */
    std::atomic<bool> stop_flag_ { false };

    /** Frame rate for timing. */
    int framerate_ = 60;
  };

}  // namespace platf::capture_plugin

/**
 * @file src/platform/macos/display.mm
 * @brief Definitions for display capture on macOS.
 */
#include "src/platform/common.h"
#include "src/platform/macos/av_img_t.h"
#include "src/platform/macos/av_video.h"
#include "src/platform/macos/nv12_zero_device.h"

#include "src/config.h"
#include "src/logging.h"

// Avoid conflict between AVFoundation and libavutil both defining AVMediaType
#define AVMediaType AVMediaType_FFmpeg
#include "src/video.h"
#undef AVMediaType

#include <mutex>

namespace fs = std::filesystem;

// The input path toggles cursor capture from a different thread than display teardown.
// Keep a retained global reference behind a mutex so visibility changes can't race
// with AVVideo destruction.
static std::mutex g_active_av_capture_mutex;
static AVVideo *g_active_av_capture = nil;
static bool g_cursor_visible = true;

namespace platf {
  using namespace std::literals;

  struct av_display_t: public display_t {
    AVVideo *av_capture {};
    CGDirectDisplayID display_id {};

    ~av_display_t() override {
      std::lock_guard<std::mutex> lock(g_active_av_capture_mutex);
      if (g_active_av_capture == av_capture) {
        [g_active_av_capture release];
        g_active_av_capture = nil;
      }
      [av_capture release];
    }

    capture_e
    capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      auto signal = [av_capture capture:^(CMSampleBufferRef sampleBuffer) {
        auto new_sample_buffer = std::make_shared<av_sample_buf_t>(sampleBuffer);
        auto new_pixel_buffer = std::make_shared<av_pixel_buf_t>(new_sample_buffer->buf);

        std::shared_ptr<img_t> img_out;
        if (!pull_free_image_cb(img_out)) {
          // got interrupt signal
          // returning false here stops capture backend
          return false;
        }
        auto av_img = std::static_pointer_cast<av_img_t>(img_out);

        auto old_data_retainer = std::make_shared<temp_retain_av_img_t>(
          av_img->sample_buffer,
          av_img->pixel_buffer,
          img_out->data);

        av_img->sample_buffer = new_sample_buffer;
        av_img->pixel_buffer = new_pixel_buffer;
        img_out->data = new_pixel_buffer->data();

        img_out->width = (int) CVPixelBufferGetWidth(new_pixel_buffer->buf);
        img_out->height = (int) CVPixelBufferGetHeight(new_pixel_buffer->buf);
        img_out->row_pitch = (int) CVPixelBufferGetBytesPerRow(new_pixel_buffer->buf);
        img_out->pixel_pitch = img_out->row_pitch / img_out->width;

        old_data_retainer = nullptr;

        if (!push_captured_image_cb(std::move(img_out), true)) {
          // got interrupt signal
          // returning false here stops capture backend
          return false;
        }

        return true;
      }];

      // FIXME: We should time out if an image isn't returned for a while
      dispatch_semaphore_wait(signal, DISPATCH_TIME_FOREVER);

      return capture_e::ok;
    }

    std::shared_ptr<img_t>
    alloc_img() override {
      return std::make_shared<av_img_t>();
    }

    std::unique_ptr<avcodec_encode_device_t>
    make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
      if (pix_fmt == pix_fmt_e::yuv420p) {
        av_capture.pixelFormat = kCVPixelFormatType_32BGRA;

        return std::make_unique<avcodec_encode_device_t>();
      }
      else if (pix_fmt == pix_fmt_e::nv12 || pix_fmt == pix_fmt_e::p010) {
        auto device = std::make_unique<nv12_zero_device>();

        device->init(static_cast<void *>(av_capture), pix_fmt, setResolution, setPixelFormat);

        return device;
      }
      else {
        BOOST_LOG(error) << "Unsupported Pixel Format."sv;
        return nullptr;
      }
    }

    int
    dummy_img(img_t *img) override {
      auto signal = [av_capture capture:^(CMSampleBufferRef sampleBuffer) {
        auto new_sample_buffer = std::make_shared<av_sample_buf_t>(sampleBuffer);
        auto new_pixel_buffer = std::make_shared<av_pixel_buf_t>(new_sample_buffer->buf);

        auto av_img = (av_img_t *) img;

        auto old_data_retainer = std::make_shared<temp_retain_av_img_t>(
          av_img->sample_buffer,
          av_img->pixel_buffer,
          img->data);

        av_img->sample_buffer = new_sample_buffer;
        av_img->pixel_buffer = new_pixel_buffer;
        img->data = new_pixel_buffer->data();

        img->width = (int) CVPixelBufferGetWidth(new_pixel_buffer->buf);
        img->height = (int) CVPixelBufferGetHeight(new_pixel_buffer->buf);
        img->row_pitch = (int) CVPixelBufferGetBytesPerRow(new_pixel_buffer->buf);
        img->pixel_pitch = img->row_pitch / img->width;

        old_data_retainer = nullptr;

        // returning false here stops capture backend
        return false;
      }];

      if (dispatch_semaphore_wait(signal, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0) {
        BOOST_LOG(error) << "Timed out waiting for initial capture frame";
        return -1;
      }

      return 0;
    }

    /**
     * A bridge from the pure C++ code of the hwdevice_t class to the pure Objective C code.
     *
     * display --> an opaque pointer to an object of this class
     * width --> the intended capture width
     * height --> the intended capture height
     */
    static void
    setResolution(void *display, int width, int height) {
      [static_cast<AVVideo *>(display) setFrameWidth:width frameHeight:height];
    }

    static void
    setPixelFormat(void *display, OSType pixelFormat) {
      static_cast<AVVideo *>(display).pixelFormat = pixelFormat;
    }
  };

  std::shared_ptr<display_t>
  display(platf::mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type != platf::mem_type_e::system && hwdevice_type != platf::mem_type_e::videotoolbox) {
      BOOST_LOG(error) << "Could not initialize display with the given hw device type."sv;
      return nullptr;
    }

    auto display = std::make_shared<av_display_t>();

    // Default to main display
    display->display_id = CGMainDisplayID();

    // Print all displays available with it's name and id
    auto display_array = [AVVideo displayNames];
    BOOST_LOG(info) << "Detecting displays"sv;
    for (NSDictionary *item in display_array) {
      NSNumber *display_id = item[@"id"];
      // We need show display's product name and corresponding display number given by user
      NSString *name = item[@"displayName"];
      // We are using CGGetActiveDisplayList that only returns active displays so hardcoded connected value in log to true
      BOOST_LOG(info) << "Detected display: "sv << name.UTF8String << " (id: "sv << [NSString stringWithFormat:@"%@", display_id].UTF8String << ") connected: true"sv;
      if (!display_name.empty() && std::atoi(display_name.c_str()) == [display_id unsignedIntValue]) {
        display->display_id = [display_id unsignedIntValue];
      }
    }
    BOOST_LOG(info) << "Configuring selected display ("sv << display->display_id << ") to stream"sv;

    // 尝试最多 3 次初始化（处理显示器热插拔和状态不稳定）
    const int max_retries = 3;
    for (int retry = 0; retry < max_retries; retry++) {
      if (retry > 0) {
        BOOST_LOG(warning) << "Display initialization failed, retrying ("
                           << retry << "/" << max_retries << ")..."sv;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }

      display->av_capture = [[AVVideo alloc] initWithDisplay:display->display_id frameRate:config.framerate];

      if (display->av_capture &&
          display->av_capture.frameWidth > 0 &&
          display->av_capture.frameHeight > 0) {
        break;
      }

      if (display->av_capture) {
        [display->av_capture release];
        display->av_capture = nil;
      }
    }

    if (!display->av_capture) {
      BOOST_LOG(error) << "Video setup failed after "sv << max_retries << " retries."sv;
      return nullptr;
    }

    display->width = display->av_capture.frameWidth;
    display->height = display->av_capture.frameHeight;

    // 添加验证
    if (display->width <= 0 || display->height <= 0) {
      BOOST_LOG(error) << "Invalid display resolution detected: "sv
                       << display->width << "x"sv << display->height
                       << " for display ID: "sv << display->display_id;
      return nullptr;
    }

    // We also need set env_width and env_height for absolute mouse coordinates
    display->env_width = display->width;
    display->env_height = display->height;

    BOOST_LOG(info) << "Display initialized successfully: "sv
                    << display->width << "x"sv << display->height
                    << " @ "sv << config.framerate << "fps"sv;

    // Register active capture for cursor toggle from input layer
    {
      std::lock_guard<std::mutex> lock(g_active_av_capture_mutex);
      if (g_active_av_capture != display->av_capture) {
        [display->av_capture retain];
        if (g_active_av_capture) {
          [g_active_av_capture release];
        }
        g_active_av_capture = display->av_capture;
        [display->av_capture setCursorVisible:g_cursor_visible ? YES : NO];
      }
    }

    return display;
  }

  std::vector<std::string>
  display_names(mem_type_e hwdevice_type) {
    __block std::vector<std::string> display_names;

    auto display_array = [AVVideo displayNames];

    display_names.reserve([display_array count]);
    [display_array enumerateObjectsUsingBlock:^(NSDictionary *_Nonnull obj, NSUInteger idx, BOOL *_Nonnull stop) {
      NSString *name = obj[@"name"];
      display_names.emplace_back(name.UTF8String);
    }];

    return display_names;
  }

  /**
   * @brief Returns if GPUs/drivers have changed since the last call to this function.
   * @return `true` if a change has occurred or if it is unknown whether a change occurred.
   */
  bool
  needs_encoder_reenumeration() {
    // We don't track GPU state, so we will always reenumerate. Fortunately, it is fast on macOS.
    return true;
  }

  std::vector<std::string>
  adapter_names() {
    // macOS doesn't expose GPU adapters the same way Windows does
    return { "Apple GPU" };
  }

  void
  set_cursor_visible(bool visible) {
    AVVideo *capture = nil;
    {
      std::lock_guard<std::mutex> lock(g_active_av_capture_mutex);
      g_cursor_visible = visible;
      capture = g_active_av_capture;
      if (capture) {
        [capture retain];
      }
    }
    if (capture) {
      [capture setCursorVisible:visible ? YES : NO];
      [capture release];
    }
  }
}  // namespace platf

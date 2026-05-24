/**
 * @file src/platform/windows/mic_write.cpp
 * @brief Sunshine microphone wrapper over the AlkaidLab Windows WASAPI sink backend.
 */

#include "mic_write.h"

#include <memory>

#include "alkaidlab/microphone_uplink/windows_wasapi_sink_backend.h"
#include "src/logging.h"

namespace platf::audio {
  namespace backend_ns = alkaidlab::backend::microphone_windows_wasapi_sink;

  mic_write_wasapi_t::~mic_write_wasapi_t() {
    cleanup();
  }

  void
  mic_write_wasapi_t::cleanup() {
    is_cleaning_up.store(true);
    if (backend) {
      backend->cleanup();
      backend.reset();
    }
  }

  capture_e
  mic_write_wasapi_t::sample(std::vector<float> &sample_out) {
    (void)sample_out;
    BOOST_LOG(error) << "mic_write_wasapi_t::sample() should not be called";
    return capture_e::error;
  }

  int
  mic_write_wasapi_t::init() {
    is_cleaning_up.store(false);
    if (!backend) {
      backend = std::make_unique<backend_ns::backend_t>();
    }
    return backend ? backend->init() : -1;
  }

  int
  mic_write_wasapi_t::write_data(const char *data, size_t len, uint16_t seq) {
    if (is_cleaning_up.load()) {
      return -1;
    }
    return backend ? backend->write_data(data, len, seq) : -1;
  }

  int
  mic_write_wasapi_t::test_write() {
    if (is_cleaning_up.load()) {
      return -1;
    }
    return backend ? backend->test_write() : -1;
  }

  int
  mic_write_wasapi_t::restore_audio_devices() {
    return backend ? backend->restore_audio_devices() : -1;
  }

}  // namespace platf::audio

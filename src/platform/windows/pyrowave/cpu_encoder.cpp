/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/pyrowave/cpu_encoder.cpp
 * @brief Opt-in PyroWave encoder session for the Windows system capture path.
 */
#include "cpu_encoder.h"

#include "src/pyrowave/packetizer.h"
#include "src/perf_recorder.h"
#include "src/config.h"
#include "transport.h"

extern "C" {
#include "third-party/moonlight-common-c/src/PyrowaveProtocol.h"
}

#include <vulkan/vulkan.h>
#include "pyrowave.h"
#include "color_metadata.h"
#include "rate_control.h"
#include "src/logging.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace platf::pyrowave_windows {
  namespace {
    struct device_deleter_t {
      void operator()(pyrowave_device device) const noexcept {
        if (device) pyrowave_device_destroy(device);
      }
    };

    struct encoder_deleter_t {
      void operator()(pyrowave_encoder encoder) const noexcept {
        if (encoder) pyrowave_encoder_destroy(encoder);
      }
    };

    class encoder_session_t final: public video::encode_session_t {
    public:
      static constexpr std::size_t maximum_bitstream_size = 16u * 1024u * 1024u;

      encoder_session_t(
        int width,
        int height,
        std::size_t packet_boundary,
        bool full_range,
        int dynamic_range,
        int bitrate_kbps,
        int frame_rate_num,
        int frame_rate_den,
        std::uint32_t perf_session_id):
          width_ { width },
          height_ { height },
          packet_boundary_ { packet_boundary },
          full_range_ { full_range },
          dynamic_range_ { dynamic_range },
          bitrate_kbps_ { bitrate_kbps },
          frame_rate_num_ { frame_rate_num },
          frame_rate_den_ { frame_rate_den },
        perf_session_id_ { perf_session_id } {
        if (packet_boundary == 0 || packet_boundary > LI_PYROWAVE_MAX_PACKET_SIZE ||
            packet_boundary <= LI_PYROWAVE_WIRE_FEC_HEADER_SIZE) {
          init_failure(
            "invalid transport packet boundary",
            packet_boundary > static_cast<std::size_t>(std::numeric_limits<int>::max()) ?
              -1 : static_cast<int>(packet_boundary));
          return;
        }
        constexpr std::uint64_t max_pixels = 8192ull * 8192ull;
        if ((width_ & 1) != 0 || (height_ & 1) != 0 || width_ <= 0 || height_ <= 0 ||
            width_ > 8192 || height_ > 8192 ||
            static_cast<std::uint64_t>(width_) * static_cast<std::uint64_t>(height_) > max_pixels ||
            frame_rate_num <= 0 || frame_rate_den <= 0) {
          init_failure("invalid dimensions");
          return;
        }

        pyrowave_device raw_device = nullptr;
        const auto device_result = pyrowave_create_default_device(&raw_device);
        if (device_result != PYROWAVE_SUCCESS || raw_device == nullptr) {
          init_failure("default device creation", static_cast<int>(device_result));
          return;
        }
        device_.reset(raw_device);

        pyrowave_encoder raw_encoder = nullptr;
        const pyrowave_encoder_create_info encoder_info {
          .device = device_.get(),
          .width = width_,
          .height = height_,
          .chroma = PYROWAVE_CHROMA_SUBSAMPLING_420,
        };
        const auto encoder_result = pyrowave_encoder_create(&encoder_info, &raw_encoder);
        if (encoder_result != PYROWAVE_SUCCESS || raw_encoder == nullptr) {
          init_failure("CPU encoder creation", static_cast<int>(encoder_result));
          device_.reset();
          return;
        }
        encoder_.reset(raw_encoder);
        const auto metadata = color_metadata_for(dynamic_range_, full_range_);
        const auto metadata_result = pyrowave_encoder_set_color_metadata(encoder_.get(), &metadata);
        if (metadata_result != PYROWAVE_SUCCESS) {
          init_failure("color metadata", static_cast<int>(metadata_result));
          encoder_.reset();
          device_.reset();
          return;
        }

        y_.resize(static_cast<std::size_t>(width_) * height_);
        u_.resize(static_cast<std::size_t>(width_ / 2) * (height_ / 2));
        v_.resize(u_.size());
        bitstream_.resize(maximum_bitstream_size);
        const auto initial_frame_budget = pyrowave_frame_budget(
          bitrate_kbps_, frame_rate_num_, frame_rate_den_);
        BOOST_LOG(info) << "[PyroWaveEncoder][CPU] rate-control budget="
                        << initial_frame_budget << " bytes/frame"
                        << ", bits_per_pixel="
                        << pyrowave_frame_bits_per_pixel(initial_frame_budget, width_, height_)
                        << ", target_bitrate=" << bitrate_kbps_ << " Kbps"
                        << ", budget_fps=" << frame_rate_num_ << '/' << frame_rate_den_;
      }

      bool
      ready() const noexcept {
        return static_cast<bool>(device_) && static_cast<bool>(encoder_);
      }

      int
      convert(platf::img_t &image) override {
        if (!ready() || image.data == nullptr || image.width <= 0 || image.height <= 0 || image.pixel_pitch < 4) {
          return -1;
        }

        const auto stride = image.row_pitch > 0 ? image.row_pitch : image.width * image.pixel_pitch;
        const auto pixel_at = [&](int output_x, int output_y) {
          const auto source_x = std::min(image.width - 1, output_x * image.width / width_);
          const auto source_y = std::min(image.height - 1, output_y * image.height / height_);
          return image.data + static_cast<std::size_t>(source_y) * stride +
                 static_cast<std::size_t>(source_x) * image.pixel_pitch;
        };
        for (int y = 0; y < height_; ++y) {
          for (int x = 0; x < width_; ++x) {
            const auto *pixel = pixel_at(x, y);
            const float blue = pixel[0] / 255.0f;
            const float green = pixel[1] / 255.0f;
            const float red = pixel[2] / 255.0f;
            const auto y_value = std::clamp(0.2126f * red + 0.7152f * green + 0.0722f * blue, 0.0f, 1.0f);
            const float y_scale = full_range_ ? 255.0f : 219.0f;
            const float y_offset = full_range_ ? 0.0f : 16.0f;
            y_[static_cast<std::size_t>(y) * width_ + x] = static_cast<std::uint8_t>(std::lround(y_offset + y_value * y_scale));
          }
        }

        for (int y = 0; y < height_; y += 2) {
          for (int x = 0; x < width_; x += 2) {
            float red = 0.0f;
            float green = 0.0f;
            float blue = 0.0f;
            for (int oy = 0; oy < 2; ++oy) {
              for (int ox = 0; ox < 2; ++ox) {
                const auto *pixel = pixel_at(x + ox, y + oy);
                blue += pixel[0] / 255.0f;
                green += pixel[1] / 255.0f;
                red += pixel[2] / 255.0f;
              }
            }
            red *= 0.25f;
            green *= 0.25f;
            blue *= 0.25f;
            const auto chroma_index = static_cast<std::size_t>(y / 2) * (width_ / 2) + x / 2;
            const float chroma_scale = full_range_ ? 255.0f : 224.0f;
            u_[chroma_index] = static_cast<std::uint8_t>(std::lround(std::clamp((-0.1146f * red - 0.3854f * green + 0.5f * blue) * chroma_scale + 128.0f, 0.0f, 255.0f)));
            v_[chroma_index] = static_cast<std::uint8_t>(std::lround(std::clamp((0.5f * red - 0.4542f * green - 0.0458f * blue) * chroma_scale + 128.0f, 0.0f, 255.0f)));
          }
        }
        return 0;
      }

      void request_idr_frame() override {}
      void request_normal_frame() override {}
      void invalidate_ref_frames(int64_t, int64_t) override {}
      void
      set_bitrate(int bitrate_kbps) override {
        bitrate_kbps_ = std::max(0, video::encoder_bitrate_for_total_request(
          bitrate_kbps,
          config::video.max_bitrate,
          0));
        const auto frame_budget = pyrowave_frame_budget(
          bitrate_kbps_, frame_rate_num_, frame_rate_den_);
        BOOST_LOG(debug) << "[PyroWaveEncoder][CPU] rate-control update: bitrate="
                         << bitrate_kbps_ << " Kbps"
                         << ", budget=" << frame_budget << " bytes/frame"
                         << ", bits_per_pixel="
                         << pyrowave_frame_bits_per_pixel(frame_budget, width_, height_)
                         << ", budget_fps=" << frame_rate_num_ << '/' << frame_rate_den_;
      }
      void set_dynamic_param(const video::dynamic_param_t &param) override {
        if (param.type == video::dynamic_param_type_e::BITRATE && param.valid) {
          set_bitrate(param.value.int_value);
        }
      }

      pyrowave_encoder encoder() const noexcept { return encoder_.get(); }
      int width() const noexcept { return width_; }
      int height() const noexcept { return height_; }
      std::size_t packet_boundary() const noexcept { return packet_boundary_; }
      std::size_t frame_budget() const noexcept {
        return pyrowave_frame_budget(
          bitrate_kbps_, frame_rate_num_, frame_rate_den_);
      }
      const std::vector<std::uint8_t> &y() const noexcept { return y_; }
      const std::vector<std::uint8_t> &u() const noexcept { return u_; }
      const std::vector<std::uint8_t> &v() const noexcept { return v_; }
      std::vector<pyrowave_packet> &source_packets() noexcept { return source_packets_; }
      std::vector<std::uint8_t> &bitstream() noexcept { return bitstream_; }

      std::uint32_t
      rtp_timestamp(std::int64_t frame_number) const noexcept {
        if (frame_number <= 1) {
          return 0;
        }
        const auto elapsed_frames = static_cast<std::uint64_t>(frame_number - 1);
        const auto ticks = elapsed_frames * 90000u * static_cast<std::uint64_t>(frame_rate_den_);
        return static_cast<std::uint32_t>(ticks / static_cast<std::uint64_t>(frame_rate_num_));
      }

      int
      report_encode_failure(std::int64_t frame_number, std::string_view stage, int result = 0, std::size_t bytes = 0, std::size_t blocks = 0, bool packetization = false) const {
        perf::record_pyrowave_failure(perf_session_id_, packetization ? perf::pyrowave_failure_stage_e::packetize : perf::pyrowave_failure_stage_e::encode);
        BOOST_LOG(warning) << "[PyroWaveEncoder][CPU] encode failed: stage=" << stage
                           << ", frame=" << frame_number
                           << ", size=" << width_ << 'x' << height_
                           << ", api_result=" << result
                           << ", bytes=" << bytes
                           << ", blocks=" << blocks;
        return -1;
      }

    private:
      void
      init_failure(std::string_view stage, int result = 0) const {
        perf::record_pyrowave_failure(perf_session_id_, perf::pyrowave_failure_stage_e::recovery);
        BOOST_LOG(warning) << "[PyroWaveEncoder][CPU] backend initialization failed: stage=" << stage
                           << ", size=" << width_ << 'x' << height_
                           << ", api_result=" << result;
      }

      int width_;
      int height_;
      std::size_t packet_boundary_;
      bool full_range_;
      int dynamic_range_;
      int bitrate_kbps_;
      int frame_rate_num_;
      int frame_rate_den_;
      std::uint32_t perf_session_id_;
      std::unique_ptr<pyrowave_device_opaque, device_deleter_t> device_;
      std::unique_ptr<pyrowave_encoder_opaque, encoder_deleter_t> encoder_;
      std::vector<std::uint8_t> y_, u_, v_;
      std::vector<pyrowave_packet> source_packets_;
      std::vector<std::uint8_t> bitstream_;
    };
  }  // namespace

  std::unique_ptr<video::encode_session_t>
  make_encoder(
    int width,
    int height,
    std::size_t packet_boundary,
    bool full_range,
    int dynamic_range,
    int bitrate_kbps,
    int frame_rate_num,
    int frame_rate_den,
    std::uint32_t perf_session_id) {
    if (frame_rate_num <= 0 || frame_rate_den <= 0) {
      BOOST_LOG(warning) << "PyroWave CPU backend initialization failed: invalid frame rate "
                         << frame_rate_num << '/' << frame_rate_den;
      return nullptr;
    }
    try {
      auto session = std::make_unique<encoder_session_t>(
        width,
        height,
        packet_boundary,
        full_range,
        dynamic_range,
        video::cap_initial_encoder_bitrate(bitrate_kbps, config::video.max_bitrate, 0),
        frame_rate_num,
        frame_rate_den,
        perf_session_id);
      if (!session->ready()) {
        BOOST_LOG(warning) << "PyroWave CPU backend could not be initialized after detailed probe";
        return nullptr;
      }
      return session;
    }
    catch (const std::exception &error) {
      BOOST_LOG(warning) << "PyroWave CPU backend initialization raised an exception: " << error.what();
      return nullptr;
    }
    catch (...) {
      BOOST_LOG(warning) << "PyroWave CPU backend initialization raised an unknown exception";
      return nullptr;
    }
  }

  int
  encode_frame(
    int64_t frame_number,
    video::encode_session_t &base_session,
    safe::mail_raw_t::queue_t<video::packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::optional<platf::frame_pipeline_trace_t> pipeline_trace) {
    auto *session = dynamic_cast<encoder_session_t *>(&base_session);
    if (session == nullptr || !session->ready()) {
      BOOST_LOG(warning) << "PyroWave CPU encode failed: session is not ready for frame " << frame_number;
      return -1;
    }
    if (pipeline_trace) {
      pipeline_trace->encode_submit = std::chrono::steady_clock::now();
    }

    BOOST_LOG(verbose) << "[PyroWaveEncoder][CPU] encode frame=" << frame_number
                        << ", size=" << session->width() << 'x' << session->height()
                        << ", packet_boundary=" << session->packet_boundary()
                        << ", bitstream_capacity=" << session->bitstream().size();

    const pyrowave_cpu_buffer buffer {
      .data = {
        const_cast<std::uint8_t *>(session->y().data()),
        const_cast<std::uint8_t *>(session->u().data()),
        const_cast<std::uint8_t *>(session->v().data()),
      },
      .row_stride_in_bytes = {
        static_cast<std::size_t>(session->width()),
        static_cast<std::size_t>(session->width() / 2),
        static_cast<std::size_t>(session->width() / 2),
      },
      .plane_size_in_bytes = { session->y().size(), session->u().size(), session->v().size() },
      .width = session->width(),
      .height = session->height(),
      .format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P,
    };
    const pyrowave_rate_control rate_control {
      .maximum_bitstream_size = session->frame_budget()
    };
    const auto encode_result = pyrowave_encoder_encode_cpu_synchronous(session->encoder(), &buffer, &rate_control);
    if (encode_result != PYROWAVE_SUCCESS) {
      return session->report_encode_failure(frame_number, "pyrowave-cpu-encode", static_cast<int>(encode_result));
    }

    std::size_t packet_count = 0;
    const auto count_result = pyrowave_encoder_compute_num_packets(
      session->encoder(), session->packet_boundary(), &packet_count);
    if (count_result != PYROWAVE_SUCCESS || packet_count == 0 || packet_count > std::numeric_limits<std::uint16_t>::max()) {
      return session->report_encode_failure(frame_number, "compute-packets", static_cast<int>(count_result), 0, packet_count, true);
    }
    auto &source_packets = session->source_packets();
    source_packets.resize(packet_count);
    auto &source_bitstream = session->bitstream();
    std::size_t written_packets = 0;
    const auto packetize_result = pyrowave_encoder_packetize(
          session->encoder(), source_packets.data(), session->packet_boundary(), &written_packets,
          source_bitstream.data(), source_bitstream.size());
    if (packetize_result != PYROWAVE_SUCCESS || written_packets == 0 || written_packets > packet_count) {
      return session->report_encode_failure(frame_number, "pyrowave-packetize", static_cast<int>(packetize_result), 0, written_packets, true);
    }

    std::vector<std::uint8_t> encoded;
    std::size_t encoded_size = 0;
    for (std::size_t index = 0; index < written_packets; ++index) {
      const auto &source_packet = source_packets[index];
      if (source_packet.offset > source_bitstream.size() ||
          source_packet.size > source_bitstream.size() - source_packet.offset) {
        return session->report_encode_failure(frame_number, "packet-bounds", 0, source_packet.size, written_packets, true);
      }
      encoded_size += source_packet.size;
    }
    encoded.reserve(encoded_size);
    for (std::size_t index = 0; index < written_packets; ++index) {
      const auto &source_packet = source_packets[index];
      encoded.insert(encoded.end(), source_bitstream.begin() + source_packet.offset,
        source_bitstream.begin() + source_packet.offset + source_packet.size);
    }

    auto storage = std::make_shared<const std::vector<std::uint8_t>>(std::move(encoded));
    BOOST_LOG(verbose) << "[PyroWaveEncoder][CPU] encoded frame=" << frame_number
                        << ", source_packets=" << written_packets
                        << ", encoded_bytes=" << storage->size();
    const auto publish_result = publish_transport_frame(
      frame_number,
      std::move(storage),
      session->packet_boundary(),
      session->rtp_timestamp(frame_number),
      packets,
      channel_data,
      frame_timestamp,
      std::move(pipeline_trace));
    if (!publish_result.success) {
      return session->report_encode_failure(
        frame_number,
        "transport-packetize",
        publish_result.result_code,
        publish_result.bytes,
        publish_result.blocks,
        true);
    }
    return 0;
  }
}  // namespace platf::pyrowave_windows

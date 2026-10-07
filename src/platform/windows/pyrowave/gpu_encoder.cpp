/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/pyrowave/gpu_encoder.cpp
 * @brief PyroWave encoder using Sunshine's shared D3D11 YUV pipeline.
 */
#include "gpu_encoder.h"

#include "../display.h"
#include "../display_vram_internal.h"
#include "src/config.h"
#include "src/pyrowave/packetizer.h"
#include "src/perf_recorder.h"
#include "transport.h"

extern "C" {
#include "third-party/moonlight-common-c/src/PyrowaveProtocol.h"
}

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <limits>
#include <utility>
#include <vector>

#include "pyrowave.h"
#include "color_metadata.h"
#include "rate_control.h"
#include "src/logging.h"

#include <string_view>

namespace platf::pyrowave_windows {
  namespace {
    using Microsoft::WRL::ComPtr;

    struct device_deleter_t {
      void operator()(pyrowave_device value) const noexcept {
        if (value) pyrowave_device_destroy(value);
      }
    };

    struct encoder_deleter_t {
      void operator()(pyrowave_encoder value) const noexcept {
        if (value) pyrowave_encoder_destroy(value);
      }
    };

    struct sync_deleter_t {
      void operator()(pyrowave_sync_object value) const noexcept {
        if (value) pyrowave_sync_object_destroy(value);
      }
    };

    struct image_deleter_t {
      void operator()(pyrowave_image value) const noexcept {
        if (value) pyrowave_image_destroy(value);
      }
    };

    using device_ptr = std::unique_ptr<pyrowave_device_opaque, device_deleter_t>;
    using encoder_ptr = std::unique_ptr<pyrowave_encoder_opaque, encoder_deleter_t>;
    using sync_ptr = std::unique_ptr<pyrowave_sync_object_opaque, sync_deleter_t>;
    using image_ptr = std::unique_ptr<pyrowave_image_opaque, image_deleter_t>;

    [[nodiscard]] bool
    duplicate_handle(HANDLE source, HANDLE &duplicate) noexcept {
      duplicate = nullptr;
      return source != nullptr &&
        DuplicateHandle(
          GetCurrentProcess(), source, GetCurrentProcess(), &duplicate,
          0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE;
    }

    class gpu_encoder_session_t final: public video::encode_session_t {
    public:
      static constexpr std::size_t maximum_bitstream_size = 16u * 1024u * 1024u;

      gpu_encoder_session_t(
        std::shared_ptr<dxgi::display_vram_t> display,
        const video::config_t &config,
        std::size_t packet_boundary,
        int frame_rate_num,
        int frame_rate_den):
          packet_boundary_ { packet_boundary },
          width_ { config.width },
          height_ { config.height },
          perf_session_id_ { config.perf_session_id },
          frame_rate_num_ { frame_rate_num },
          frame_rate_den_ { frame_rate_den },
          bitrate_kbps_ {
            video::cap_initial_encoder_bitrate(config.bitrate, ::config::video.max_bitrate, 0)
          },
           conversion_ { dxgi::make_shared_yuv_encode_device(display, config) } {
        if (packet_boundary == 0 || packet_boundary > LI_PYROWAVE_MAX_PACKET_SIZE ||
            packet_boundary <= LI_PYROWAVE_WIRE_FEC_HEADER_SIZE) {
          init_failure(
            "invalid transport packet boundary",
            packet_boundary > static_cast<std::size_t>(std::numeric_limits<int>::max()) ?
              -1 : static_cast<int>(packet_boundary));
          return;
        }
        if (!display || config.width <= 0 || config.height <= 0 ||
            (config.width & 1) != 0 || (config.height & 1) != 0 ||
            config.width > 8192 || config.height > 8192 ||
            frame_rate_num <= 0 || frame_rate_den <= 0 ||
            !conversion_) {
          init_failure("invalid dimensions, frame rate, display or conversion device");
          return;
        }

        DXGI_ADAPTER_DESC1 adapter_desc {};
        if (!display->adapter || FAILED(display->adapter->GetDesc1(&adapter_desc))) {
          init_failure("DXGI adapter description");
          return;
        }

        pyrowave_luid luid {};
        static_assert(sizeof(luid.luid) == VK_LUID_SIZE);
        std::memcpy(luid.luid, &adapter_desc.AdapterLuid, VK_LUID_SIZE);

        pyrowave_device raw_device = nullptr;
        const auto device_result = pyrowave_create_device_by_compat(
          0, 0, nullptr, nullptr, &luid, &raw_device);
        if (device_result != PYROWAVE_SUCCESS || raw_device == nullptr) {
          init_failure("PyroWave device creation", static_cast<int>(device_result));
          if (raw_device) pyrowave_device_destroy(raw_device);
          return;
        }
        if (!pyrowave_device_confirm_interop_support(raw_device)) {
          init_failure("PyroWave D3D11/Vulkan interop confirmation");
          pyrowave_device_destroy(raw_device);
          return;
        }
        device_.reset(raw_device);

        const auto device5_result = conversion_->d3d_device()->QueryInterface(IID_PPV_ARGS(&device5_));
        const auto context4_result = conversion_->d3d_context()->QueryInterface(IID_PPV_ARGS(&context4_));
        if (FAILED(device5_result) || FAILED(context4_result)) {
          init_failure("D3D11 fence interfaces", FAILED(device5_result) ? static_cast<int>(device5_result) : static_cast<int>(context4_result));
          return;
        }

        const auto fence_result = device5_->CreateFence(
          0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_));
        if (FAILED(fence_result)) {
          init_failure("D3D11 shared fence", static_cast<int>(fence_result));
          return;
        }

        HANDLE fence_handle = nullptr;
        const auto shared_handle_result = fence_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle);
        if (FAILED(shared_handle_result) ||
            fence_handle == nullptr) {
          init_failure("D3D11 fence shared handle", static_cast<int>(shared_handle_result));
          return;
        }

        pyrowave_sync_object raw_sync = nullptr;
        const pyrowave_sync_object_create_info sync_info {
          .device = device_.get(),
          .external_handle = reinterpret_cast<pyrowave_os_handle>(fence_handle),
          .handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT,
          .semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE,
          .import_flags = 0,
        };
        const auto sync_result = pyrowave_sync_object_create(&sync_info, &raw_sync);
        if (sync_result != PYROWAVE_SUCCESS || raw_sync == nullptr) {
          init_failure("PyroWave external fence import", static_cast<int>(sync_result));
          CloseHandle(fence_handle);
          return;
        }
        sync_.reset(raw_sync);

        const auto &planes = conversion_->yuv_planes();
        for (std::size_t plane = 0; plane < planes.size(); ++plane) {
          HANDLE texture_handle = nullptr;
          if (!duplicate_handle(planes[plane].shared_handle, texture_handle)) {
            init_failure("D3D11 YUV plane handle duplication", static_cast<int>(GetLastError()));
            return;
          }
          const VkImageCreateInfo image_info {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = planes[plane].ten_bit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM,
            .extent = {
              planes[plane].width,
              planes[plane].height,
              1,
            },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
          };
          const pyrowave_image_create_info import_info {
            .device = device_.get(),
            .external_handle = reinterpret_cast<pyrowave_os_handle>(texture_handle),
            .handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
            .image_create_info = &image_info,
          };
          pyrowave_image raw_image = nullptr;
          const auto image_result = pyrowave_image_create(&import_info, &raw_image);
          if (image_result != PYROWAVE_SUCCESS || raw_image == nullptr) {
            init_failure("PyroWave YUV plane import", static_cast<int>(image_result));
            CloseHandle(texture_handle);
            return;
          }
          images_[plane].reset(raw_image);
          const auto image_view_result = pyrowave_image_get_image_view(
            images_[plane].get(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT,
            &buffers_.planes[plane]);
          if (image_view_result != PYROWAVE_SUCCESS) {
            init_failure("PyroWave YUV plane image view", static_cast<int>(image_view_result));
            return;
          }
        }

        const pyrowave_encoder_create_info encoder_info {
          .device = device_.get(),
          .width = config.width,
          .height = config.height,
          .chroma = PYROWAVE_CHROMA_SUBSAMPLING_420,
        };
        pyrowave_encoder raw_encoder = nullptr;
        const auto encoder_result = pyrowave_encoder_create(&encoder_info, &raw_encoder);
        if (encoder_result != PYROWAVE_SUCCESS || raw_encoder == nullptr) {
          init_failure("PyroWave GPU encoder creation", static_cast<int>(encoder_result));
          return;
        }
        encoder_.reset(raw_encoder);

        const auto metadata = color_metadata_for(conversion_->colorspace);
        if (!metadata) {
          init_failure("capture color space is outside the negotiated PyroWave contract");
          encoder_.reset();
          return;
        }
        const auto metadata_result = pyrowave_encoder_set_color_metadata(encoder_.get(), &*metadata);
        if (metadata_result != PYROWAVE_SUCCESS) {
          init_failure("PyroWave color metadata", static_cast<int>(metadata_result));
          encoder_.reset();
          return;
        }

        bitstream_.resize(maximum_bitstream_size);
        const auto initial_frame_budget = pyrowave_frame_budget(
          bitrate_kbps_, frame_rate_num_, frame_rate_den_);
        BOOST_LOG(info) << "[PyroWaveEncoder][GPU] rate-control budget="
                        << initial_frame_budget << " bytes/frame"
                        << ", bits_per_pixel="
                        << pyrowave_frame_bits_per_pixel(initial_frame_budget, width_, height_)
                        << ", target_bitrate=" << bitrate_kbps_ << " Kbps"
                        << ", budget_fps=" << frame_rate_num_ << '/' << frame_rate_den_;
        initialized_ = true;
      }

      ~gpu_encoder_session_t() override = default;

      [[nodiscard]] bool
      ready() const noexcept {
        return initialized_;
      }

      int
      convert(platf::img_t &image) override {
        has_frame_ = false;
        if (!ready()) {
          BOOST_LOG(warning) << "PyroWave GPU conversion failed: encoder is not ready";
          return -1;
        }
        const auto conversion_result = conversion_->convert(image);
        if (conversion_result != 0) {
          BOOST_LOG(warning) << "PyroWave GPU conversion failed: result=" << conversion_result;
          return -1;
        }
        has_frame_ = true;
        return 0;
      }

      void request_idr_frame() override {}
      void request_normal_frame() override {}
      void invalidate_ref_frames(int64_t, int64_t) override {}

      void
      set_bitrate(int bitrate_kbps) override {
        bitrate_kbps_ = std::max(0, video::encoder_bitrate_for_total_request(
          bitrate_kbps,
          ::config::video.max_bitrate,
          0));
        const auto frame_budget = pyrowave_frame_budget(
          bitrate_kbps_, frame_rate_num_, frame_rate_den_);
        BOOST_LOG(debug) << "[PyroWaveEncoder][GPU] rate-control update: bitrate="
                         << bitrate_kbps_ << " Kbps"
                         << ", budget=" << frame_budget << " bytes/frame"
                         << ", bits_per_pixel="
                         << pyrowave_frame_bits_per_pixel(frame_budget, width_, height_)
                         << ", budget_fps=" << frame_rate_num_ << '/' << frame_rate_den_;
      }

      void
      set_dynamic_param(const video::dynamic_param_t &param) override {
        if (param.valid && param.type == video::dynamic_param_type_e::BITRATE) {
          set_bitrate(param.value.int_value);
        }
      }

      int
      encode(
        int64_t frame_number,
        safe::mail_raw_t::queue_t<video::packet_t> &packets,
        void *channel_data,
        std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
        std::optional<platf::frame_pipeline_trace_t> pipeline_trace) {
        if (!ready() || !has_frame_) {
          return encode_failure(frame_number, "encoder-not-ready");
        }

        BOOST_LOG(verbose) << "[PyroWaveEncoder][GPU] encode frame=" << frame_number
                            << ", size=" << width_ << 'x' << height_
                            << ", packet_boundary=" << packet_boundary_
                            << ", bitstream_capacity=" << bitstream_.size();

        const auto acquire_value = sync_value_ + 1;
        const auto release_value = acquire_value + 1;
        const auto signal_result = context4_->Signal(fence_.Get(), acquire_value);
        if (FAILED(signal_result)) {
          return encode_failure(frame_number, "d3d11-fence-signal", static_cast<int>(signal_result));
        }
        context4_->Flush();

        std::array<pyrowave_gpu_external_reference, 3> references {};
        for (std::size_t plane = 0; plane < references.size(); ++plane) {
          references[plane] = {
            .image = images_[plane].get(),
            .queue_family_index = VK_QUEUE_FAMILY_EXTERNAL,
          };
        }
        const pyrowave_gpu_sync_operation acquire {
          .images = references.data(),
          .num_images = references.size(),
          .sync = {
            pyrowave_sync_object_get_semaphore(sync_.get()),
            acquire_value,
          },
        };
        const pyrowave_gpu_sync_operation release {
          .images = references.data(),
          .num_images = references.size(),
          .sync = {
            pyrowave_sync_object_get_semaphore(sync_.get()),
            release_value,
          },
        };

        if (pipeline_trace) {
          pipeline_trace->encode_submit = std::chrono::steady_clock::now();
        }

        const auto frame_budget = pyrowave_frame_budget(
          bitrate_kbps_, frame_rate_num_, frame_rate_den_);
        const pyrowave_rate_control rate_control {
          .maximum_bitstream_size = frame_budget,
        };
        const auto encode_result = pyrowave_encoder_encode_gpu_synchronous(
          encoder_.get(), &acquire, &release, &buffers_, &rate_control);
        if (encode_result != PYROWAVE_SUCCESS) {
          return encode_failure(frame_number, "pyrowave-gpu-encode", static_cast<int>(encode_result));
        }
        const auto wait_result = pyrowave_sync_object_cpu_wait(sync_.get(), release_value, 1000000000ull);
        if (wait_result != PYROWAVE_SUCCESS) {
          return encode_failure(frame_number, "pyrowave-fence-wait", static_cast<int>(wait_result));
        }
        sync_value_ = release_value;

        std::size_t packet_count = 0;
        const auto count_result = pyrowave_encoder_compute_num_packets(
          encoder_.get(), packet_boundary_, &packet_count);
        if (count_result != PYROWAVE_SUCCESS || packet_count == 0 || packet_count > UINT16_MAX) {
          return encode_failure(frame_number, "compute-packets", static_cast<int>(count_result), 0, packet_count, true);
        }

        source_packets_.resize(packet_count);
        std::size_t written_packets = 0;
        const auto packetize_result = pyrowave_encoder_packetize(
          encoder_.get(), source_packets_.data(), packet_boundary_, &written_packets,
          bitstream_.data(), bitstream_.size());
        if (packetize_result != PYROWAVE_SUCCESS || written_packets == 0 || written_packets > packet_count) {
          return encode_failure(frame_number, "pyrowave-packetize", static_cast<int>(packetize_result), 0, written_packets, true);
        }

        std::vector<std::uint8_t> encoded;
        for (std::size_t index = 0; index < written_packets; ++index) {
          const auto &packet = source_packets_[index];
          if (packet.offset > bitstream_.size() ||
              packet.size > bitstream_.size() - packet.offset) {
            return encode_failure(frame_number, "packet-bounds", 0, packet.size, written_packets, true);
          }
          encoded.insert(
            encoded.end(),
            bitstream_.begin() + packet.offset,
            bitstream_.begin() + packet.offset + packet.size);
        }

        auto storage = std::make_shared<const std::vector<std::uint8_t>>(std::move(encoded));
        BOOST_LOG(verbose) << "[PyroWaveEncoder][GPU] encoded frame=" << frame_number
                            << ", source_packets=" << written_packets
                            << ", encoded_bytes=" << storage->size();
        const auto publish_result = publish_transport_frame(
          frame_number,
          std::move(storage),
          packet_boundary_,
          rtp_timestamp(frame_number),
          packets,
          channel_data,
          frame_timestamp,
          std::move(pipeline_trace));
        if (!publish_result.success) {
          return encode_failure(
            frame_number,
            "transport-packetize",
            publish_result.result_code,
            publish_result.bytes,
            publish_result.blocks,
            true);
        }
        return 0;
      }

    private:
      int
      encode_failure(std::int64_t frame_number, std::string_view stage, int result = 0, std::size_t bytes = 0, std::size_t blocks = 0, bool packetization = false) const {
        perf::record_pyrowave_failure(perf_session_id_, packetization ? perf::pyrowave_failure_stage_e::packetize : perf::pyrowave_failure_stage_e::encode);
        BOOST_LOG(warning) << "[PyroWaveEncoder][GPU] encode failed: stage=" << stage
                           << ", frame=" << frame_number
                           << ", size=" << width_ << 'x' << height_
                           << ", api_result=" << result
                           << ", bytes=" << bytes
                           << ", blocks=" << blocks;
        return -1;
      }

      void
      init_failure(std::string_view stage, int result = 0) const {
        perf::record_pyrowave_failure(perf_session_id_, perf::pyrowave_failure_stage_e::recovery);
        BOOST_LOG(warning) << "[PyroWaveEncoder][GPU] backend initialization failed: stage=" << stage
                           << ", size=" << width_ << 'x' << height_
                           << ", frame_rate=" << frame_rate_num_ << '/' << frame_rate_den_
                           << ", api_result=" << result;
      }

      [[nodiscard]] std::uint32_t
      rtp_timestamp(std::int64_t frame_number) const noexcept {
        if (frame_number <= 1) {
          return 0;
        }
        const auto elapsed_frames = static_cast<std::uint64_t>(frame_number - 1);
        const auto ticks = elapsed_frames * 90000u * static_cast<std::uint64_t>(frame_rate_den_);
        return static_cast<std::uint32_t>(ticks / static_cast<std::uint64_t>(frame_rate_num_));
      }

      std::size_t packet_boundary_;
      int width_ = 0;
      int height_ = 0;
      std::uint32_t perf_session_id_ = 0;
      int frame_rate_num_ = 0;
      int frame_rate_den_ = 1;
      int bitrate_kbps_ = 0;
      bool initialized_ = false;
      bool has_frame_ = false;
      std::unique_ptr<dxgi::shared_yuv_encode_device_t> conversion_;
      Microsoft::WRL::ComPtr<ID3D11Device5> device5_;
      Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4_;
      Microsoft::WRL::ComPtr<ID3D11Fence> fence_;
      device_ptr device_;
      encoder_ptr encoder_;
      sync_ptr sync_;
      std::array<image_ptr, 3> images_;
      pyrowave_gpu_buffers buffers_ {};
      std::uint64_t sync_value_ = 0;
      std::vector<pyrowave_packet> source_packets_;
      std::vector<std::uint8_t> bitstream_;
    };
  }  // namespace

  std::unique_ptr<video::encode_session_t>
  make_gpu_encoder(
    std::shared_ptr<dxgi::display_vram_t> display,
    const video::config_t &config,
    std::size_t packet_boundary,
    int frame_rate_num,
    int frame_rate_den) {
    try {
      auto session = std::make_unique<gpu_encoder_session_t>(
        std::move(display), config, packet_boundary, frame_rate_num, frame_rate_den);
      if (!session->ready()) {
        BOOST_LOG(warning) << "PyroWave GPU backend could not be initialized after detailed probe";
        return {};
      }
      return session;
    }
    catch (const std::exception &exception) {
      BOOST_LOG(error) << "PyroWave GPU backend initialization raised an exception: " << exception.what();
      return {};
    }
    catch (...) {
      BOOST_LOG(error) << "PyroWave GPU backend initialization raised an unknown exception";
      return {};
    }
  }

  bool
  is_gpu_encoder_session(const video::encode_session_t &session) noexcept {
    return dynamic_cast<const gpu_encoder_session_t *>(&session) != nullptr;
  }

  int
  encode_gpu_frame(
    int64_t frame_number,
    video::encode_session_t &base_session,
    safe::mail_raw_t::queue_t<video::packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::optional<platf::frame_pipeline_trace_t> pipeline_trace) {
    auto *session = dynamic_cast<gpu_encoder_session_t *>(&base_session);
    if (session == nullptr) {
      return -1;
    }
    return session->encode(
      frame_number, packets, channel_data, frame_timestamp, std::move(pipeline_trace));
  }
}  // namespace platf::pyrowave_windows

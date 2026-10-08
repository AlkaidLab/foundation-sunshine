#include "transport.h"

#include "src/pyrowave/packetizer.h"
#include "src/pyrowave/dynamic_hdr.h"
#include "src/logging.h"

extern "C" {
#include "third-party/moonlight-common-c/src/PyrowaveProtocol.h"
}

#include <chrono>
#include <algorithm>
#include <exception>
#include <limits>
#include <utility>
#include <vector>

namespace platf::pyrowave_windows {
  namespace {
    constexpr std::uint16_t runtime_metadata_flags = LI_PYROWAVE_METADATA_FLAG_PROTECTED |
      LI_PYROWAVE_METADATA_FLAG_RUNTIME | LI_PYROWAVE_METADATA_FLAG_OPTIONAL;

    void
    append_u16(std::vector<std::uint8_t> &out, std::uint16_t value) {
      out.push_back(static_cast<std::uint8_t>(value >> 8));
      out.push_back(static_cast<std::uint8_t>(value));
    }

    void
    append_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
      out.push_back(static_cast<std::uint8_t>(value >> 24));
      out.push_back(static_cast<std::uint8_t>(value >> 16));
      out.push_back(static_cast<std::uint8_t>(value >> 8));
      out.push_back(static_cast<std::uint8_t>(value));
    }

    std::vector<std::uint8_t>
    build_runtime_metadata(std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
      std::vector<std::uint8_t> metadata;
      if (!frame_timestamp) {
        return metadata;
      }

      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - *frame_timestamp).count();
      const auto latency = static_cast<std::uint16_t>(std::clamp<std::int64_t>(
        (elapsed + 50) / 100, 0, std::numeric_limits<std::uint16_t>::max()));
      append_u16(metadata, LI_PYROWAVE_METADATA_HOST_PROCESSING_LATENCY);
      append_u16(metadata, runtime_metadata_flags);
      append_u32(metadata, sizeof(latency));
      append_u16(metadata, latency);
      return metadata;
    }
  }

  transport_publish_result_t
  publish_transport_frame(
    std::int64_t frame_number,
    std::shared_ptr<const std::vector<std::uint8_t>> bitstream,
    std::size_t packet_boundary,
    std::uint32_t rtp_timestamp,
    safe::mail_raw_t::queue_t<video::packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::optional<platf::frame_pipeline_trace_t> pipeline_trace,
    const pyrowave::hdr_frame_metadata_t *dynamic_metadata) noexcept {
    transport_publish_result_t result;
    const auto input_bytes = bitstream ? bitstream->size() : 0u;
    const auto boundary_overflow = packet_boundary >
      std::numeric_limits<std::size_t>::max() - LI_PYROWAVE_WIRE_FEC_HEADER_SIZE;
    const auto boundary_invalid = packet_boundary == 0 ||
      packet_boundary <= LI_PYROWAVE_WIRE_FEC_HEADER_SIZE ||
      packet_boundary > LI_PYROWAVE_MAX_PACKET_SIZE - LI_PYROWAVE_WIRE_FEC_HEADER_SIZE;
    BOOST_LOG(verbose) << "[PyroWaveTransport] publish frame=" << frame_number
                        << ", input_bytes=" << input_bytes
                        << ", inner_payload_boundary=" << packet_boundary
                        << ", wire_packet_boundary="
                        << (boundary_overflow ? 0u : packet_boundary + LI_PYROWAVE_WIRE_FEC_HEADER_SIZE)
                        << ", rtp_timestamp=" << rtp_timestamp
                        << ", block_aware_fec=1";
    if (frame_number <= 0 || !bitstream || bitstream->empty() ||
        boundary_overflow || boundary_invalid) {
      BOOST_LOG(warning) << "[PyroWaveTransport] rejected frame before packetizer"
                         << ", frame=" << frame_number
                         << ", input_bytes=" << input_bytes
                         << ", inner_payload_boundary=" << packet_boundary
                         << ", boundary_overflow=" << boundary_overflow
                         << ", boundary_invalid=" << boundary_invalid;
      result.result_code = -1;
      return result;
    }

    try {
      pyrowave::encoded_frame_t frame {
        .frame_id = static_cast<pyrowave::frame_id_t>(frame_number),
        .deadline = {
          std::chrono::steady_clock::now(),
          std::chrono::milliseconds(100),
        },
        .bitstream = { std::move(bitstream) },
      };
      auto metadata = build_runtime_metadata(frame_timestamp);
      auto metadata_flags = static_cast<std::uint16_t>(metadata.empty() ? 0 : runtime_metadata_flags);
      if (dynamic_metadata) {
        const std::size_t hdr_header_bytes = 8u + (dynamic_metadata->hlg_nominal_peak_nits != 0 ? 10u : 0u);
        if (dynamic_metadata->type == 0 || dynamic_metadata->payload.empty() ||
            metadata.size() + hdr_header_bytes > LI_PYROWAVE_MAX_METADATA_SIZE ||
            dynamic_metadata->payload.size() > LI_PYROWAVE_MAX_METADATA_SIZE - metadata.size() - hdr_header_bytes) {
          result.result_code = -1;
          return result;
        }
        append_u16(metadata, dynamic_metadata->type);
        append_u16(metadata, LI_PYROWAVE_METADATA_FLAG_PROTECTED | LI_PYROWAVE_METADATA_FLAG_REQUIRED);
        append_u32(metadata, static_cast<std::uint32_t>(dynamic_metadata->payload.size()));
        metadata.insert(metadata.end(), dynamic_metadata->payload.begin(), dynamic_metadata->payload.end());
        if (dynamic_metadata->hlg_nominal_peak_nits != 0) {
          append_u16(metadata, LI_PYROWAVE_METADATA_HLG_NOMINAL_PEAK);
          append_u16(metadata, LI_PYROWAVE_METADATA_FLAG_PROTECTED | LI_PYROWAVE_METADATA_FLAG_REQUIRED);
          append_u32(metadata, 2);
          append_u16(metadata, dynamic_metadata->hlg_nominal_peak_nits);
        }
        // Optional runtime TLVs and required HDR TLVs coexist. Requiredness
        // belongs to each TLV, not to every item in the metadata area.
        metadata_flags = LI_PYROWAVE_METADATA_FLAG_PROTECTED |
          (frame_timestamp ? LI_PYROWAVE_METADATA_FLAG_RUNTIME : 0);
      }
      auto packetized = pyrowave::make_transport_packetizer()->packetize(frame, {
        .packet_boundary = packet_boundary + LI_PYROWAVE_WIRE_FEC_HEADER_SIZE,
        .rtp_timestamp = rtp_timestamp,
        .block_aware_fec = true,
        .metadata = std::move(metadata),
        .metadata_flags = metadata_flags,
      });
      result.bytes = packetized.bitstream.storage ? packetized.bitstream.storage->size() : 0;
      result.blocks = packetized.packets.size();
      const auto packetized_valid = pyrowave::validate(packetized);
      BOOST_LOG(verbose) << "[PyroWaveTransport] packetizer result frame=" << frame_number
                          << ", failure=" << static_cast<int>(packetized.failure)
                          << ", output_bytes=" << result.bytes
                          << ", blocks=" << result.blocks
                          << ", valid=" << packetized_valid;
      // validate() also accepts a well-formed failure with no payload. That
      // is not a successful frame and must never reach the broadcast queue.
      if (packetized.failure != pyrowave::failure_e::none || !packetized_valid) {
        BOOST_LOG(warning) << "[PyroWaveTransport] packetization failed"
                           << ", frame=" << frame_number
                           << ", failure=" << static_cast<int>(packetized.failure)
                           << ", input_bytes=" << input_bytes
                           << ", output_bytes=" << result.bytes
                           << ", blocks=" << result.blocks;
        result.result_code = static_cast<int>(packetized.failure == pyrowave::failure_e::none
          ? pyrowave::failure_e::packetization_failed : packetized.failure);
        return result;
      }

      auto output = std::make_unique<video::packet_raw_generic>(
        std::vector<std::uint8_t>(packetized.bitstream.view().begin(), packetized.bitstream.view().end()),
        frame_number,
        true);
      output->channel_data = channel_data;
      output->frame_timestamp = frame_timestamp;
      if (pipeline_trace) {
        pipeline_trace->packet_ready = std::chrono::steady_clock::now();
      }
      output->pipeline_trace = std::move(pipeline_trace);
      packets->raise(std::move(output));
      result.success = true;
      return result;
    }
    catch (const std::exception &) {
      result.result_code = -1;
      return result;
    }
    catch (...) {
      result.result_code = -1;
      return result;
    }
  }

}  // namespace platf::pyrowave_windows

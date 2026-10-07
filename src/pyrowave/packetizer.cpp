/**
 * @file src/pyrowave/packetizer.cpp
 * @brief Bounds-checked server-side PyroWave frame-envelope packetizer.
 */
#include "packetizer.h"

#include "src/logging.h"

extern "C" {
#include "third-party/moonlight-common-c/src/PyrowaveProtocol.h"
}

#include <algorithm>
#include <limits>
#include <new>

namespace pyrowave {
  namespace {
    constexpr std::size_t default_packet_boundary = LI_PYROWAVE_MAX_PACKET_SIZE;

    bool
    validate_metadata(const std::vector<std::uint8_t> &metadata) noexcept {
      std::size_t offset = 0;
      while (offset < metadata.size()) {
        if (metadata.size() - offset < 8) {
          return false;
        }
        const auto flags = static_cast<std::uint16_t>((metadata[offset + 2] << 8) |
                                                       metadata[offset + 3]);
        const auto length = (static_cast<std::uint32_t>(metadata[offset + 4]) << 24) |
                            (static_cast<std::uint32_t>(metadata[offset + 5]) << 16) |
                            (static_cast<std::uint32_t>(metadata[offset + 6]) << 8) |
                            metadata[offset + 7];
        if ((flags & ~(LI_PYROWAVE_METADATA_FLAG_PROTECTED |
                       LI_PYROWAVE_METADATA_FLAG_RUNTIME |
                       LI_PYROWAVE_METADATA_FLAG_OPTIONAL |
                       LI_PYROWAVE_METADATA_FLAG_REQUIRED)) != 0 ||
            ((flags & LI_PYROWAVE_METADATA_FLAG_OPTIONAL) != 0 &&
             (flags & LI_PYROWAVE_METADATA_FLAG_REQUIRED) != 0) ||
            length > metadata.size() - offset - 8) {
          return false;
        }
        offset += 8 + length;
      }
      return offset == metadata.size();
    }

    packetization_result_t
    failure(failure_e reason) noexcept {
      return { .failure = reason };
    }
  }

  class transport_packetizer_t final: public packetizer_t {
  public:
    [[nodiscard]] packetization_result_t
    packetize(const encoded_frame_t &frame, const packetization_request_t &request) noexcept override {
      const auto frame_valid = validate(frame);
      const auto request_valid = validate(request);
      const auto input = frame.bitstream.view();
      const auto metadata_length = request.metadata.size();
      const auto metadata_valid = validate_metadata(request.metadata);
      if (!frame_valid || !request_valid || !metadata_valid ||
          metadata_length > LI_PYROWAVE_MAX_METADATA_SIZE ||
          metadata_length > std::numeric_limits<std::uint16_t>::max()) {
        BOOST_LOG(warning) << "[PyroWavePacketizer] rejected invalid frame/request"
                           << ", frame_id=" << frame.frame_id
                           << ", frame_kind=" << static_cast<int>(frame.kind)
                           << ", bitstream_bytes=" << input.size()
                           << ", metadata_bytes=" << metadata_length
                           << ", deadline_ms=" << frame.deadline.budget.count()
                           << ", packet_boundary=" << request.packet_boundary
                           << ", rtp_timestamp=" << request.rtp_timestamp
                           << ", block_aware_fec=" << request.block_aware_fec
                           << ", frame_valid=" << frame_valid
                           << ", request_valid=" << request_valid
                           << ", metadata_valid=" << metadata_valid;
        return failure(failure_e::configuration_invalid);
      }

      try {
        const auto boundary = request.packet_boundary == 0 ? default_packet_boundary : request.packet_boundary;
        const auto header_size = static_cast<std::size_t>(LI_PYROWAVE_WIRE_HEADER_SIZE);
        if (boundary <= header_size || boundary > LI_PYROWAVE_MAX_PACKET_SIZE) {
          BOOST_LOG(warning) << "[PyroWavePacketizer] rejected frame " << frame.frame_id
                             << ": invalid packet boundary " << boundary;
          return failure(failure_e::configuration_invalid);
        }

        std::vector<std::uint8_t> protected_payload;
        protected_payload.reserve(metadata_length + input.size());
        protected_payload.insert(protected_payload.end(), request.metadata.begin(), request.metadata.end());
        protected_payload.insert(protected_payload.end(), input.begin(), input.end());
        if (protected_payload.size() > std::numeric_limits<std::uint32_t>::max() ||
            input.size() > std::numeric_limits<std::uint32_t>::max()) {
          return failure(failure_e::packetization_failed);
        }

        const auto payload_boundary = boundary - header_size;
        const auto data_block_count = (protected_payload.size() + payload_boundary - 1) / payload_boundary;
        const auto fec_group_count = request.block_aware_fec
          ? (data_block_count + LI_PYROWAVE_FEC_DATA_PER_GROUP - 1) /
              LI_PYROWAVE_FEC_DATA_PER_GROUP
          : 0;
        const auto block_count = data_block_count + fec_group_count;
        if (data_block_count == 0 || data_block_count > LI_PYROWAVE_MAX_FRAME_BLOCKS ||
            block_count > LI_PYROWAVE_MAX_FRAME_BLOCKS ||
            block_count > std::numeric_limits<std::uint16_t>::max() ||
            payload_boundary > std::numeric_limits<std::uint16_t>::max()) {
          BOOST_LOG(warning) << "[PyroWavePacketizer] failed for frame " << frame.frame_id
                             << ": protected_bytes=" << protected_payload.size()
                             << ", payload_boundary=" << payload_boundary
                             << ", data_blocks=" << data_block_count
                             << ", wire_blocks=" << block_count;
          return failure(failure_e::packetization_failed);
        }

        const auto fec_scheme = request.block_aware_fec
          ? LI_PYROWAVE_FEC_SCHEME_XOR : LI_PYROWAVE_FEC_SCHEME_NONE;
        auto storage = std::make_shared<std::vector<std::uint8_t>>();
        storage->reserve((block_count + 1) * boundary);
        std::vector<packet_span_t> spans;
        spans.reserve(block_count + 1);

        const auto build_packet = [&](const LI_PYROWAVE_PACKET_HEADER &header,
                                      const std::uint8_t *payload,
                                      std::size_t payload_size) {
          const auto packet_offset = storage->size();
          storage->resize(packet_offset + header.headerLength + payload_size);
          std::size_t packet_size = 0;
          const auto result = LiPyrowaveBuildPacket(
            &header, payload, payload_size, storage->data() + packet_offset,
            storage->size() - packet_offset, &packet_size);
          if (result != LI_PYROWAVE_PACKET_OK || packet_size == 0) {
            BOOST_LOG(warning) << "[PyroWavePacketizer] failed for frame " << frame.frame_id
                               << ": packet=" << header.blockIndex << '/' << header.blockCount
                               << ", kind=" << static_cast<int>(header.packetKind)
                               << ", payload_bytes=" << payload_size
                               << ", api_result=" << static_cast<int>(result);
            return false;
          }
          spans.push_back({ packet_offset, packet_size });
          return true;
        };

        std::vector<std::uint8_t> frame_header_payload(payload_boundary, 0);
        std::copy(request.metadata.begin(), request.metadata.end(), frame_header_payload.begin());
        const auto common_flags = static_cast<std::uint8_t>(
          (metadata_length != 0 ? LI_PYROWAVE_FLAG_METADATA_PRESENT : 0) |
          (request.mark_critical ? LI_PYROWAVE_FLAG_CRITICAL : 0));
        LI_PYROWAVE_PACKET_HEADER frame_header {
          .version = LI_PYROWAVE_PROTOCOL_VERSION,
          .packetKind = LI_PYROWAVE_PACKET_FRAME_HEADER,
          .flags = static_cast<std::uint8_t>(LI_PYROWAVE_FLAG_START_OF_FRAME | common_flags),
          .reserved = 0,
          .headerLength = static_cast<std::uint16_t>(header_size),
          .metadataFlags = request.metadata_flags,
          .frameId = static_cast<std::uint32_t>(frame.frame_id),
          .rtpTimestamp = request.rtp_timestamp,
          .codecPayloadLength = static_cast<std::uint32_t>(input.size()),
          .protectedPayloadLength = static_cast<std::uint32_t>(protected_payload.size()),
          .metadataLength = static_cast<std::uint16_t>(metadata_length),
          .fecScheme = static_cast<std::uint8_t>(fec_scheme),
          .reserved2 = 0,
          .dataBlockCount = static_cast<std::uint16_t>(data_block_count),
          .parityBlockCount = static_cast<std::uint16_t>(fec_group_count),
          .fecBlockPayloadSize = static_cast<std::uint16_t>(payload_boundary),
          .reserved3 = 0,
          .blockIndex = 0,
          .blockCount = static_cast<std::uint16_t>(block_count),
          .fecGroupIndex = 0,
          .fecDataCount = 0,
          .fecParityCount = 0,
          .fecShardIndex = 0,
          .reserved4 = 0,
          .payloadLength = static_cast<std::uint16_t>(payload_boundary),
          .reserved5 = 0,
          .reserved6 = 0,
        };
        if (!build_packet(frame_header, frame_header_payload.data(), frame_header_payload.size())) {
          return failure(failure_e::packetization_failed);
        }

        for (std::size_t index = 0; index < data_block_count; ++index) {
          const auto offset = index * payload_boundary;
          const auto source_payload_size = std::min(payload_boundary, protected_payload.size() - offset);
          const auto payload_size = request.block_aware_fec ? payload_boundary : source_payload_size;
          std::vector<std::uint8_t> padded_payload;
          const auto *payload = protected_payload.data() + offset;
          if (source_payload_size != payload_size) {
            padded_payload.assign(payload_size, 0);
            std::copy_n(payload, source_payload_size, padded_payload.data());
            payload = padded_payload.data();
          }
          const auto group_index = request.block_aware_fec
            ? index / LI_PYROWAVE_FEC_DATA_PER_GROUP : 0;
          const auto group_data_count = request.block_aware_fec
            ? std::min<std::size_t>(LI_PYROWAVE_FEC_DATA_PER_GROUP,
              data_block_count - group_index * LI_PYROWAVE_FEC_DATA_PER_GROUP) : 0;
          LI_PYROWAVE_PACKET_HEADER header {
            .version = LI_PYROWAVE_PROTOCOL_VERSION,
            .packetKind = LI_PYROWAVE_PACKET_DATA,
            .flags = static_cast<std::uint8_t>(
              (index == 0 ? LI_PYROWAVE_FLAG_START_OF_FRAME : 0) |
              (index + 1 == data_block_count ? LI_PYROWAVE_FLAG_END_OF_FRAME : 0) |
              common_flags),
            .reserved = 0,
            .headerLength = static_cast<std::uint16_t>(header_size),
            .metadataFlags = request.metadata_flags,
            .frameId = static_cast<std::uint32_t>(frame.frame_id),
            .rtpTimestamp = request.rtp_timestamp,
            .codecPayloadLength = static_cast<std::uint32_t>(input.size()),
            .protectedPayloadLength = static_cast<std::uint32_t>(protected_payload.size()),
            .metadataLength = static_cast<std::uint16_t>(metadata_length),
            .fecScheme = static_cast<std::uint8_t>(fec_scheme),
            .reserved2 = 0,
            .dataBlockCount = static_cast<std::uint16_t>(data_block_count),
            .parityBlockCount = static_cast<std::uint16_t>(fec_group_count),
            .fecBlockPayloadSize = static_cast<std::uint16_t>(payload_boundary),
            .reserved3 = 0,
            .blockIndex = static_cast<std::uint16_t>(index),
            .blockCount = static_cast<std::uint16_t>(block_count),
            .fecGroupIndex = static_cast<std::uint16_t>(group_index),
            .fecDataCount = static_cast<std::uint8_t>(group_data_count),
            .fecParityCount = static_cast<std::uint8_t>(request.block_aware_fec ? LI_PYROWAVE_FEC_PARITY_SHARDS : 0),
            .fecShardIndex = static_cast<std::uint8_t>(request.block_aware_fec
              ? index % LI_PYROWAVE_FEC_DATA_PER_GROUP : 0),
            .reserved4 = 0,
            .payloadLength = static_cast<std::uint16_t>(payload_size),
            .reserved5 = 0,
            .reserved6 = 0,
          };
          if (!build_packet(header, payload, payload_size)) {
            return failure(failure_e::packetization_failed);
          }
        }

        if (request.block_aware_fec) {
          std::vector<std::uint8_t> parity(payload_boundary);
          for (std::size_t group = 0; group < fec_group_count; ++group) {
            std::fill(parity.begin(), parity.end(), 0);
            const auto group_start = group * LI_PYROWAVE_FEC_DATA_PER_GROUP;
            const auto group_end = std::min(data_block_count,
              group_start + LI_PYROWAVE_FEC_DATA_PER_GROUP);
            for (std::size_t index = group_start; index < group_end; ++index) {
              const auto offset = index * payload_boundary;
              for (std::size_t byte = 0; byte < payload_boundary; ++byte) {
                if (offset + byte < protected_payload.size()) {
                  parity[byte] ^= protected_payload[offset + byte];
                }
              }
            }
            const auto group_data_count = group_end - group_start;
            LI_PYROWAVE_PACKET_HEADER header {
              .version = LI_PYROWAVE_PROTOCOL_VERSION,
              .packetKind = LI_PYROWAVE_PACKET_PARITY,
              .flags = static_cast<std::uint8_t>(LI_PYROWAVE_FLAG_FEC_PARITY | common_flags),
              .reserved = 0,
              .headerLength = static_cast<std::uint16_t>(header_size),
              .metadataFlags = request.metadata_flags,
              .frameId = static_cast<std::uint32_t>(frame.frame_id),
              .rtpTimestamp = request.rtp_timestamp,
              .codecPayloadLength = static_cast<std::uint32_t>(input.size()),
              .protectedPayloadLength = static_cast<std::uint32_t>(protected_payload.size()),
              .metadataLength = static_cast<std::uint16_t>(metadata_length),
              .fecScheme = static_cast<std::uint8_t>(fec_scheme),
              .reserved2 = 0,
              .dataBlockCount = static_cast<std::uint16_t>(data_block_count),
              .parityBlockCount = static_cast<std::uint16_t>(fec_group_count),
              .fecBlockPayloadSize = static_cast<std::uint16_t>(payload_boundary),
              .reserved3 = 0,
              .blockIndex = static_cast<std::uint16_t>(data_block_count + group),
              .blockCount = static_cast<std::uint16_t>(block_count),
              .fecGroupIndex = static_cast<std::uint16_t>(group),
              .fecDataCount = static_cast<std::uint8_t>(group_data_count),
              .fecParityCount = LI_PYROWAVE_FEC_PARITY_SHARDS,
              .fecShardIndex = static_cast<std::uint8_t>(group_data_count),
              .reserved4 = 0,
              .payloadLength = static_cast<std::uint16_t>(parity.size()),
              .reserved5 = 0,
              .reserved6 = 0,
            };
            if (!build_packet(header, parity.data(), parity.size())) {
              return failure(failure_e::packetization_failed);
            }
          }
        }

        return {
          .failure = failure_e::none,
          .bitstream = { std::move(storage) },
          .packets = std::move(spans),
        };
      }
      catch (const std::bad_alloc &) {
        try {
          BOOST_LOG(warning) << "[PyroWavePacketizer] failed for frame " << frame.frame_id
                             << ": allocation failure (input_bytes=" << input.size() << ')';
        }
        catch (...) {
        }
        return failure(failure_e::packetization_failed);
      }
      catch (...) {
        try {
          BOOST_LOG(warning) << "[PyroWavePacketizer] failed for frame " << frame.frame_id
                             << ": unexpected exception";
        }
        catch (...) {
        }
        return failure(failure_e::packetization_failed);
      }
    }

    void reset() noexcept override {}
  };

  std::unique_ptr<packetizer_t>
  make_transport_packetizer() {
    return std::make_unique<transport_packetizer_t>();
  }

}  // namespace pyrowave

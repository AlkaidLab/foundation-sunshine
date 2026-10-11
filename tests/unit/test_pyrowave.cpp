/**
 * @file tests/unit/test_pyrowave.cpp
 * @brief Pure-data and state-transition tests for the PyroWave foundation.
 */
#include "src/pyrowave/capabilities.h"
#include "src/pyrowave/capture_policy.h"
#include "src/pyrowave/config.h"
#include "src/pyrowave/gpu_interop.h"
#include "src/pyrowave/packet.h"
#include "src/pyrowave/packetizer.h"
#include "src/pyrowave/session.h"
#include "src/pyrowave/types.h"
#include "src/pyrowave/runtime.h"
#include "src/pyrowave/dynamic_hdr.h"
#include "third-party/moonlight-common-c/tests/PyrowaveDynamicHdrFixtures.h"
#ifdef _WIN32
#include <vulkan/vulkan.h>
#include "src/platform/windows/pyrowave/color_metadata.h"
#include "src/platform/windows/pyrowave/rate_control.h"
#include "src/platform/windows/pyrowave/transport.h"
#endif

extern "C" {
#include "third-party/moonlight-common-c/src/PyrowaveProtocol.h"
#include "third-party/moonlight-common-c/src/PyrowaveReassembly.h"
}

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <memory>
#include <vector>

namespace {

  pyrowave::config_t
  enabled_config() {
    pyrowave::config_t config;
    config.enabled = true;
    config.allow_experimental_client = true;
    return config;
  }

  pyrowave::device_capabilities_t
  supported_device() {
    pyrowave::device_capabilities_t device;
    device.api_version = { 101, 0, 0 };
    device.vulkan_13 = true;
    device.subgroup = true;
    device.subgroup_size_control = true;
    device.shader_int16 = true;
    device.storage_buffer_8bit = true;
    return device;
  }

  pyrowave::client_capabilities_t
  supported_client() {
    pyrowave::client_capabilities_t client;
    client.api_version = { 101, 0, 0 };
    client.pyrowave = true;
    client.reassembly = true;
    client.frame_deadline = true;
    client.block_aware_fec = true;
    return client;
  }

  std::vector<std::uint8_t>
  optional_metadata(std::size_t total_size) {
    std::vector<std::uint8_t> metadata(total_size, 0x5a);
    const auto value_size = static_cast<std::uint32_t>(total_size - 8);
    // Unknown optional TLV, with a valid length and flags.
    metadata[0] = 0x80;
    metadata[1] = 0x00;
    metadata[2] = 0x00;
    metadata[3] = LI_PYROWAVE_METADATA_FLAG_PROTECTED | LI_PYROWAVE_METADATA_FLAG_OPTIONAL;
    metadata[4] = static_cast<std::uint8_t>(value_size >> 24);
    metadata[5] = static_cast<std::uint8_t>(value_size >> 16);
    metadata[6] = static_cast<std::uint8_t>(value_size >> 8);
    metadata[7] = static_cast<std::uint8_t>(value_size);
    return metadata;
  }

#ifdef _WIN32
  SS_HDR_METADATA
  hdr_source_metadata() {
    SS_HDR_METADATA source {};
    source.maxDisplayLuminance = 1000;
    source.minDisplayLuminance = 1;
    source.maxContentLightLevel = 1200;
    source.maxFrameAverageLightLevel = 200;
    return source;
  }

  platf::hdr_frame_luminance_stats_t
  hdr_scene_stats() {
    platf::hdr_frame_luminance_stats_t stats {};
    stats.valid = true;
    stats.sample_sequence = 1;
    stats.min_maxrgb = 0;
    stats.max_maxrgb = 1000;
    stats.avg_maxrgb = 100;
    stats.avg_maxrgb_pq = video::hdr_metadata::nits_to_pq(100);
    stats.percentile_1_pq = 0;
    stats.near_black_fraction = 0.05f;
    stats.near_black_stats_valid = true;
    stats.percentile_10_pq = video::hdr_metadata::nits_to_pq(10);
    stats.percentile_90_pq = video::hdr_metadata::nits_to_pq(700);
    stats.percentile_99 = 1000;
    stats.analysis_max_nits = 1000;
    const float distribution[] = { 0, 5, 10, 25, 100, 400, 700, 900, 1000 };
    std::copy(std::begin(distribution), std::end(distribution), stats.distribution_maxrgb);
    return stats;
  }
#endif

}  // namespace

#ifdef _WIN32
TEST(PyrowaveApi, UsesTheForkVersionNamespace) {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  std::uint32_t patch = 0;
  pyrowave_get_api_version(&major, &minor, &patch);
  EXPECT_EQ(major, 101u);
  EXPECT_EQ(minor, 0u);
  EXPECT_EQ(patch, 0u);
  const pyrowave::config_t config;
  EXPECT_EQ(config.expected_api_version.major, major);
  EXPECT_EQ(config.expected_api_version.minor, minor);
  EXPECT_EQ(config.expected_api_version.patch, patch);
}

TEST(PyrowaveApi, RejectsInvalidContextsWithoutAccessingADevice) {
  pyrowave_cpu_buffer buffer {};
  EXPECT_EQ(pyrowave_encoder_set_frame_context(nullptr, -1), PYROWAVE_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(pyrowave_encoder_set_frame_context(nullptr, PYROWAVE_MAX_FRAME_CONTEXTS), PYROWAVE_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(pyrowave_decoder_decode_cpu_buffer_async(nullptr, &buffer, -1), PYROWAVE_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(pyrowave_decoder_decode_cpu_buffer_complete(nullptr, &buffer, -1), PYROWAVE_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(pyrowave_decoder_decode_cpu_buffer_async(nullptr, &buffer, PYROWAVE_MAX_FRAME_CONTEXTS), PYROWAVE_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(pyrowave_decoder_decode_cpu_buffer_complete(nullptr, &buffer, PYROWAVE_MAX_FRAME_CONTEXTS), PYROWAVE_ERROR_INVALID_ARGUMENT);
}
#endif

TEST(PyrowaveConfigTest, RejectsMissingApiVersion) {
  auto config = enabled_config();
  config.expected_api_version = {};
  EXPECT_FALSE(pyrowave::validate(config));
}

TEST(PyrowaveConfigTest, RejectsOddYuv420Stream) {
  pyrowave::stream_config_t config;
  config.width = 1919;
  config.height = 1080;
  config.framerate_num = 60;
  EXPECT_FALSE(pyrowave::validate(config));
}

TEST(PyrowaveCapabilityTest, RequiresExactApiVersion) {
  auto config = enabled_config();
  auto device = supported_device();
  auto client = supported_client();
  client.api_version.minor = 5;

  const auto result = pyrowave::evaluate_capabilities(config, device, client);
  EXPECT_EQ(result.availability, pyrowave::availability_e::failed);
  EXPECT_EQ(result.failure, pyrowave::failure_e::api_mismatch);
}

TEST(PyrowaveCapturePolicyTest, RejectsFallbackWhileAnotherSessionIsRegistered) {
  pyrowave::capture_policy_t policy;
  ASSERT_TRUE(policy.attach(true));
  ASSERT_TRUE(policy.attach(false));
  EXPECT_FALSE(policy.request_system_capture());
  EXPECT_FALSE(policy.system_capture());
  policy.detach();
  EXPECT_TRUE(policy.request_system_capture());
}

TEST(PyrowaveCapturePolicyTest, KeepsFallbackAcrossReinitAndRejectsIncompatibleJoins) {
  pyrowave::capture_policy_t policy;
  ASSERT_TRUE(policy.attach(true));
  ASSERT_TRUE(policy.request_system_capture());
  EXPECT_TRUE(policy.system_capture());
  EXPECT_FALSE(policy.attach(false));
  EXPECT_TRUE(policy.attach(true));
  EXPECT_TRUE(policy.request_system_capture());
  policy.detach();
  EXPECT_TRUE(policy.system_capture());
  policy.detach();
  EXPECT_TRUE(policy.system_capture());
  EXPECT_FALSE(policy.attach(false));
  EXPECT_FALSE(policy.request_system_capture());
  pyrowave::capture_policy_t next_capture;
  EXPECT_FALSE(next_capture.system_capture());
  EXPECT_TRUE(next_capture.attach(false));
}

TEST(PyrowavePacketTest, NegotiatedWireLimitAlsoBoundsOuterRtpGeometry) {
  EXPECT_EQ(pyrowave::limit_rtp_packet_size(1392, 65536), 1392);
  EXPECT_EQ(pyrowave::limit_rtp_packet_size(1392, 1024), 1040);
  EXPECT_EQ(pyrowave::limit_rtp_packet_size(1000, 1024), 1000);
  EXPECT_EQ(pyrowave::limit_rtp_packet_size(-1, 1024), 0);
  EXPECT_EQ(pyrowave::limit_rtp_packet_size(1392, 128), 0);
  EXPECT_EQ(pyrowave::limit_rtp_packet_size(1392, 65537), 0);
  EXPECT_TRUE(pyrowave::validate(pyrowave::packetization_request_t {
    .packet_boundary = LI_PYROWAVE_MAX_PACKET_SIZE,
  }));
  EXPECT_FALSE(pyrowave::validate(pyrowave::packetization_request_t {
    .packet_boundary = LI_PYROWAVE_MAX_PACKET_SIZE + 1u,
  }));
}

TEST(PyrowaveCapabilityTest, RequiresBlockAwareFec) {
  auto config = enabled_config();
  auto device = supported_device();
  auto client = supported_client();
  client.block_aware_fec = false;

  const auto result = pyrowave::evaluate_capabilities(config, device, client);
  EXPECT_EQ(result.availability, pyrowave::availability_e::failed);
  EXPECT_EQ(result.failure, pyrowave::failure_e::client_unsupported);
}

TEST(PyrowavePacketTest, OwnsEncodedBuffer) {
  auto bytes = std::make_shared<const std::vector<std::uint8_t>>(
    std::initializer_list<std::uint8_t> { 1, 2, 3 });
  pyrowave::encoded_frame_t frame;
  frame.frame_id = 1;
  frame.bitstream.storage = bytes;

  bytes.reset();
  ASSERT_TRUE(pyrowave::validate(frame));
  ASSERT_EQ(frame.bitstream.view().size(), 3u);
  EXPECT_EQ(frame.bitstream.view()[1], 2u);
}

TEST(PyrowavePacketTest, RejectsOutOfBoundsPacket) {
  auto bytes = std::make_shared<const std::vector<std::uint8_t>>(
    std::initializer_list<std::uint8_t> { 1, 2, 3 });
  pyrowave::packetization_result_t result;
  result.bitstream.storage = bytes;
  result.packets.push_back({ 2, 2 });

  EXPECT_FALSE(pyrowave::validate(result));
}

TEST(PyrowaveConfigTest, AcceptsStaticHlgTransfer) {
  pyrowave::config_t config = enabled_config();
  config.transfer = pyrowave::transfer_e::hlg;
  EXPECT_TRUE(pyrowave::validate(config));
}

TEST(PyrowaveConfigTest, AcceptsStaticHlgStream) {
  pyrowave::stream_config_t config;
  config.width = 1920;
  config.height = 1080;
  config.framerate_num = 60;
  config.transfer = pyrowave::transfer_e::hlg;
  EXPECT_TRUE(pyrowave::validate(config));
}

TEST(PyrowavePacketizerTest, WrapsFrameWithExactBoundedPackets) {
  auto source = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>(9000, 0x5a));
  pyrowave::encoded_frame_t frame {
    .frame_id = 7,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  const auto result = packetizer->packetize(frame, {
    .packet_boundary = 1024,
    .rtp_timestamp = 1234,
    .mark_critical = true,
  });

  ASSERT_EQ(result.failure, pyrowave::failure_e::none);
  ASSERT_FALSE(result.packets.empty());
  ASSERT_TRUE(pyrowave::validate(result));

  for (std::size_t index = 0; index < result.packets.size(); ++index) {
    LI_PYROWAVE_PACKET_HEADER header {};
    const std::uint8_t *payload = nullptr;
    const auto span = result.packets[index];
    ASSERT_EQ(LiPyrowaveParsePacket(
                result.bitstream.view().data() + span.offset,
                span.size,
                &header,
                &payload), LI_PYROWAVE_PACKET_OK);
    EXPECT_EQ(header.frameId, 7u);
    EXPECT_EQ(header.rtpTimestamp, 1234u);
    if (header.packetKind == LI_PYROWAVE_PACKET_FRAME_HEADER) {
      EXPECT_EQ(index, 0u);
      EXPECT_EQ(header.blockIndex, 0u);
    }
    else {
      EXPECT_EQ(header.blockIndex, index - 1);
    }
    EXPECT_EQ(header.blockCount, result.packets.size() - 1);
    EXPECT_NE(header.flags & LI_PYROWAVE_FLAG_CRITICAL, 0u);
  }
}

TEST(PyrowavePacketizerTest, ProtectsMetadataAndStripsItBeforeDecode) {
  auto source = std::make_shared<const std::vector<std::uint8_t>>(
    std::initializer_list<std::uint8_t> { 0xa0, 0xb1, 0xc2, 0xd3 });
  pyrowave::encoded_frame_t frame {
    .frame_id = 13,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source },
  };
  const std::vector<std::uint8_t> metadata {
    0x01, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x02, 0x00, 0x2a,
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  const auto result = packetizer->packetize(frame, {
    .packet_boundary = 1024,
    .rtp_timestamp = 1313,
    .metadata = metadata,
    .metadata_flags = LI_PYROWAVE_METADATA_FLAG_PROTECTED |
                      LI_PYROWAVE_METADATA_FLAG_RUNTIME |
                      LI_PYROWAVE_METADATA_FLAG_OPTIONAL,
  });

  ASSERT_EQ(result.failure, pyrowave::failure_e::none);
  LI_PYROWAVE_REASSEMBLY_STATE state;
  LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
  for (const auto &span : result.packets) {
    const auto *packet = result.bitstream.view().data() + span.offset;
    const auto status = LiPyrowaveReassemblyPushPacket(&state, packet, span.size, 5000, 100000);
    EXPECT_TRUE(status == LI_PYROWAVE_REASSEMBLY_ACCEPTED ||
                status == LI_PYROWAVE_REASSEMBLY_COMPLETE);
  }
  ASSERT_TRUE(LiPyrowaveReassemblyIsComplete(&state));

  std::vector<std::uint8_t> metadata_out(metadata.size());
  std::size_t metadata_length = 0;
  std::uint16_t metadata_flags = 0;
  EXPECT_EQ(LiPyrowaveReassemblyCopyMetadata(
              &state, metadata_out.data(), metadata_out.size(), &metadata_length, &metadata_flags),
            LI_PYROWAVE_REASSEMBLY_COMPLETE);
  EXPECT_EQ(metadata_length, metadata.size());
  EXPECT_EQ(metadata_out, metadata);
  EXPECT_EQ(metadata_flags, LI_PYROWAVE_METADATA_FLAG_PROTECTED |
                            LI_PYROWAVE_METADATA_FLAG_RUNTIME |
                            LI_PYROWAVE_METADATA_FLAG_OPTIONAL);

  std::vector<std::uint8_t> output(source->size());
  std::size_t output_length = 0;
  EXPECT_EQ(LiPyrowaveReassemblyCopyFrame(
              &state, output.data(), output.size(), &output_length, nullptr),
            LI_PYROWAVE_REASSEMBLY_COMPLETE);
  EXPECT_EQ(output_length, source->size());
  EXPECT_EQ(output, *source);
  LiPyrowaveReassemblyDestroy(&state);
}

TEST(PyrowavePacketizerTest, RejectsMetadataLargerThanProtocolLimit) {
  const pyrowave::encoded_frame_t frame {
    .frame_id = 14,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { std::make_shared<const std::vector<std::uint8_t>>(4, 0x5a) },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  for (const std::size_t boundary : { 128u, 0u, LI_PYROWAVE_MAX_PACKET_SIZE }) {
    for (const bool fec : { false, true }) {
      SCOPED_TRACE(::testing::Message() << "boundary=" << boundary << ", fec=" << fec);
      const pyrowave::packetization_request_t request {
        .packet_boundary = boundary,
        .block_aware_fec = fec,
        .metadata = optional_metadata(LI_PYROWAVE_MAX_METADATA_SIZE + 1),
        .metadata_flags = LI_PYROWAVE_METADATA_FLAG_PROTECTED | LI_PYROWAVE_METADATA_FLAG_OPTIONAL,
      };
      // Fail before exercising the copy if the request guard regresses.
      ASSERT_FALSE(pyrowave::validate(request));
      const auto result = packetizer->packetize(frame, request);
      EXPECT_EQ(result.failure, pyrowave::failure_e::configuration_invalid);
      EXPECT_TRUE(result.bitstream.empty());
      EXPECT_TRUE(result.packets.empty());
    }
  }
}

TEST(PyrowavePacketizerTest, RoundTripsProtectedMetadataAcrossPacketsAndLostBlock) {
  const auto source = std::make_shared<const std::vector<std::uint8_t>>(4, 0xa5);
  const pyrowave::encoded_frame_t frame {
    .frame_id = 15,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  for (const std::size_t boundary : { 128u, 0u, LI_PYROWAVE_MAX_PACKET_SIZE }) {
    const auto effective_boundary = boundary == 0 ? LI_PYROWAVE_MAX_PACKET_SIZE : boundary;
    for (const bool fec : { false, true }) {
      SCOPED_TRACE(::testing::Message() << "boundary=" << boundary << ", fec=" << fec);
      const pyrowave::packetization_request_t request {
        .packet_boundary = boundary,
        .block_aware_fec = fec,
        .metadata = optional_metadata(std::min<std::size_t>(effective_boundary * 3, LI_PYROWAVE_MAX_METADATA_SIZE)),
        .metadata_flags = LI_PYROWAVE_METADATA_FLAG_PROTECTED | LI_PYROWAVE_METADATA_FLAG_OPTIONAL,
      };
      ASSERT_TRUE(pyrowave::validate(request));
      const auto result = packetizer->packetize(frame, request);
      ASSERT_EQ(result.failure, pyrowave::failure_e::none);
      LI_PYROWAVE_REASSEMBLY_STATE state;
      LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
      bool dropped_data = false;
      for (const auto &packet : result.packets) {
        EXPECT_LE(packet.size, effective_boundary);
        LI_PYROWAVE_PACKET_HEADER header {};
        const std::uint8_t *payload = nullptr;
        ASSERT_EQ(LiPyrowaveParsePacket(result.bitstream.view().data() + packet.offset, packet.size, &header, &payload),
                  LI_PYROWAVE_PACKET_OK);
        if (fec && !dropped_data && header.packetKind == LI_PYROWAVE_PACKET_DATA &&
            (header.flags & LI_PYROWAVE_FLAG_FEC_PARITY) == 0) {
          dropped_data = true;
          continue;
        }
        const auto status = LiPyrowaveReassemblyPushPacket(&state,
          result.bitstream.view().data() + packet.offset, packet.size, 5000, 100000);
        EXPECT_TRUE(status == LI_PYROWAVE_REASSEMBLY_ACCEPTED ||
                    status == LI_PYROWAVE_REASSEMBLY_COMPLETE ||
                    status == LI_PYROWAVE_REASSEMBLY_DUPLICATE);
      }
      EXPECT_TRUE(LiPyrowaveReassemblyIsComplete(&state));
      std::vector<std::uint8_t> metadata_out(request.metadata.size());
      std::size_t written = 0;
      EXPECT_EQ(LiPyrowaveReassemblyCopyMetadata(&state, metadata_out.data(), metadata_out.size(),
        &written, nullptr), LI_PYROWAVE_REASSEMBLY_COMPLETE);
      EXPECT_EQ(written, request.metadata.size());
      EXPECT_EQ(metadata_out, request.metadata);
      std::vector<std::uint8_t> decoded_bytes(source->size());
      EXPECT_EQ(LiPyrowaveReassemblyCopyFrame(&state, decoded_bytes.data(), decoded_bytes.size(),
        &written, nullptr), LI_PYROWAVE_REASSEMBLY_COMPLETE);
      EXPECT_EQ(written, source->size());
      EXPECT_EQ(decoded_bytes, *source);
      LiPyrowaveReassemblyDestroy(&state);
    }
  }
}

TEST(PyrowavePacketizerTest, SegmentedInputMatchesConcatenatedReferenceBytes) {
  constexpr std::size_t payload_size = 32;
  constexpr std::size_t boundary = LI_PYROWAVE_WIRE_HEADER_SIZE + payload_size;
  auto packetizer = pyrowave::make_transport_packetizer();
  for (const auto metadata_size : { 0u, 8u, 31u, 32u, 33u, 63u, 64u, 65u }) {
    for (const auto codec_size : { 1u, 31u, 32u, 33u, 577u }) {
      for (const bool fec : { false, true }) {
        SCOPED_TRACE(::testing::Message() << "metadata=" << metadata_size << ", codec=" << codec_size << ", fec=" << fec);
        auto codec = std::make_shared<std::vector<std::uint8_t>>(codec_size);
        for (std::size_t i = 0; i < codec->size(); ++i) {
          (*codec)[i] = static_cast<std::uint8_t>(i * 37 + 11);
        }
        const pyrowave::encoded_frame_t frame {
          .frame_id = 17,
          .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
          .bitstream = { codec },
        };
        const pyrowave::packetization_request_t request {
          .packet_boundary = boundary,
          .rtp_timestamp = 1717,
          .block_aware_fec = fec,
          .mark_critical = true,
          .metadata = metadata_size == 0 ? std::vector<std::uint8_t> {} : optional_metadata(metadata_size),
          .metadata_flags = static_cast<std::uint16_t>(metadata_size == 0 ? 0 :
            LI_PYROWAVE_METADATA_FLAG_PROTECTED | LI_PYROWAVE_METADATA_FLAG_OPTIONAL),
        };
        const auto result = packetizer->packetize(frame, request);
        ASSERT_EQ(result.failure, pyrowave::failure_e::none);
        ASSERT_TRUE(pyrowave::validate(result));

        // Keep the old concatenate-then-slice model only as a byte reference.
        auto protected_bytes = request.metadata;
        protected_bytes.insert(protected_bytes.end(), codec->begin(), codec->end());
        const auto data_count = (protected_bytes.size() + payload_size - 1) / payload_size;
        const auto parity_count = fec ? (data_count + LI_PYROWAVE_FEC_DATA_PER_GROUP - 1) / LI_PYROWAVE_FEC_DATA_PER_GROUP : 0;
        const auto common_flags = static_cast<std::uint8_t>(LI_PYROWAVE_FLAG_CRITICAL |
          (metadata_size == 0 ? 0 : LI_PYROWAVE_FLAG_METADATA_PRESENT));
        LI_PYROWAVE_PACKET_HEADER common {};
        common.version = LI_PYROWAVE_PROTOCOL_VERSION;
        common.headerLength = LI_PYROWAVE_WIRE_HEADER_SIZE;
        common.metadataFlags = request.metadata_flags;
        common.frameId = 17;
        common.rtpTimestamp = 1717;
        common.codecPayloadLength = codec_size;
        common.protectedPayloadLength = static_cast<std::uint32_t>(protected_bytes.size());
        common.metadataLength = static_cast<std::uint16_t>(metadata_size);
        common.fecScheme = fec ? LI_PYROWAVE_FEC_SCHEME_XOR : LI_PYROWAVE_FEC_SCHEME_NONE;
        common.dataBlockCount = static_cast<std::uint16_t>(data_count);
        common.parityBlockCount = static_cast<std::uint16_t>(parity_count);
        common.fecBlockPayloadSize = payload_size;
        common.blockCount = static_cast<std::uint16_t>(data_count + parity_count);

        std::vector<std::uint8_t> reference;
        std::vector<pyrowave::packet_span_t> reference_spans;
        const auto append_reference = [&](LI_PYROWAVE_PACKET_HEADER header, const std::vector<std::uint8_t> &payload) {
          header.payloadLength = static_cast<std::uint16_t>(payload.size());
          std::vector<std::uint8_t> wire(boundary);
          std::size_t written = 0;
          ASSERT_EQ(LiPyrowaveBuildPacket(&header, payload.data(), payload.size(), wire.data(), wire.size(), &written),
                    LI_PYROWAVE_PACKET_OK);
          reference_spans.push_back({ reference.size(), written });
          reference.insert(reference.end(), wire.begin(), wire.begin() + written);
        };
        auto header = common;
        header.packetKind = LI_PYROWAVE_PACKET_FRAME_HEADER;
        header.flags = LI_PYROWAVE_FLAG_START_OF_FRAME | common_flags;
        std::vector<std::uint8_t> repeated_metadata(payload_size, 0);
        std::copy_n(request.metadata.begin(), std::min(request.metadata.size(), payload_size), repeated_metadata.begin());
        append_reference(header, repeated_metadata);

        for (std::size_t index = 0; index < data_count; ++index) {
          const auto offset = index * payload_size;
          const auto count = std::min(payload_size, protected_bytes.size() - offset);
          std::vector<std::uint8_t> payload(protected_bytes.begin() + offset, protected_bytes.begin() + offset + count);
          if (fec) payload.resize(payload_size, 0);
          header = common;
          header.packetKind = LI_PYROWAVE_PACKET_DATA;
          header.flags = common_flags |
            (index == 0 ? LI_PYROWAVE_FLAG_START_OF_FRAME : 0) |
            (index + 1 == data_count ? LI_PYROWAVE_FLAG_END_OF_FRAME : 0);
          header.blockIndex = static_cast<std::uint16_t>(index);
          if (fec) {
            const auto group = index / LI_PYROWAVE_FEC_DATA_PER_GROUP;
            header.fecGroupIndex = static_cast<std::uint16_t>(group);
            header.fecDataCount = static_cast<std::uint8_t>(std::min<std::size_t>(LI_PYROWAVE_FEC_DATA_PER_GROUP,
              data_count - group * LI_PYROWAVE_FEC_DATA_PER_GROUP));
            header.fecParityCount = LI_PYROWAVE_FEC_PARITY_SHARDS;
            header.fecShardIndex = static_cast<std::uint8_t>(index % LI_PYROWAVE_FEC_DATA_PER_GROUP);
          }
          append_reference(header, payload);
        }

        for (std::size_t group = 0; group < parity_count; ++group) {
          const auto group_start = group * LI_PYROWAVE_FEC_DATA_PER_GROUP;
          const auto group_end = std::min(data_count, group_start + LI_PYROWAVE_FEC_DATA_PER_GROUP);
          std::vector<std::uint8_t> parity(payload_size, 0);
          for (std::size_t index = group_start; index < group_end; ++index) {
            const auto offset = index * payload_size;
            for (std::size_t byte = 0; byte < payload_size && offset + byte < protected_bytes.size(); ++byte) {
              parity[byte] ^= protected_bytes[offset + byte];
            }
          }
          header = common;
          header.packetKind = LI_PYROWAVE_PACKET_PARITY;
          header.flags = LI_PYROWAVE_FLAG_FEC_PARITY | common_flags;
          header.blockIndex = static_cast<std::uint16_t>(data_count + group);
          header.fecGroupIndex = static_cast<std::uint16_t>(group);
          header.fecDataCount = static_cast<std::uint8_t>(group_end - group_start);
          header.fecParityCount = LI_PYROWAVE_FEC_PARITY_SHARDS;
          header.fecShardIndex = header.fecDataCount;
          append_reference(header, parity);
        }
        EXPECT_EQ(*result.bitstream.storage, reference);
        ASSERT_EQ(result.packets.size(), reference_spans.size());
        for (std::size_t i = 0; i < reference_spans.size(); ++i) {
          EXPECT_EQ(result.packets[i].offset, reference_spans[i].offset);
          EXPECT_EQ(result.packets[i].size, reference_spans[i].size);
        }
      }
    }
  }
}

#ifdef _WIN32
TEST(PyrowaveDynamicHdrTest, ProducesEachDesktopMetadataProfile) {
  const auto source = hdr_source_metadata();
  const auto stats = hdr_scene_stats();
  for (int format = 1; format <= 5; ++format) {
    pyrowave::hdr_metadata_producer_t producer;
    // The shared fixture payloads use the source's 1000-nit target. Target
    // dependent Vivid behavior is covered separately; keep this test focused
    // on the registered type and the golden syntax vectors.
    ASSERT_TRUE(producer.configure(format, source, 1000));
    EXPECT_TRUE(producer.enabled());
    const auto *metadata = producer.build(stats);
    ASSERT_NE(metadata, nullptr);
    EXPECT_EQ(metadata->type, LiPyrowaveDynamicHdrMetadataType(format));
    EXPECT_EQ(metadata->hlg_nominal_peak_nits, (format == 3 || format == 5) ? 1000 : 0);
    ASSERT_FALSE(metadata->payload.empty());
    const auto &fixture = PyrowaveDynamicHdrFixtures[format - 1];
    const std::string_view hex(fixture.payloadHex);
    std::vector<std::uint8_t> expected;
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
      expected.push_back(static_cast<std::uint8_t>((digit(hex[i]) << 4) | digit(hex[i + 1])));
    }
    // This shared fixture targets the same 1000-nit peak as its source scene,
    // so HDR10+ does not need a tone-mapping curve here. The fixture's two
    // targeted-display bytes retain its historical 500-nit value; verify the
    // target-independent syntax around that field and cover target-dependent
    // generation in the dedicated metadata tests.
    if (format == 1) {
      ASSERT_EQ(metadata->payload.size(), expected.size());
      EXPECT_TRUE(std::equal(metadata->payload.begin(), metadata->payload.begin() + 9, expected.begin()));
      EXPECT_TRUE(std::equal(metadata->payload.begin() + 11, metadata->payload.end(), expected.begin() + 11));
    }
    else {
      EXPECT_EQ(metadata->payload, expected);
    }
    ASSERT_TRUE(producer.configure(0, source, 500));
    EXPECT_FALSE(producer.enabled());
    EXPECT_EQ(producer.build(stats), nullptr);
  }
}

TEST(PyrowaveDynamicHdrTest, DoesNotInventMetadataBeforeTheFirstValidScene) {
  for (int format = 1; format <= 5; ++format) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    ASSERT_TRUE(producer.configure(format, hdr_source_metadata(), 500));
    EXPECT_EQ(producer.build({}), nullptr);
    auto invalid = hdr_scene_stats();
    invalid.avg_maxrgb = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(producer.build(invalid), nullptr);
    ASSERT_NE(producer.build(hdr_scene_stats()), nullptr);
  }
}

TEST(PyrowaveDynamicHdrTest, KeepsValidSceneMetadataDuringReadbackGaps) {
  for (int format = 1; format <= 5; ++format) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    ASSERT_TRUE(producer.configure(format, hdr_source_metadata(), 500));
    ASSERT_NE(producer.build(hdr_scene_stats()), nullptr);
    // The first Dolby RPU has scene_refresh; subsequent pictures must not.
    const auto *metadata = producer.build(hdr_scene_stats());
    ASSERT_NE(metadata, nullptr);
    const auto expected = *metadata;
    for (int frame = 0; frame < 8; ++frame) {
      metadata = producer.build({});
      ASSERT_NE(metadata, nullptr);
      EXPECT_EQ(metadata->type, expected.type);
      EXPECT_EQ(metadata->hlg_nominal_peak_nits, expected.hlg_nominal_peak_nits);
      EXPECT_EQ(metadata->payload, expected.payload);
    }
  }
}

TEST(PyrowaveDynamicHdrTest, InvalidNewSamplesDoNotPoisonTheCachedScene) {
  for (int format = 1; format <= 5; ++format) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    const auto stats = hdr_scene_stats();
    ASSERT_TRUE(producer.configure(format, hdr_source_metadata(), 500));
    ASSERT_NE(producer.build(stats), nullptr);
    const auto *metadata = producer.build(stats);
    ASSERT_NE(metadata, nullptr);
    const auto expected = metadata->payload;

    std::vector<platf::hdr_frame_luminance_stats_t> invalid(7, stats);
    invalid[0].avg_maxrgb_pq = std::numeric_limits<float>::quiet_NaN();
    invalid[1].avg_maxrgb = std::numeric_limits<float>::infinity();
    invalid[2].min_maxrgb = -1;
    invalid[3].percentile_99 = -1;
    invalid[4].percentile_90_pq = 1.1f;
    invalid[5].distribution_maxrgb[3] = std::numeric_limits<float>::quiet_NaN();
    invalid[6].percentile_10_pq = -0.1f;
    if (format != 1) {
      auto missing_pq_average = stats;
      missing_pq_average.avg_maxrgb_pq = 0;
      invalid.push_back(missing_pq_average);
    }
    for (auto &candidate : invalid) {
      candidate.sample_sequence = 2;
      metadata = producer.build(candidate);
      ASSERT_NE(metadata, nullptr);
      EXPECT_EQ(metadata->payload, expected);
      metadata = producer.build(stats);
      ASSERT_NE(metadata, nullptr);
      EXPECT_EQ(metadata->payload, expected);
    }
  }
}

TEST(PyrowaveDynamicHdrTest, ANewSceneReplacesCachedMetadata) {
  for (int format = 1; format <= 5; ++format) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    ASSERT_TRUE(producer.configure(format, hdr_source_metadata(), 500));
    ASSERT_NE(producer.build(hdr_scene_stats()), nullptr);
    const auto *metadata = producer.build(hdr_scene_stats());
    ASSERT_NE(metadata, nullptr);
    const auto previous = metadata->payload;

    auto scene = hdr_scene_stats();
    scene.sample_sequence = 2;
    scene.max_maxrgb = 3000;
    scene.avg_maxrgb = 1000;
    scene.avg_maxrgb_pq = video::hdr_metadata::nits_to_pq(1000);
    scene.percentile_90_pq = video::hdr_metadata::nits_to_pq(2100);
    scene.percentile_99 = 3000;
    scene.analysis_max_nits = 3000;
    for (auto &value : scene.distribution_maxrgb) value *= 3;
    metadata = producer.build(scene);
    ASSERT_NE(metadata, nullptr);
    EXPECT_NE(metadata->payload, previous);
    metadata = producer.build({});
    ASSERT_NE(metadata, nullptr);
    const auto current = metadata->payload;
    EXPECT_NE(current, previous);
    metadata = producer.build({});
    ASSERT_NE(metadata, nullptr);
    EXPECT_EQ(metadata->payload, current);
  }
}

TEST(PyrowaveDynamicHdrTest, ReconfigurationClearsThePreviousScene) {
  for (int format = 1; format <= 5; ++format) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    const auto source = hdr_source_metadata();
    ASSERT_TRUE(producer.configure(format, source, 500));
    ASSERT_NE(producer.build(hdr_scene_stats()), nullptr);
    ASSERT_TRUE(producer.configure(format, source, 600));
    EXPECT_EQ(producer.build({}), nullptr);
    ASSERT_NE(producer.build(hdr_scene_stats()), nullptr);
    ASSERT_TRUE(producer.configure(format % 5 + 1, source, 600));
    EXPECT_EQ(producer.build({}), nullptr);
    ASSERT_NE(producer.build(hdr_scene_stats()), nullptr);
    ASSERT_TRUE(producer.configure(0, source, 600));
    EXPECT_EQ(producer.build(hdr_scene_stats()), nullptr);
    ASSERT_TRUE(producer.configure(format, source, 600));
    EXPECT_EQ(producer.build({}), nullptr);
    ASSERT_NE(producer.build(hdr_scene_stats()), nullptr);
    EXPECT_FALSE(producer.configure(99, source, 600));
    EXPECT_FALSE(producer.enabled());
    EXPECT_EQ(producer.build({}), nullptr);
  }
}

TEST(PyrowaveDynamicHdrTest, ABlackSceneCanInitializeMetadata) {
  platf::hdr_frame_luminance_stats_t black {};
  black.valid = true;
  black.sample_sequence = 1;
  for (int format = 1; format <= 5; ++format) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    ASSERT_TRUE(producer.configure(format, hdr_source_metadata(), 500));
    const auto *metadata = producer.build(black);
    ASSERT_NE(metadata, nullptr);
    EXPECT_FALSE(metadata->payload.empty());
    EXPECT_NE(producer.build({}), nullptr);
  }
}

TEST(PyrowaveDynamicHdrTest, DolbySceneRefreshIsNotRepeatedAcrossReadbackGaps) {
  const auto source = hdr_source_metadata();
  const auto stats = hdr_scene_stats();
  for (int format : { 4, 5 }) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    ASSERT_TRUE(producer.configure(format, source, 500));
    video::dolby_vision::rpu_generator_t reference;
    ASSERT_TRUE(reference.configure({
      .source_mastering_peak_nits = source.maxDisplayLuminance,
      .mastering_min_nits_x10000 = source.minDisplayLuminance,
      .max_cll_nits = source.maxContentLightLevel,
      .max_fall_nits = source.maxFrameAverageLightLevel,
    }));
    auto raw = video::dolby_vision::frame_metadata_from_stats(stats);
    ASSERT_TRUE(raw);
    raw->scene_refresh = true;
    auto expected = reference.generate(*raw);
    const auto *metadata = producer.build(stats);
    ASSERT_NE(metadata, nullptr);
    EXPECT_EQ(metadata->payload, (std::vector<std::uint8_t>(expected.begin(), expected.end())));
    raw->scene_refresh = false;
    expected = reference.generate(*raw);
    for (int frame = 0; frame < 3; ++frame) {
      metadata = producer.build({});
      ASSERT_NE(metadata, nullptr);
      EXPECT_EQ(metadata->payload, (std::vector<std::uint8_t>(expected.begin(), expected.end())));
    }
  }
}

TEST(PyrowaveDynamicHdrTest, DolbySmoothingAdvancesPerEncodedFrameWithoutANewSample) {
  const auto source = hdr_source_metadata();
  const auto initial = hdr_scene_stats();
  auto next = initial;
  next.sample_sequence = 2;
  next.avg_maxrgb = 115;
  next.avg_maxrgb_pq = video::hdr_metadata::nits_to_pq(115);
  next.percentile_99 = 1100;
  for (int format : { 4, 5 }) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    ASSERT_TRUE(producer.configure(format, source, 500));
    ASSERT_NE(producer.build(initial), nullptr);
    video::dolby_vision::rpu_generator_t reference;
    ASSERT_TRUE(reference.configure({
      .source_mastering_peak_nits = source.maxDisplayLuminance,
      .mastering_min_nits_x10000 = source.minDisplayLuminance,
      .max_cll_nits = source.maxContentLightLevel,
      .max_fall_nits = source.maxFrameAverageLightLevel,
    }));
    const auto initial_raw = video::dolby_vision::frame_metadata_from_stats(initial);
    const auto next_raw = video::dolby_vision::frame_metadata_from_stats(next);
    ASSERT_TRUE(initial_raw);
    ASSERT_TRUE(next_raw);
    video::dolby_vision::level1_temporal_filter_t filter;
    filter.update(*initial_raw);
    for (int frame = 0; frame < 3; ++frame) {
      const auto expected = reference.generate(filter.update(*next_raw));
      const auto *metadata = producer.build(frame == 0 ? next : platf::hdr_frame_luminance_stats_t {});
      ASSERT_NE(metadata, nullptr);
      EXPECT_EQ(metadata->payload, (std::vector<std::uint8_t>(expected.begin(), expected.end())));
    }
  }
}

TEST(PyrowaveDynamicHdrTest, CachedSceneMetadataStillTravelsInEveryProtectedFrame) {
  for (int format = 1; format <= 5; ++format) {
    SCOPED_TRACE(format);
    pyrowave::hdr_metadata_producer_t producer;
    ASSERT_TRUE(producer.configure(format, hdr_source_metadata(), 500));
    auto mail = std::make_shared<safe::mail_raw_t>();
    auto packets = mail->queue<video::packet_t>("pyrowave-cached-scene-hdr");
    for (int frame = 1; frame <= 2; ++frame) {
      const auto *metadata = producer.build(frame == 1 ? hdr_scene_stats() : platf::hdr_frame_luminance_stats_t {});
      ASSERT_NE(metadata, nullptr);
      const auto source = std::make_shared<const std::vector<std::uint8_t>>(2000, 0x63);
      const auto published = platf::pyrowave_windows::publish_transport_frame(
        frame, source, 1312, 10, packets, nullptr, std::nullopt, std::nullopt, metadata);
      ASSERT_TRUE(published.success);
      auto output = packets->pop();
      ASSERT_NE(output, nullptr);
      LI_PYROWAVE_REASSEMBLY_STATE state;
      LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
      for (std::size_t offset = 0; offset < output->data_size(); offset += 1376) {
        LiPyrowaveReassemblyPushPacket(&state, output->data() + offset, 1376, 1000, 100000);
      }
      ASSERT_TRUE(LiPyrowaveReassemblyIsComplete(&state));
      std::vector<std::uint8_t> recovered(8 + metadata->payload.size() + (metadata->hlg_nominal_peak_nits ? 10 : 0));
      std::size_t written = 0;
      std::uint16_t flags = 0;
      ASSERT_EQ(LiPyrowaveReassemblyCopyMetadata(&state, recovered.data(), recovered.size(), &written, &flags),
                LI_PYROWAVE_REASSEMBLY_COMPLETE);
      EXPECT_EQ(written, recovered.size());
      EXPECT_EQ(flags, LI_PYROWAVE_METADATA_FLAG_PROTECTED);
      EXPECT_EQ(recovered[1], metadata->type);
      EXPECT_EQ(recovered[3], LI_PYROWAVE_METADATA_FLAG_PROTECTED | LI_PYROWAVE_METADATA_FLAG_REQUIRED);
      EXPECT_TRUE(std::equal(metadata->payload.begin(), metadata->payload.end(), recovered.begin() + 8));
      LiPyrowaveReassemblyDestroy(&state);
    }
  }
}

TEST(PyrowaveCapabilityTest, RejectsAnUpstreamRuntimeInTheForkContract) {
  auto config = enabled_config();
  auto device = supported_device();
  auto client = supported_client();
  device.api_version = { 1, 0, 0 };
  auto result = pyrowave::evaluate_capabilities(config, device, client);
  EXPECT_EQ(result.failure, pyrowave::failure_e::api_mismatch);
  device = supported_device();
  client.api_version = { 1, 0, 0 };
  result = pyrowave::evaluate_capabilities(config, device, client);
  EXPECT_EQ(result.failure, pyrowave::failure_e::api_mismatch);
}

TEST(PyrowaveTransportTest, PublishesSelectedHdrAndHlgReferenceWithTheSameProtectedFrame) {
  const auto from_hex = [](std::string_view hex) {
    std::vector<std::uint8_t> bytes;
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
      bytes.push_back(static_cast<std::uint8_t>((digit(hex[i]) << 4) | digit(hex[i + 1])));
    }
    return bytes;
  };
  for (const auto &fixture : PyrowaveDynamicHdrFixtures) {
    auto mail = std::make_shared<safe::mail_raw_t>();
    auto packets = mail->queue<video::packet_t>("pyrowave-test-hdr");
    const auto source = std::make_shared<const std::vector<std::uint8_t>>(6000, 0x63);
    const pyrowave::hdr_frame_metadata_t metadata {
      .type = fixture.type, .hlg_nominal_peak_nits = fixture.hlgNominalPeakNits,
      .payload = from_hex(fixture.payloadHex),
    };
    const auto published = platf::pyrowave_windows::publish_transport_frame(
      fixture.format, source, 128, 10, packets, nullptr, std::chrono::steady_clock::now(), std::nullopt, &metadata);
    ASSERT_TRUE(published.success);
    auto output = packets->pop();
    ASSERT_NE(output, nullptr);
    LI_PYROWAVE_REASSEMBLY_STATE state;
    LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
    // Reverse arrival, no FRAME_HEADER, and a missing first DATA block. The
    // dynamic TLV must still be recovered from DATA/PARITY, not its header copy.
    for (std::size_t i = published.blocks; i-- > 0;) {
      LI_PYROWAVE_PACKET_HEADER header {};
      const std::uint8_t *payload = nullptr;
      const auto *packet = output->data() + i * 192;
      ASSERT_EQ(LiPyrowaveParsePacket(packet, 192, &header, &payload), LI_PYROWAVE_PACKET_OK);
      if (header.packetKind == LI_PYROWAVE_PACKET_FRAME_HEADER ||
          (header.packetKind == LI_PYROWAVE_PACKET_DATA && header.blockIndex == 0)) continue;
      LiPyrowaveReassemblyPushPacket(&state, packet, 192, 5000, 100000);
    }
    ASSERT_TRUE(LiPyrowaveReassemblyIsComplete(&state));
    const auto expected_length = 10 + 8 + metadata.payload.size() + (metadata.hlg_nominal_peak_nits != 0 ? 10 : 0);
    std::vector<std::uint8_t> recovered(expected_length);
    std::size_t written = 0;
    std::uint16_t flags = 0;
    ASSERT_EQ(LiPyrowaveReassemblyCopyMetadata(&state, recovered.data(), recovered.size(), &written, &flags),
              LI_PYROWAVE_REASSEMBLY_COMPLETE);
    EXPECT_EQ(written, expected_length);
    EXPECT_EQ(flags, LI_PYROWAVE_METADATA_FLAG_PROTECTED | LI_PYROWAVE_METADATA_FLAG_RUNTIME);
    EXPECT_EQ(recovered[11], metadata.type);
    EXPECT_EQ(recovered[13], LI_PYROWAVE_METADATA_FLAG_PROTECTED | LI_PYROWAVE_METADATA_FLAG_REQUIRED);
    EXPECT_TRUE(std::equal(metadata.payload.begin(), metadata.payload.end(), recovered.begin() + 18));
    if (metadata.hlg_nominal_peak_nits != 0) {
      EXPECT_EQ(recovered[recovered.size() - 1], 0xe8);
      EXPECT_EQ(recovered[recovered.size() - 2], 0x03);
    }
    std::vector<std::uint8_t> decoded(source->size());
    ASSERT_EQ(LiPyrowaveReassemblyCopyFrame(&state, decoded.data(), decoded.size(), &written, nullptr),
              LI_PYROWAVE_REASSEMBLY_COMPLETE);
    EXPECT_EQ(decoded, *source);
    LiPyrowaveReassemblyDestroy(&state);
  }
}

TEST(PyrowaveTransportTest, RejectsHdrAreaOverflowBeforeQueueing) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto packets = mail->queue<video::packet_t>("pyrowave-test-hdr-overflow");
  const pyrowave::hdr_frame_metadata_t metadata {
    .type = LI_PYROWAVE_METADATA_HDR_VIVID, .hlg_nominal_peak_nits = 1000,
    .payload = std::vector<std::uint8_t>(LI_PYROWAVE_MAX_METADATA_SIZE - 8u, 0),
  };
  const auto result = platf::pyrowave_windows::publish_transport_frame(
    1, std::make_shared<const std::vector<std::uint8_t>>(100, 1), 1312, 10, packets,
    nullptr, std::nullopt, std::nullopt, &metadata);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(packets->peek());
}

TEST(PyrowaveTransportTest, PublishesFrameWithProtectedRuntimeMetadata) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto packets = mail->queue<video::packet_t>("pyrowave-test-video");
  auto source = std::make_shared<const std::vector<std::uint8_t>>(323933, 0x5a);
  const auto timestamp = std::chrono::steady_clock::now() - std::chrono::milliseconds(12);
  const auto result = platf::pyrowave_windows::publish_transport_frame(
    1, source, 1312, 0, packets, nullptr, timestamp, std::nullopt);

  ASSERT_TRUE(result.success);
  ASSERT_TRUE(packets->peek());
  auto packet = packets->pop(std::chrono::milliseconds(0));
  ASSERT_TRUE(packet);
  const auto &output = packet;
  ASSERT_EQ(output->data_size(), result.bytes);
  ASSERT_GT(result.blocks, 255u);
  LI_PYROWAVE_PACKET_HEADER header {};
  const std::uint8_t *payload = nullptr;
  ASSERT_EQ(LiPyrowaveParsePacket(output->data(), 1376, &header, &payload), LI_PYROWAVE_PACKET_OK);
  EXPECT_EQ(header.metadataLength, 10u);
  EXPECT_EQ(header.metadataFlags, LI_PYROWAVE_METADATA_FLAG_PROTECTED |
                                 LI_PYROWAVE_METADATA_FLAG_RUNTIME |
                                 LI_PYROWAVE_METADATA_FLAG_OPTIONAL);

  LI_PYROWAVE_REASSEMBLY_STATE state;
  LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
  for (std::size_t offset = 0; offset < output->data_size(); offset += 1376) {
    const auto status = LiPyrowaveReassemblyPushPacket(&state,
      output->data() + offset, 1376, 1000, 100000);
    EXPECT_TRUE(status == LI_PYROWAVE_REASSEMBLY_ACCEPTED ||
                status == LI_PYROWAVE_REASSEMBLY_COMPLETE);
  }
  ASSERT_TRUE(LiPyrowaveReassemblyIsComplete(&state));
  std::vector<std::uint8_t> metadata(10);
  std::size_t metadata_length = 0;
  std::uint16_t metadata_flags = 0;
  EXPECT_EQ(LiPyrowaveReassemblyCopyMetadata(&state, metadata.data(), metadata.size(),
              &metadata_length, &metadata_flags), LI_PYROWAVE_REASSEMBLY_COMPLETE);
  EXPECT_EQ(metadata_length, 10u);
  EXPECT_EQ((static_cast<std::uint16_t>(metadata[0]) << 8) | metadata[1],
            LI_PYROWAVE_METADATA_HOST_PROCESSING_LATENCY);
  EXPECT_EQ((static_cast<std::uint16_t>(metadata[2]) << 8) | metadata[3], metadata_flags);
  std::vector<std::uint8_t> decoded(source->size());
  std::size_t decoded_length = 0;
  EXPECT_EQ(LiPyrowaveReassemblyCopyFrame(&state, decoded.data(), decoded.size(),
              &decoded_length, nullptr), LI_PYROWAVE_REASSEMBLY_COMPLETE);
  EXPECT_EQ(decoded_length, source->size());
  EXPECT_EQ(decoded, *source);
  LiPyrowaveReassemblyDestroy(&state);
}

TEST(PyrowaveTransportTest, PublishesReplayWithoutMetadataFlags) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto packets = mail->queue<video::packet_t>("pyrowave-test-video");
  auto source = std::make_shared<const std::vector<std::uint8_t>>(9000, 0x3c);
  const auto result = platf::pyrowave_windows::publish_transport_frame(
    2, source, 1312, 1500, packets, nullptr, std::nullopt, std::nullopt);

  ASSERT_TRUE(result.success);
  auto packet = packets->pop(std::chrono::milliseconds(0));
  ASSERT_TRUE(packet);
  LI_PYROWAVE_PACKET_HEADER header {};
  const std::uint8_t *payload = nullptr;
  ASSERT_EQ(LiPyrowaveParsePacket(packet->data(), 1376, &header, &payload), LI_PYROWAVE_PACKET_OK);
  EXPECT_EQ(header.metadataLength, 0u);
  EXPECT_EQ(header.metadataFlags, 0u);
  EXPECT_EQ(header.flags & LI_PYROWAVE_FLAG_METADATA_PRESENT, 0u);
}

TEST(PyrowaveTransportTest, DoesNotQueueFailedPacketization) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto packets = mail->queue<video::packet_t>("pyrowave-test-video");
  auto source = std::make_shared<const std::vector<std::uint8_t>>(9000, 0x4d);
  // This positive host frame ID cannot be represented by the wire header and
  // makes the real packetizer return a valid, payload-free failure result.
  const auto frame_id = static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
  const auto result = platf::pyrowave_windows::publish_transport_frame(
    frame_id, source, 1312, 0, packets, nullptr, std::nullopt, std::nullopt);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.result_code, 0);
  EXPECT_EQ(result.bytes, 0u);
  EXPECT_EQ(result.blocks, 0u);
  EXPECT_FALSE(packets->peek());
}

TEST(PyrowaveRateControlTest, KeepsLowBitrateBudgetBelowLegacyFloor) {
  using platf::pyrowave_windows::pyrowave_frame_budget;

  EXPECT_EQ(pyrowave_frame_budget(10'000, 120, 1), 10'416u);
  EXPECT_EQ(pyrowave_frame_budget(100'000, 120, 1), 104'166u);
  EXPECT_EQ(pyrowave_frame_budget(1'000, 120, 1), 1'041u);
  EXPECT_EQ(pyrowave_frame_budget(100, 120, 1), 104u);
}

TEST(PyrowaveRateControlTest, UsesFixedNegotiatedFrameRate) {
  using platf::pyrowave_windows::pyrowave_frame_budget;

  EXPECT_EQ(pyrowave_frame_budget(20'000, 120, 1), 20'833u);
  EXPECT_EQ(pyrowave_frame_budget(20'000, 60, 1), 41'666u);
  EXPECT_EQ(pyrowave_frame_budget(600'000, 60, 1), 1u * 1024u * 1024u);
}

TEST(PyrowaveRateControlTest, DoesNotDoubleBitrateAt120Fps) {
  constexpr int bitrate_kbps = 104'300;
  constexpr int frame_rate = 120;
  const auto budget = platf::pyrowave_windows::pyrowave_frame_budget(bitrate_kbps, frame_rate, 1);

  EXPECT_EQ(budget, 108'645u);
  EXPECT_LE(static_cast<std::uint64_t>(budget) * 8u * frame_rate,
            static_cast<std::uint64_t>(bitrate_kbps) * 1000u);
}

TEST(PyrowaveRateControlTest, PreservesFractionalNegotiatedFrameRate) {
  using platf::pyrowave_windows::pyrowave_frame_budget;

  EXPECT_EQ(pyrowave_frame_budget(60'000, 60'000, 1001), 125'125u);
  EXPECT_EQ(pyrowave_frame_budget(60'000, 120'000, 1001), 62'562u);
}

TEST(PyrowaveColorMetadataTest, PreservesLimitedAndFullRangeForAllSupportedModes) {
  for (const int dynamic_range : { 0, 1, 2 }) {
    const auto limited = platf::pyrowave_windows::color_metadata_for(dynamic_range, false);
    const auto full = platf::pyrowave_windows::color_metadata_for(dynamic_range, true);

    EXPECT_EQ(limited.range, PYROWAVE_YCBCR_LIMITED);
    EXPECT_EQ(full.range, PYROWAVE_YCBCR_FULL);
    EXPECT_EQ(limited.primaries, dynamic_range == 0
      ? PYROWAVE_COLOR_PRIMARIES_BT709 : PYROWAVE_COLOR_PRIMARIES_BT2020);
    EXPECT_EQ(limited.transfer, dynamic_range == 0
      ? PYROWAVE_TRANSFER_BT709
      : dynamic_range == 1 ? PYROWAVE_TRANSFER_PQ : PYROWAVE_TRANSFER_HLG);
  }
}

TEST(PyrowaveColorMetadataTest, UsesResolvedConversionColorSpace) {
  using video::colorspace_e;
  using platf::pyrowave_windows::color_metadata_for;
  const auto sdr = color_metadata_for({ colorspace_e::rec709, false, 8 });
  ASSERT_TRUE(sdr);
  EXPECT_EQ(sdr->transfer, PYROWAVE_TRANSFER_BT709);
  EXPECT_EQ(sdr->range, PYROWAVE_YCBCR_LIMITED);
  const auto pq = color_metadata_for({ colorspace_e::bt2020, true, 10 });
  ASSERT_TRUE(pq);
  EXPECT_EQ(pq->transfer, PYROWAVE_TRANSFER_PQ);
  EXPECT_EQ(pq->range, PYROWAVE_YCBCR_FULL);
  const auto hlg = color_metadata_for({ colorspace_e::bt2020hlg, false, 10 });
  ASSERT_TRUE(hlg);
  EXPECT_EQ(hlg->transfer, PYROWAVE_TRANSFER_HLG);
  EXPECT_FALSE(color_metadata_for({ colorspace_e::bt2020sdr, true, 10 }));
  EXPECT_FALSE(color_metadata_for({ colorspace_e::rec709, true, 10 }));
  EXPECT_FALSE(color_metadata_for({ colorspace_e::bt2020, true, 8 }));
}
#endif

TEST(PyrowavePacketizerTest, HonorsSmallerNegotiatedWireLimitWithoutSplittingInnerPackets) {
  const auto rtp_size = pyrowave::limit_rtp_packet_size(1392, 1024);
  ASSERT_EQ(rtp_size, 1040);
  const auto outer_payload_size = static_cast<std::size_t>(rtp_size - 16);
  pyrowave::encoded_frame_t frame {
    .frame_id = 1,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { std::make_shared<const std::vector<std::uint8_t>>(9000, 0x5a) },
  };
  const auto result = pyrowave::make_transport_packetizer()->packetize(frame, {
    .packet_boundary = outer_payload_size,
    .block_aware_fec = true,
  });
  ASSERT_EQ(result.failure, pyrowave::failure_e::none);
  ASSERT_FALSE(result.packets.empty());
  for (const auto &packet : result.packets) {
    EXPECT_EQ(packet.size, outer_payload_size);
    EXPECT_EQ(packet.offset % outer_payload_size, 0u);
    LI_PYROWAVE_PACKET_HEADER header {};
    const std::uint8_t *payload = nullptr;
    EXPECT_EQ(LiPyrowaveParsePacket(result.bitstream.view().data() + packet.offset,
      packet.size, &header, &payload), LI_PYROWAVE_PACKET_OK);
  }
}

TEST(PyrowavePacketizerTest, RecoversOneMissingBlockWithBlockAwareFec) {
  auto source = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>(5000, 0x5a));
  pyrowave::encoded_frame_t frame {
    .frame_id = 8,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  const auto result = packetizer->packetize(frame, {
    .packet_boundary = 1240,
    .rtp_timestamp = 5678,
    .block_aware_fec = true,
  });

  ASSERT_EQ(result.failure, pyrowave::failure_e::none);
  ASSERT_GT(result.packets.size(), 2u);

  LI_PYROWAVE_REASSEMBLY_STATE state;
  LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
  for (std::size_t index = 0; index < result.packets.size(); ++index) {
    LI_PYROWAVE_PACKET_HEADER header {};
    const std::uint8_t *payload = nullptr;
    const auto span = result.packets[index];
    ASSERT_EQ(LiPyrowaveParsePacket(result.bitstream.view().data() + span.offset,
                                    span.size, &header, &payload), LI_PYROWAVE_PACKET_OK);
    if (index == 1) continue;
    const auto status = LiPyrowaveReassemblyPushPacket(
      &state, result.bitstream.view().data() + span.offset, span.size, 1000 + index, 100000);
     EXPECT_TRUE(status == LI_PYROWAVE_REASSEMBLY_ACCEPTED ||
                 status == LI_PYROWAVE_REASSEMBLY_COMPLETE);
  }
  EXPECT_TRUE(LiPyrowaveReassemblyIsComplete(&state));
  std::vector<std::uint8_t> output(source->size());
  std::size_t output_length = 0;
  EXPECT_EQ(LiPyrowaveReassemblyCopyFrame(&state, output.data(), output.size(),
                                          &output_length, nullptr),
            LI_PYROWAVE_REASSEMBLY_COMPLETE);
  EXPECT_EQ(output_length, source->size());
  EXPECT_EQ(output, *source);
  LiPyrowaveReassemblyDestroy(&state);
}

TEST(PyrowavePacketizerTest, RecoversMissingBlocksAcrossFecGroupsAndKeepsTail) {
  constexpr std::size_t payload_boundary = 1200;
  constexpr std::size_t data_block_count = LI_PYROWAVE_FEC_DATA_PER_GROUP * 2 + 3;
  const auto source_size = payload_boundary * data_block_count - 17;
  auto source = std::make_shared<std::vector<std::uint8_t>>(source_size);
  for (std::size_t index = 0; index < source->size(); ++index) {
    (*source)[index] = static_cast<std::uint8_t>((index * 37 + 11) & 0xff);
  }
  std::shared_ptr<const std::vector<std::uint8_t>> source_view = source;

  pyrowave::encoded_frame_t frame {
    .frame_id = 9,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source_view },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  const auto result = packetizer->packetize(frame, {
    .packet_boundary = payload_boundary + LI_PYROWAVE_WIRE_FEC_HEADER_SIZE,
    .rtp_timestamp = 6789,
    .block_aware_fec = true,
  });

  ASSERT_EQ(result.failure, pyrowave::failure_e::none);
   ASSERT_EQ(result.packets.size(), data_block_count + 3u + 1u);

  LI_PYROWAVE_REASSEMBLY_STATE state;
  LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
  for (std::size_t index = result.packets.size(); index-- > 0;) {
    LI_PYROWAVE_PACKET_HEADER header {};
    const std::uint8_t *payload = nullptr;
    const auto span = result.packets[index];
    const auto *packet = result.bitstream.view().data() + span.offset;
    ASSERT_EQ(LiPyrowaveParsePacket(packet, span.size, &header, &payload), LI_PYROWAVE_PACKET_OK);
    if ((header.flags & LI_PYROWAVE_FLAG_FEC_PARITY) == 0 &&
        (header.blockIndex == 1 || header.blockIndex == 34)) {
      continue;
    }

    const auto status = LiPyrowaveReassemblyPushPacket(
      &state, packet, span.size, 2000 + index, 100000);
    EXPECT_TRUE(status == LI_PYROWAVE_REASSEMBLY_ACCEPTED ||
                status == LI_PYROWAVE_REASSEMBLY_COMPLETE ||
                status == LI_PYROWAVE_REASSEMBLY_DUPLICATE)
      << "status=" << static_cast<int>(status) << ", packet_index=" << index;
  }

  ASSERT_TRUE(LiPyrowaveReassemblyIsComplete(&state));
  std::vector<std::uint8_t> output(source_size);
  std::size_t output_length = 0;
  EXPECT_EQ(LiPyrowaveReassemblyCopyFrame(&state, output.data(), output.size(),
                                          &output_length, nullptr),
            LI_PYROWAVE_REASSEMBLY_COMPLETE);
  EXPECT_EQ(output_length, source_size);
  EXPECT_EQ(output, *source);
  LiPyrowaveReassemblyDestroy(&state);
}

TEST(PyrowavePacketizerTest, DoesNotRecoverTwoMissingBlocksFromOneParityShard) {
  auto source = std::make_shared<const std::vector<std::uint8_t>>(
    std::vector<std::uint8_t>(5000, 0x3c));
  pyrowave::encoded_frame_t frame {
    .frame_id = 10,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  const auto result = packetizer->packetize(frame, {
    .packet_boundary = 1240,
    .rtp_timestamp = 6790,
    .block_aware_fec = true,
  });

  ASSERT_EQ(result.failure, pyrowave::failure_e::none);
  LI_PYROWAVE_REASSEMBLY_STATE state;
  LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
  for (std::size_t index = 0; index < result.packets.size(); ++index) {
    LI_PYROWAVE_PACKET_HEADER header {};
    const std::uint8_t *payload = nullptr;
    const auto span = result.packets[index];
    const auto *packet = result.bitstream.view().data() + span.offset;
    ASSERT_EQ(LiPyrowaveParsePacket(packet, span.size, &header, &payload), LI_PYROWAVE_PACKET_OK);
    if ((header.flags & LI_PYROWAVE_FLAG_FEC_PARITY) == 0 &&
        (header.blockIndex == 1 || header.blockIndex == 2)) {
      continue;
    }
    LiPyrowaveReassemblyPushPacket(&state, packet, span.size, 3000 + index, 100000);
  }

  EXPECT_FALSE(LiPyrowaveReassemblyIsComplete(&state));
  LiPyrowaveReassemblyDestroy(&state);
}

TEST(PyrowavePacketizerTest, AcceptsReorderedPacketsAndRejectsDuplicates) {
  auto source = std::make_shared<const std::vector<std::uint8_t>>(
    std::vector<std::uint8_t>(5000, 0x4d));
  pyrowave::encoded_frame_t frame {
    .frame_id = 11,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  const auto result = packetizer->packetize(frame, {
    .packet_boundary = 1240,
    .rtp_timestamp = 6791,
    .block_aware_fec = true,
  });

  ASSERT_EQ(result.failure, pyrowave::failure_e::none);
   ASSERT_EQ(result.packets.size(), 7u);
  LI_PYROWAVE_REASSEMBLY_STATE state;
  LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);

   const std::size_t order[] = { 2, 2, 4, 0, 5, 1, 3, 6 };
  for (std::size_t position = 0; position < std::size(order); ++position) {
    const auto span = result.packets[order[position]];
    const auto *packet = result.bitstream.view().data() + span.offset;
    const auto status = LiPyrowaveReassemblyPushPacket(
      &state, packet, span.size, 4000 + position, 100000);
    if (position == 1) {
      EXPECT_EQ(status, LI_PYROWAVE_REASSEMBLY_DUPLICATE);
    }
    else {
      EXPECT_TRUE(status == LI_PYROWAVE_REASSEMBLY_ACCEPTED ||
                  status == LI_PYROWAVE_REASSEMBLY_COMPLETE ||
                  status == LI_PYROWAVE_REASSEMBLY_DUPLICATE)
        << "status=" << static_cast<int>(status) << ", packet_index=" << order[position];
    }
  }

  EXPECT_TRUE(LiPyrowaveReassemblyIsComplete(&state));
  LiPyrowaveReassemblyDestroy(&state);
}

TEST(PyrowavePacketizerTest, RejectsBoundariesThatCannotCarryWireHeaders) {
  auto source = std::make_shared<const std::vector<std::uint8_t>>(
    std::vector<std::uint8_t>(16, 0x55));
  pyrowave::encoded_frame_t frame {
    .frame_id = 12,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source },
  };
  auto packetizer = pyrowave::make_transport_packetizer();

  const auto result = packetizer->packetize(frame, {
    .packet_boundary = LI_PYROWAVE_WIRE_FEC_HEADER_SIZE,
    .block_aware_fec = true,
  });
  EXPECT_EQ(result.failure, pyrowave::failure_e::configuration_invalid);
  EXPECT_TRUE(result.packets.empty());
  EXPECT_TRUE(result.bitstream.empty());
}

TEST(PyrowavePacketTest, FailedPacketizationDoesNotExposePayload) {
  pyrowave::packetization_result_t result;
  result.failure = pyrowave::failure_e::packetization_failed;
  result.bitstream.storage = std::make_shared<const std::vector<std::uint8_t>>(
    std::initializer_list<std::uint8_t> { 1 });

  EXPECT_FALSE(pyrowave::validate(result));
  result.bitstream.storage.reset();
  EXPECT_TRUE(pyrowave::validate(result));
}

TEST(PyrowaveRuntimeTest, ExposesFormatOnlyOnSupportedServerPlatform) {
  EXPECT_TRUE(pyrowave::is_experimental_video_format(LI_PYROWAVE_VIDEO_FORMAT));
#ifdef _WIN32
  EXPECT_TRUE(pyrowave::is_server_video_format_available(LI_PYROWAVE_VIDEO_FORMAT));
  EXPECT_FALSE(pyrowave::is_server_video_format_available(0));
#else
  EXPECT_FALSE(pyrowave::is_server_video_format_available(LI_PYROWAVE_VIDEO_FORMAT));
  EXPECT_TRUE(pyrowave::is_server_video_format_available(0));
#endif
}

TEST(PyrowavePacketTest, RejectsNegativeReassemblyDeadline) {
  pyrowave::reassembly_policy_t policy;
  policy.deadline = std::chrono::milliseconds { -1 };

  EXPECT_FALSE(pyrowave::validate(policy));
}

TEST(PyrowaveGpuInteropTest, RequiresAnImageAndValidDimensions) {
  pyrowave::gpu_frame_t frame;
  frame.width = 1920;
  frame.height = 1080;
  frame.format = pyrowave::image_format_e::nv12;
  EXPECT_FALSE(pyrowave::validate(frame));

  frame.opaque_image = &frame;
  EXPECT_TRUE(pyrowave::validate(frame));
  frame.width = 1919;
  EXPECT_FALSE(pyrowave::validate(frame));
}

TEST(PyrowaveSessionTest, ValidatesLifecycle) {
  pyrowave::session_t session { 7 };
  EXPECT_EQ(session.start(), pyrowave::failure_e::invalid_state);
  EXPECT_EQ(session.snapshot().state, pyrowave::session_state_e::failed);

  EXPECT_EQ(session.negotiate(enabled_config(), supported_device(), supported_client()), pyrowave::failure_e::none);
  EXPECT_EQ(session.snapshot().state, pyrowave::session_state_e::prepared);
  EXPECT_EQ(session.start(), pyrowave::failure_e::none);
  EXPECT_EQ(session.snapshot().state, pyrowave::session_state_e::streaming);

  session.stop();
  EXPECT_EQ(session.snapshot().state, pyrowave::session_state_e::idle);
  session.stop();
  EXPECT_EQ(session.snapshot().state, pyrowave::session_state_e::idle);
}

TEST(PyrowaveSessionTest, FailedSessionCanNegotiateAgain) {
  pyrowave::session_t session { 8 };
  EXPECT_EQ(session.start(), pyrowave::failure_e::invalid_state);
  EXPECT_EQ(session.snapshot().state, pyrowave::session_state_e::failed);

  EXPECT_EQ(session.negotiate(enabled_config(), supported_device(), supported_client()), pyrowave::failure_e::none);
  EXPECT_EQ(session.snapshot().state, pyrowave::session_state_e::prepared);
}

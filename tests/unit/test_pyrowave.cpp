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
    device.api_version = { 0, 6, 1 };
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
    client.api_version = { 0, 6, 1 };
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

}  // namespace

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

TEST(PyrowavePacketizerTest, RejectsMetadataLargerThanFrameHeaderPayload) {
  const pyrowave::encoded_frame_t frame {
    .frame_id = 14,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { std::make_shared<const std::vector<std::uint8_t>>(4, 0x5a) },
  };
  auto packetizer = pyrowave::make_transport_packetizer();
  for (const std::size_t boundary : { 128u, 0u, LI_PYROWAVE_MAX_PACKET_SIZE }) {
    const auto effective_boundary = boundary == 0 ? LI_PYROWAVE_MAX_PACKET_SIZE : boundary;
    for (const bool fec : { false, true }) {
      SCOPED_TRACE(::testing::Message() << "boundary=" << boundary << ", fec=" << fec);
      const pyrowave::packetization_request_t request {
        .packet_boundary = boundary,
        .block_aware_fec = fec,
        .metadata = optional_metadata(effective_boundary - LI_PYROWAVE_WIRE_HEADER_SIZE + 1),
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

TEST(PyrowavePacketizerTest, RoundTripsMetadataThatFillsFrameHeaderPayload) {
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
        .metadata = optional_metadata(effective_boundary - LI_PYROWAVE_WIRE_HEADER_SIZE),
        .metadata_flags = LI_PYROWAVE_METADATA_FLAG_PROTECTED | LI_PYROWAVE_METADATA_FLAG_OPTIONAL,
      };
      ASSERT_TRUE(pyrowave::validate(request));
      const auto result = packetizer->packetize(frame, request);
      ASSERT_EQ(result.failure, pyrowave::failure_e::none);
      LI_PYROWAVE_REASSEMBLY_STATE state;
      LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
      for (const auto &packet : result.packets) {
        EXPECT_LE(packet.size, effective_boundary);
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

#ifdef _WIN32
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

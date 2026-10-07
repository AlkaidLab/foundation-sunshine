/**
 * @file tests/tools/pyrowave_packetizer_probe.cpp
 * @brief Reproduce the server-side PyroWave transport packetization contract.
 *
 * This tool deliberately stops before GPU capture and encoding. It isolates
 * the frame/request validation and outer transport packetization that follows
 * a PyroWave encoder output frame.
 */
#include "src/pyrowave/packetizer.h"

extern "C" {
#include "third-party/moonlight-common-c/src/PyrowaveProtocol.h"
}

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace {

  constexpr std::size_t outer_packet_overhead = 16u;

  struct options_t {
    std::size_t packet_size = 1392u;
    std::size_t bitstream_bytes = 9000u;
    std::uint64_t frame_id = 1u;
    std::uint32_t rtp_timestamp = 0u;
    std::optional<std::size_t> inner_payload_boundary;
    bool block_aware_fec = true;
  };

  template<typename value_t>
  bool
  parse_unsigned(std::string_view text, value_t &value) {
    unsigned long long parsed = 0;
    const auto begin = text.data();
    const auto end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc {} || result.ptr != end ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<value_t>::max())) {
      return false;
    }

    value = static_cast<value_t>(parsed);
    return true;
  }

  bool
  read_value(int &index, int argc, char **argv, std::string_view option, std::string_view &value) {
    if (index + 1 >= argc || option != argv[index]) {
      return false;
    }

    value = argv[++index];
    return true;
  }

  void
  print_usage(const char *program) {
    std::cout
      << "Usage: " << program << " [options]\n"
      << "  --packet-size N           Moonlight packetSize (default: 1392)\n"
      << "  --bitstream-bytes N       Synthetic encoded frame size (default: 9000)\n"
      << "  --frame-id N              Frame id (default: 1)\n"
      << "  --rtp-timestamp N         RTP timestamp (default: 0)\n"
      << "  --inner-boundary N        Override derived inner payload boundary\n"
      << "  --no-fec                  Use the non-FEC wire header\n"
      << "  --help                    Show this help\n";
  }

}  // namespace

int
main(int argc, char **argv) {
  options_t options;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--help") {
      print_usage(argv[0]);
      return 0;
    }
    if (argument == "--no-fec") {
      options.block_aware_fec = false;
      continue;
    }

    std::string_view value;
    if (read_value(index, argc, argv, "--packet-size", value)) {
      if (!parse_unsigned(value, options.packet_size)) {
        std::cerr << "Invalid --packet-size: " << value << '\n';
        return 2;
      }
    }
    else if (read_value(index, argc, argv, "--bitstream-bytes", value)) {
      if (!parse_unsigned(value, options.bitstream_bytes)) {
        std::cerr << "Invalid --bitstream-bytes: " << value << '\n';
        return 2;
      }
    }
    else if (read_value(index, argc, argv, "--frame-id", value)) {
      if (!parse_unsigned(value, options.frame_id)) {
        std::cerr << "Invalid --frame-id: " << value << '\n';
        return 2;
      }
    }
    else if (read_value(index, argc, argv, "--rtp-timestamp", value)) {
      if (!parse_unsigned(value, options.rtp_timestamp)) {
        std::cerr << "Invalid --rtp-timestamp: " << value << '\n';
        return 2;
      }
    }
    else if (read_value(index, argc, argv, "--inner-boundary", value)) {
      std::size_t boundary = 0;
      if (!parse_unsigned(value, boundary)) {
        std::cerr << "Invalid --inner-boundary: " << value << '\n';
        return 2;
      }
      options.inner_payload_boundary = boundary;
    }
    else {
      std::cerr << "Unknown option: " << argument << '\n';
      print_usage(argv[0]);
      return 2;
    }
  }

  const auto header_size = options.block_aware_fec
    ? LI_PYROWAVE_WIRE_FEC_HEADER_SIZE
    : LI_PYROWAVE_WIRE_HEADER_SIZE;
  const auto outer_payload_capacity = options.packet_size > outer_packet_overhead
    ? options.packet_size - outer_packet_overhead
    : 0u;
  const auto derived_inner_boundary = outer_payload_capacity > header_size
    ? outer_payload_capacity - header_size
    : 0u;
  const auto inner_boundary = options.inner_payload_boundary.value_or(derived_inner_boundary);
  const auto wire_boundary = inner_boundary > std::numeric_limits<std::size_t>::max() - header_size
    ? 0u
    : inner_boundary + header_size;

  auto source = std::make_shared<const std::vector<std::uint8_t>>(
    options.bitstream_bytes, static_cast<std::uint8_t>(0x5a));
  pyrowave::encoded_frame_t frame {
    .frame_id = options.frame_id,
    .deadline = { std::chrono::steady_clock::now(), std::chrono::milliseconds(100) },
    .bitstream = { source },
  };
  const pyrowave::packetization_request_t request {
    .packet_boundary = wire_boundary,
    .rtp_timestamp = options.rtp_timestamp,
    .block_aware_fec = options.block_aware_fec,
  };

  const auto frame_valid = pyrowave::validate(frame);
  const auto request_valid = pyrowave::validate(request);
  std::cout << "[PyroWavePacketizerProbe] frame_id=" << frame.frame_id
            << ", input_bytes=" << frame.bitstream.view().size()
            << ", packet_size=" << options.packet_size
            << ", outer_payload_capacity=" << outer_payload_capacity
            << ", inner_payload_boundary=" << inner_boundary
            << ", wire_packet_boundary=" << wire_boundary
            << ", header_size=" << header_size
            << ", block_aware_fec=" << options.block_aware_fec
            << ", frame_valid=" << frame_valid
            << ", request_valid=" << request_valid << '\n';

  auto packetizer = pyrowave::make_transport_packetizer();
  const auto result = packetizer->packetize(frame, request);
  const auto result_valid = pyrowave::validate(result);
  std::cout << "[PyroWavePacketizerProbe] failure=" << static_cast<int>(result.failure)
            << ", output_bytes=" << (result.bitstream.storage ? result.bitstream.storage->size() : 0u)
            << ", blocks=" << result.packets.size()
            << ", result_valid=" << result_valid << '\n';

  if (!result.packets.empty()) {
    LI_PYROWAVE_PACKET_HEADER header {};
    const std::uint8_t *payload = nullptr;
    const auto first = result.packets.front();
    const auto parse_result = LiPyrowaveParsePacket(
      result.bitstream.view().data() + first.offset,
      first.size,
      &header,
      &payload);
    std::cout << "[PyroWavePacketizerProbe] first_packet_bytes=" << first.size
              << ", parse_result=" << static_cast<int>(parse_result)
              << ", frame_id=" << header.frameId
              << ", block=" << header.blockIndex << '/' << header.blockCount
              << ", payload_bytes=" << header.payloadLength << '\n';
  }

  return result_valid ? 0 : 1;
}

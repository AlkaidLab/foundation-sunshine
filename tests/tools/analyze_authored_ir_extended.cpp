// SPDX-License-Identifier: Apache-2.0
#include "src/haptics/authored_ir_extended.h"
#include <array>
#include <fstream>
#include <iostream>
int
main(int argc, char **argv) {
  if (argc != 3) return 2;
  std::ifstream input(argv[1], std::ios::binary);
  std::ofstream output(argv[2], std::ios::binary);
  haptics::authored_ir_extended_session_t session;
  if (!input || !output || !session.ready()) return 3;
  std::array<std::uint8_t, 960> pcm {};
  std::uint32_t sequence = 0;
  std::uint64_t position = 0;
  while (input.read(reinterpret_cast<char *>(pcm.data()), pcm.size()) || input.gcount()) {
    const auto bytes = input.gcount();
    if (bytes % 4) return 4;
    const auto frames = static_cast<std::uint16_t>(bytes / 4);
    auto packet = session.process(0, position == 0 ? 1 : 0, frames, sequence++, position * 1000000 / 48000,
      std::span<const std::uint8_t>(pcm.data(), static_cast<std::size_t>(bytes)));
    position += frames;
    if (packet) output.write(reinterpret_cast<const char *>(packet->data()), packet->size());
  }
  if (input.bad()) return 5;
  auto packet = session.process(0, 2, 0, sequence, position * 1000000 / 48000, {});
  if (!packet) return 6;
  output.write(reinterpret_cast<const char *>(packet->data()), packet->size());
  output.flush();
  return output ? 0 : 7;
}

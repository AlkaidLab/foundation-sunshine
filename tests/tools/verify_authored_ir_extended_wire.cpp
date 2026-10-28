// Validate production analyzer output with the production common-c parser.
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <vector>
extern "C" {
#include "Ds5HapticsIrStream.h"
}

static void
capture(const LI_DS5_HAPTICS_IR_EXTENDED_FRAME *frame, void *context) {
  *static_cast<LI_DS5_HAPTICS_IR_EXTENDED_FRAME *>(context) = *frame;
}

int
main(int argc, char **argv) {
  if (argc != 2) return 2;
  unsigned streams = 0, packets = 0, unique_events = 0;
  for (const auto &entry : std::filesystem::directory_iterator(argv[1])) {
    if (entry.path().extension() != ".irextbin") continue;
    std::ifstream input(entry.path(), std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    std::map<std::pair<std::uint32_t, std::uint32_t>, LI_DS5_HAPTICS_IR_EVENT> seen;
    std::size_t at = 0;
    std::uint64_t previous_time = 0;
    std::uint32_t previous_sequence = 0;
    bool first = true, ended = false;
    while (at < bytes.size()) {
      if (bytes.size() - at < 112) return 3;
      const auto length = 72 + unsigned(bytes[at + 74]) + (unsigned(bytes[at + 75]) << 8);
      if (length > bytes.size() - at) return 4;
      LI_DS5_HAPTICS_IR_EXTENDED_FRAME frame {};
      if (!processDs5HapticsIrExtendedStreamPacket(bytes.data() + at, static_cast<int>(length), capture, &frame)) {
        std::cerr << entry.path() << ": invalid packet at " << at << '\n';
        return 5;
      }
      if (ended || (!first && (frame.windowEndUs < previous_time || frame.base.sourceSequenceNumber != previous_sequence + 1))) return 6;
      if (first && !(frame.base.flags & LI_DS5_HAPTICS_IR_FLAG_DISCONTINUITY)) return 7;
      for (unsigned i = 0; i < frame.eventCount; ++i) {
        const auto &event = frame.events[i];
        const auto key = std::make_pair(frame.epoch, event.eventId);
        auto [found, inserted] = seen.emplace(key, event);
        if (inserted)
          ++unique_events;
        else {
          const auto &original = found->second;
          if (original.sourceTimeUs != event.sourceTimeUs || original.leftStrength != event.leftStrength ||
              original.rightStrength != event.rightStrength || original.confidence != event.confidence ||
              original.sharpness != event.sharpness) {
            std::cerr << entry.path() << ": event " << event.eventId << " changed after publication\n";
            return 8;
          }
        }
      }
      ended = (frame.base.flags & LI_DS5_HAPTICS_IR_FLAG_STREAM_END) != 0;
      if (ended && (frame.eventCount || frame.lanes[0].continuousLow || frame.lanes[0].continuousHigh ||
                     frame.lanes[1].continuousLow || frame.lanes[1].continuousHigh)) return 9;
      first = false;
      previous_time = frame.windowEndUs;
      previous_sequence = frame.base.sourceSequenceNumber;
      ++packets;
      at += length;
    }
    if (first || !ended) return 10;
    ++streams;
  }
  std::cout << streams << " streams, " << packets << " packets, " << unique_events << " immutable events accepted\n";
  return streams ? 0 : 11;
}

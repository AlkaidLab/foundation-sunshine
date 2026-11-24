#pragma once
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace spatial_audio {
  enum class Format : uint32_t { mat_iec61937 = 1,
    diagnostic_call_plan = 0x80000001u };
  enum class State : uint32_t { idle,
    preparing,
    running,
    draining,
    stopped,
    fault };
  enum class Read : uint32_t { frame,
    pending,
    ended,
    failed };
  // The current MAT evidence supplies a CARRIER clock, not a calibrated mapping
  // back to the original captured media PTS. Never hide startup buffering here.
  enum class Clock : uint32_t { carrier_48000 = 1 };
  struct Frame {
    uint64_t epoch = 0, domain = 0, sequence = 0, sample_start = 0;
    uint32_t sample_rate = 48000, duration_frames = 960;
    Format format = Format::mat_iec61937;
    Clock clock = Clock::carrier_48000;
    std::vector<uint8_t> bytes;
  };
  struct Descriptor {
    uint32_t version = 1;
    Format format = Format::mat_iec61937;
    Clock clock = Clock::carrier_48000;
    uint32_t rate = 48000;
  };
  class EncodedAudioSource {
  public:
    virtual ~EncodedAudioSource() = default;
    virtual Descriptor
    descriptor() const = 0;
    virtual bool
    start() = 0;
    virtual Read
    read(Frame &) = 0;  // Own worker only; never an audiodg callback.
    virtual void
    stop() = 0;
    virtual State
    state() const = 0;
    virtual std::string
    error() const = 0;
  };
  inline void
  require(bool b, const char *why) {
    if (!b) throw std::runtime_error(why);
  }
  constexpr size_t carrier_bytes = 61440, process_bytes = 30720;
  inline void
  validate_carrier(std::span<const uint8_t> b) {
    require(b.size() == carrier_bytes, "incomplete MAT carrier cycle");
    require(b[0] == 0x72 && b[1] == 0xf8 && b[2] == 0x1f && b[3] == 0x4e, "MAT carrier preamble");
    auto pc = uint32_t(b[4]) | (uint32_t(b[5]) << 8), pd = uint32_t(b[6]) | (uint32_t(b[7]) << 8);
    require((pc & 31) == 0x16 && !(pc & 0x80) && pd >= 2 && pd <= carrier_bytes - 8 && !(pd & 1), "MAT-family carrier dimensions");
  }
}  // namespace spatial_audio

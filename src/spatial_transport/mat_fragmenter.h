#pragma once
#include "mat_wire.h"
#include "../spatial_audio/encoded_audio_source.h"

namespace spatial_transport {
  // Caller supplies a bounded sink (M4 feeder: one FEC group at a time).
  template <class Emit>
  void
  fragment(const spatial_audio::Frame &f, Emit emit) {
    using namespace spatial_audio;
    require(f.epoch && f.domain && f.format == Format::mat_iec61937 &&
              f.clock == Clock::carrier_48000 && f.sample_rate == 48000 && f.duration_frames == 960 &&
              f.sequence <= UINT64_MAX / 960 && f.sample_start == f.sequence * 960,
      "MAT transport descriptor");
    validate_carrier(f.bytes);
    const auto hash = mat_hash(f.bytes.data(), f.bytes.size());
    for (unsigned i = 0; i < MAT_WIRE_FRAGMENTS; i++) {
      uint8_t packet[MAT_WIRE_PACKET];
      mat_wire_make(packet, MAT_WIRE_DATA, f.epoch, f.domain, f.sequence, i, hash, f.bytes.data());
      emit(packet, sizeof(packet));
    }
  }
}  // namespace spatial_transport

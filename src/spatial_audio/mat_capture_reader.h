#pragma once
#include "encoded_audio_source.h"
#include "mat_capture_protocol.h"
namespace mat_capture {
  // Wire parsing only. NativeMatPipeSource separately authenticates the publisher,
  // audiodg identity and non-test descriptor before allowing this reader to run.
  template <class Io>
  spatial_audio::Read
  read_record(Io io, uint64_t &next, uint64_t &last_qpc, uint64_t epoch, uint64_t domain, spatial_audio::Frame &out) {
    using spatial_audio::require;
    uint8_t h[record_bytes];
    io(h, sizeof(h), false);
    require(valid_record(h, next, last_qpc), "MAT capture record sequence/time/bounds");
    uint32_t kind = mat_get32(h + 4);
    last_qpc = mat_get64(h + 16);
    require(kind != error_kind, "Native MAT capture failed; never synthesize EOS");
    if (kind == end_kind) {
      require(next > 0 && mat_get32(h + 28) == mat_hash(nullptr, 0), "Empty/invalid live MAT end");
      uint8_t ack[end_ack_bytes];
      encode_end_ack(ack, next);
      io(ack, sizeof(ack), true);
      return spatial_audio::Read::ended;
    }
    spatial_audio::Frame f;
    f.bytes.resize(spatial_audio::carrier_bytes);
    io(f.bytes.data(), f.bytes.size(), false);
    require(mat_hash(f.bytes.data(), f.bytes.size()) == mat_get32(h + 28), "MAT capture bytes changed");
    spatial_audio::validate_carrier(f.bytes);
    require(next < uint64_t(INT64_MAX) / 960, "Live MAT source counter overflow");
    f.epoch = epoch;
    f.domain = domain;
    f.sequence = next;
    f.sample_start = next * 960;
    ++next;
    out = std::move(f);
    return spatial_audio::Read::frame;
  }
}  // namespace mat_capture

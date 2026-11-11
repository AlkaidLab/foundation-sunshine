#pragma once
#include "../../third-party/moonlight-common-c/src/MatWire.h"
namespace mat_capture {
  constexpr uint32_t descriptor_bytes = 80, record_bytes = 40, end_ack_bytes = 16, pipe_version = 2;
  constexpr uint32_t native_object_source = 2, test_source = 0x80000001u;
  constexpr uint32_t cycle_kind = 1, end_kind = 2, error_kind = 3, record_magic = 0x4359434d;
  struct Descriptor {
    uint32_t kind = 0, publisher_pid = 0, engine_pid = 0;
    uint64_t publisher_created = 0, engine_created = 0, epoch = 0, encoder = 0, qpc_hz = 0;
  };
  inline void
  encode_descriptor(uint8_t *p, const Descriptor &d) {
    memset(p, 0, descriptor_bytes);
    memcpy(p, "MCAPv002", 8);
    mat_put32(p + 8, descriptor_bytes);
    mat_put32(p + 12, pipe_version);
    mat_put32(p + 16, d.kind);
    mat_put64(p + 24, d.publisher_created);
    mat_put32(p + 32, d.publisher_pid);
    mat_put32(p + 36, d.engine_pid);
    mat_put64(p + 40, d.engine_created);
    mat_put64(p + 48, d.epoch);
    mat_put64(p + 56, d.encoder);
    mat_put32(p + 64, 48000);
    mat_put32(p + 68, MAT_WIRE_CYCLE);
    mat_put64(p + 72, d.qpc_hz);
  }
  inline bool
  decode_descriptor(const uint8_t *p, Descriptor &d) {
    if (memcmp(p, "MCAPv002", 8) || mat_get32(p + 8) != descriptor_bytes || mat_get32(p + 12) != pipe_version || mat_get32(p + 20) ||
        mat_get32(p + 64) != 48000 || mat_get32(p + 68) != MAT_WIRE_CYCLE) return false;
    d = { mat_get32(p + 16), mat_get32(p + 32), mat_get32(p + 36), mat_get64(p + 24), mat_get64(p + 40),
      mat_get64(p + 48), mat_get64(p + 56), mat_get64(p + 72) };
    // Source 2 binds an observed object renderer feeding a dedicated native MAT
    // worker via paired MainPluginRenderer pre-submit observations.
    return d.kind == native_object_source && d.publisher_pid && d.engine_pid && d.publisher_created && d.engine_created &&
           d.epoch && d.epoch <= INT64_MAX && d.encoder && d.qpc_hz && d.qpc_hz <= INT64_MAX;
  }
  inline void
  encode_end_ack(uint8_t *p, uint64_t cycles) {
    memcpy(p, "MCAPDONE", 8);
    mat_put64(p + 8, cycles);
  }
  inline bool
  valid_end_ack(const uint8_t *p, uint64_t cycles) { return cycles && memcmp(p, "MCAPDONE", 8) == 0 && mat_get64(p + 8) == cycles; }
  inline void
  encode_record(uint8_t *p, uint32_t kind, uint64_t index, uint64_t qpc, const uint8_t *data, size_t bytes) {
    memset(p, 0, record_bytes);
    mat_put32(p, record_magic);
    mat_put32(p + 4, kind);
    mat_put64(p + 8, index);
    mat_put64(p + 16, qpc);
    mat_put32(p + 24, (uint32_t) bytes);
    mat_put32(p + 28, mat_hash(data, bytes));
  }
  inline bool
  valid_record(const uint8_t *p, uint64_t expected, uint64_t last_qpc) {
    uint32_t kind = mat_get32(p + 4), bytes = mat_get32(p + 24);
    return mat_get32(p) == record_magic && kind >= cycle_kind && kind <= error_kind && mat_get64(p + 8) == expected &&
           mat_get64(p + 16) >= last_qpc && mat_get64(p + 16) > 0 && mat_get64(p + 16) <= INT64_MAX &&
           !mat_get64(p + 32) && bytes == (kind == cycle_kind ? MAT_WIRE_CYCLE : 0);
  }
}  // namespace mat_capture

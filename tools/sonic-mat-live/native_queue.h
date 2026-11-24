#pragma once
#include "../../src/spatial_audio/stream_output_protocol.h"
namespace sonic_live {
  inline void
  initialize_native_queue(receiver::Shared &input, spatial_stream::Output &output, uint64_t domain, uint64_t epoch, const uint64_t nonce[2]) {
    memset(&input, 0, sizeof(input));
    memset(&output, 0, sizeof(output));
    input.contract = { receiver::magic, 1, 320, sizeof(input), sizeof(receiver::Slot), 32, 124968, sizeof(receiver::Call), domain, epoch, { nonce[0], nonce[1] }, GetCurrentProcessId(), 48000, 480, 0, receiver::hz() };
    receiver::put(input.writer.heartbeat, receiver::qpc());
    output.magic = spatial_stream::streamMagic;
    output.version = spatial_stream::streamVersion;
    output.bytes = sizeof(output);
    output.capacity = 8;
    output.payloadBytes = spatial_stream::payloadCapacity;
    output.format = spatial_stream::nativeFormat;
    output.ownerPid = GetCurrentProcessId();
    output.domain = domain;
    output.epoch = epoch;
    memcpy(output.nonce, nonce, 16);
    auto &stats = output.header;
    stats.createHr = stats.mapperHr = stats.initializeHr = stats.submitHr = stats.processHr = E_PENDING;
  }
  inline bool
  enqueue_native(receiver::Shared &input, const receiver::Slot &packet, HANDLE data) {
    receiver::put(input.writer.heartbeat, receiver::qpc());
    if (receiver::get(input.common.fault) || receiver::get(input.common.cancel) || receiver::get(input.common.producerDone)) return false;
    const auto &h = packet.header;
    if (h.calls > receiver::callCapacity || h.frames != 480 || (h.kind != 1 && h.kind != 2)) {
      receiver::fail(input, receiver::badFrame);
      return false;
    }
    auto w = receiver::get(input.writer.write), r = receiver::get(input.reader.read);
    if (r < 0 || w < r || w == INT64_MAX) {
      receiver::fail(input, receiver::counters);
      return false;
    }
    if (w - r >= 32) return false;
    auto &destination = input.slots[w % 32];
    auto m = h;
    m.magic = receiver::frameMagic;
    m.bytes = sizeof(m) + m.calls * sizeof(receiver::Call);
    m.sequence = w;
    m.domain = input.contract.domain;
    m.epoch = input.contract.epoch;
    m.enqueuedQpc = receiver::qpc();
    m.reserved = 0;
    m.checksum = receiver::checksum(m, packet.calls);
    destination.header = m;
    if (m.calls) memcpy(destination.calls, packet.calls, m.calls * sizeof(receiver::Call));
    receiver::put(input.writer.write, w + 1);
    if (m.kind == 2) receiver::put(input.common.producerDone, 1);
    SetEvent(data);
    return true;
  }
}  // namespace sonic_live

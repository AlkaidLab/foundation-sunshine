#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "src/platform/datagram_send.h"

namespace streaming {
  // Queue entries carry a registration ID, so a stale packet cannot resolve
  // to a different session even if the allocator reuses the same address.
  template <class Session>
  class live_broadcast_sessions_t {
  public:
    std::uint64_t
    add(const std::shared_ptr<Session> &session) {
      std::lock_guard lock(_mutex);
      const auto id = ++_last_id;
      _sessions.emplace(id, session);
      return id;
    }

    void
    remove(std::uint64_t id) {
      std::lock_guard lock(_mutex);
      _sessions.erase(id);
    }

    std::shared_ptr<Session>
    snapshot(std::uint64_t id) {
      std::lock_guard lock(_mutex);
      const auto it = _sessions.find(id);
      return it == _sessions.end() ? nullptr : it->second.lock();
    }

  private:
    std::mutex _mutex;
    std::uint64_t _last_id = 0;
    std::unordered_map<std::uint64_t, std::weak_ptr<Session>> _sessions;
  };

  // MS-WSASENDMSG-20261120 (Return value): would-block is a transient drop.
  template <class Sent, class Dropped, class Failed>
  void
  account_audio_datagram(bool sent, bool would_block, Sent record_sent, Dropped record_drop, Failed record_failure) {
    if (sent) {
      record_sent();
    }
    else if (would_block) {
      record_drop();
    }
    else {
      record_failure();
    }
  }

  struct video_batch_result_t {
    std::size_t sent = 0;
    std::size_t dropped_for_backpressure = 0;
  };

  // A paced batch already consumed its time slot. On local backpressure
  // discard the unsent tail instead of retrying it or building up a queue.
  template <class Active, class SendBatch, class SendPacket>
  video_batch_result_t
  submit_video_batch(platf::batched_send_info_t &batch, Active active, SendBatch send_batch, SendPacket send_packet) {
    if (!active()) return {};
    if (send_batch(batch)) return { batch.block_count, 0 };
    if (batch.would_block) return { 0, batch.block_count };

    video_batch_result_t result;
    for (std::size_t i = 0; i < batch.block_count && active(); ++i) {
      const auto block = batch.block_offset + i;
      auto packet = platf::send_info_t {
        batch.headers ? batch.headers + block * batch.header_size : nullptr,
        batch.header_size,
        batch.buffer_for_payload_offset(block * batch.payload_size).buffer,
        batch.payload_size,
        batch.native_socket,
        batch.target_address,
        batch.target_port,
        batch.source_address,
      };
      if (send_packet(packet)) {
        ++result.sent;
      }
      else if (packet.would_block) {
        result.dropped_for_backpressure = batch.block_count - i;
        break;
      }
    }
    return result;
  }
}  // namespace streaming

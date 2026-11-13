#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

#include <boost/asio.hpp>
#include "crypto.h"
#include <moonlight-common-c/src/EndpointRefreshProtocol.h>

namespace stream {
  struct endpoint_refresh_stats_t {
    std::uint64_t valid_probes {};
    std::uint64_t sequence_gap_estimate {};
    std::uint64_t stale_probes {};
    std::uint64_t invalid_packets {};
    std::uint64_t challenges_generated {};
    std::uint64_t responses_validated {};
    std::uint64_t acknowledgements_generated {};
    std::uint64_t response_send_failures {};
    std::uint64_t port_changes {};
  };

  // Mutable validation state belongs exclusively to the shared receive thread.
  // Senders only sample the confirmed port; no locks or QoS calls on that path.
  class endpoint_refresh_t {
  public:
    using endpoint_t = boost::asio::ip::udp::endpoint;
    using packet_t = std::array<unsigned char, LI_ER_PACKET_SIZE>;
    using clock = std::chrono::steady_clock;

    endpoint_refresh_t(const boost::asio::ip::address &address, uint8_t stream,
      std::string_view session, const crypto::aes_t &ri_key):
        address(address), stream(stream) {
      if (session.size() != id.size() || ri_key.size() != LI_ER_RI_KEY_SIZE) {
        return;
      }
      std::memcpy(id.data(), session.data(), id.size());
      valid = LiErDeriveKey(ri_key.data(), id.data(), key.data());
    }

    ~endpoint_refresh_t() { LiErWipe(key.data(), key.size()); }

    bool ready() const { return valid; }
    uint16_t port() const { return confirmed_port.load(std::memory_order_acquire); }

    endpoint_refresh_stats_t stats() const noexcept {
      return {
        valid_probes.load(std::memory_order_relaxed),
        sequence_gap_estimate.load(std::memory_order_relaxed),
        stale_probes.load(std::memory_order_relaxed),
        invalid_packets.load(std::memory_order_relaxed),
        challenges_generated.load(std::memory_order_relaxed),
        responses_validated.load(std::memory_order_relaxed),
        acknowledgements_generated.load(std::memory_order_relaxed),
        response_send_failures.load(std::memory_order_relaxed),
        port_changes.load(std::memory_order_relaxed),
      };
    }

    void record_response_send_failure() noexcept {
      response_send_failures.fetch_add(1, std::memory_order_relaxed);
    }

    std::optional<packet_t> receive(const void *data, size_t size, const endpoint_t &peer, clock::time_point now) {
      LI_ER_MESSAGE message {};
      if (!valid || peer.address() != address || !peer.port() ||
          !LiErDecode(data, size, stream, id.data(), key.data(), &message)) {
        invalid_packets.fetch_add(1, std::memory_order_relaxed);
        return {};
      }

      if (message.type == LI_ER_PROBE) {
        if (message.sequence <= last_probe) {
          stale_probes.fetch_add(1, std::memory_order_relaxed);
          return {};
        }
        if (last_probe != 0 && message.sequence > last_probe && message.sequence - last_probe > 1) {
          sequence_gap_estimate.fetch_add(message.sequence - last_probe - 1, std::memory_order_relaxed);
        }
        last_probe = message.sequence;
        valid_probes.fetch_add(1, std::memory_order_relaxed);
        const auto previous_port = port();
        if (peer.port() == previous_port) {
          if (now - last_ack < std::chrono::milliseconds(250)) {
            return {};
          }
          last_ack = now;
          acknowledgements_generated.fetch_add(1, std::memory_order_relaxed);
          return encode(LI_ER_ACK, message.sequence, confirmed_nonce.data());
        }
        // A valid probe authenticates the new NAT mapping. Media must not wait
        // for the optional challenge response, which may be lost on the path.
        confirmed_port.store(peer.port(), std::memory_order_release);
        if (previous_port != 0) {
          port_changes.fetch_add(1, std::memory_order_relaxed);
        }
        // Drop the previous port's pending challenge before the rate limit can
        // return. A delayed response must not move media back to that port.
        pending_port = 0;
        last_ack = now;
        if (now - last_challenge < std::chrono::milliseconds(250)) {
          return {};
        }
        if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
          return {};
        }
        pending_port = peer.port();
        pending_sequence = message.sequence;
        expires = now + std::chrono::seconds(2);
        confirmed_nonce = nonce;
        last_challenge = now;
        challenges_generated.fetch_add(1, std::memory_order_relaxed);
        return encode(LI_ER_CHALLENGE, pending_sequence, nonce.data());
      }
      if (message.type == LI_ER_RESPONSE && pending_port && peer.port() == pending_port &&
          now <= expires && message.sequence == pending_sequence &&
          std::memcmp(message.nonce, nonce.data(), nonce.size()) == 0) {
        confirmed_port.store(pending_port, std::memory_order_release);
        confirmed_nonce = nonce;
        pending_port = 0;
        last_ack = now;
        responses_validated.fetch_add(1, std::memory_order_relaxed);
        acknowledgements_generated.fetch_add(1, std::memory_order_relaxed);
        return encode(LI_ER_ACK, message.sequence, nonce.data());
      }
      invalid_packets.fetch_add(1, std::memory_order_relaxed);
      return {};
    }

  private:
    std::optional<packet_t> encode(uint8_t type, uint64_t sequence, const unsigned char *challenge) const {
      packet_t packet {};
      if (!LiErEncode(packet.data(), type, stream, id.data(), key.data(), sequence, challenge)) {
        return {};
      }
      return packet;
    }

    boost::asio::ip::address address;
    uint8_t stream;
    bool valid { false };
    std::array<unsigned char, LI_ER_SESSION_SIZE> id {};
    std::array<unsigned char, LI_ER_KEY_SIZE> key {};
    std::atomic<uint16_t> confirmed_port { 0 };
    std::atomic<std::uint64_t> valid_probes { 0 };
    std::atomic<std::uint64_t> sequence_gap_estimate { 0 };
    std::atomic<std::uint64_t> stale_probes { 0 };
    std::atomic<std::uint64_t> invalid_packets { 0 };
    std::atomic<std::uint64_t> challenges_generated { 0 };
    std::atomic<std::uint64_t> responses_validated { 0 };
    std::atomic<std::uint64_t> acknowledgements_generated { 0 };
    std::atomic<std::uint64_t> response_send_failures { 0 };
    std::atomic<std::uint64_t> port_changes { 0 };
    uint64_t last_probe { 0 }, pending_sequence { 0 };
    uint16_t pending_port { 0 };
    std::array<unsigned char, LI_ER_NONCE_SIZE> nonce {};
    std::array<unsigned char, LI_ER_NONCE_SIZE> confirmed_nonce {};
    clock::time_point expires {}, last_challenge {}, last_ack {};
  };
}

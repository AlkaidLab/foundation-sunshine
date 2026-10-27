#pragma once
#include "../spatial_audio/native_mat_pipe_source.h"
#include "mat_fragmenter.h"
#include "../../third-party/moonlight-common-c/src/MatLiveProfile.h"
#include "../../third-party/moonlight-common-c/src/MatLiveCrypto.h"
#include <openssl/rand.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace spatial_transport {
  struct LiveSession {
    LI_MAT_LIVE_PROFILE profile {};
    std::unique_ptr<spatial_audio::EncodedAudioSource> source;
    std::atomic<bool> started { false };
  };
  inline std::shared_ptr<LiveSession>
  prepare_live(const std::string &pipe, const std::string &producer) {
#if defined(_WIN32)
    if (pipe.empty() && producer.empty()) return {};
    spatial_audio::require(!pipe.empty() && (!producer.empty() || pipe == spatial_audio::mat_auto_pipe), "Live MAT source identity required");
    auto source = std::make_unique<spatial_audio::NativeMatPipeSource>();
    source->prepare(pipe, producer);
    auto result = std::make_shared<LiveSession>();
    uint64_t epoch = 0;
    spatial_audio::require(RAND_bytes(reinterpret_cast<unsigned char *>(&epoch), sizeof(epoch)) == 1, "MAT live epoch entropy");
    result->profile.epoch = (epoch & INT64_MAX) | 1;
    result->profile.domain = 1;
    result->source = std::move(source);
    return result;
#else
    (void) pipe;
    (void) producer;
    return {};
#endif
  }
  inline std::string
  describe_live(const LiveSession &s) {
    return "a=x-ss-audio.mat-live:1\r\na=x-ss-audio.mat-live-carrier:" MAT_CARRIER
           "\r\na=x-ss-audio.mat-live-clock:48000\r\na=x-ss-audio.mat-live-crypto:" MAT_LIVE_CRYPTO
           "\r\na=x-ss-audio.mat-live-epoch:" +
           std::to_string(s.profile.epoch) +
           "\r\na=x-ss-audio.mat-live-domain:" + std::to_string(s.profile.domain) + "\r\n";
  }
  inline bool
  accept_live(const LiveSession *s, const std::string &version, const std::string &carrier, const std::string &clock,
    const std::string &crypto, const std::string &epoch, const std::string &domain) {
    return s && version == "1" && MatLiveProfileConfirm(&s->profile, MAT_LIVE_NAME, carrier.c_str(), clock.c_str(), crypto.c_str(), epoch.c_str(), domain.c_str());
  }
  // Source lifetime is the stream lifetime. No file/count/SHA or source-sample catch-up.
  // Every packet is independently authenticated using a 64-bit counter before RTP/FEC.
  template <class Emit, class Wait, class Cancel>
  uint64_t
  live_session(LiveSession &session, const uint8_t *key, size_t key_size, Emit emit, Wait wait, Cancel cancelled) {
    using spatial_audio::require;
    using Clock = std::chrono::steady_clock;
    require(session.source && !session.started.exchange(true), "Live MAT source lease already used");
#if defined(_WIN32)
    if (auto pipe = dynamic_cast<spatial_audio::NativeMatPipeSource *>(session.source.get())) pipe->cancel_when(cancelled);
#endif
    struct Stop {
      spatial_audio::EncodedAudioSource &source;
      ~Stop() { source.stop(); }
    } stop { *session.source };
    const auto descriptor = session.source->descriptor();
    require(descriptor.version == 1 && descriptor.format == spatial_audio::Format::mat_iec61937 &&
              descriptor.clock == spatial_audio::Clock::carrier_48000 && descriptor.rate == 48000,
      "Live MAT source format/clock unsupported");
    const auto source_failed = [&](const char *message) {
      const auto detail = session.source->error();
      throw std::runtime_error(detail.empty() ? message : std::string(message) + ": " + detail);
    };
    if (!session.source->start()) source_failed("Live MAT source could not start");
    MAT_LIVE_CIPHER cipher {};
    require(MatLiveCipherInit(&cipher, key, key_size, session.profile.epoch, session.profile.domain), "Live MAT key");
    struct Wipe {
      MAT_LIVE_CIPHER &c;
      ~Wipe() { MatLiveWipe(&c, sizeof(c)); }
    } wipe { cipher };
    uint64_t packet = 0, cycles = 0;
    uint32_t hash = 2166136261u;
    const auto send = [&](const uint8_t *plain) {
      require(!cancelled() && packet < uint64_t(INT64_MAX), "Live MAT cancelled/counter overflow");
      uint8_t wire[MAT_LIVE_PACKET];
      require(MatLiveSeal(&cipher, packet++, plain, wire), "Live MAT packet encryption");
      emit(wire, sizeof(wire));
    };
    uint8_t control[MAT_WIRE_PACKET];
    mat_wire_make(control, MAT_WIRE_SYNC, session.profile.epoch, session.profile.domain, 0, 0, 0, nullptr);
    for (unsigned i = 0; i < 4; i++) send(control);
    auto last = Clock::now();
    spatial_audio::Frame frame;
    while (!cancelled()) {
      auto status = session.source->read(frame);
      if (status == spatial_audio::Read::ended) break;
      if (status == spatial_audio::Read::failed) source_failed("Native MAT source failed");
      if (status == spatial_audio::Read::pending) {
        require(Clock::now() - last < std::chrono::seconds(2), "Live MAT source stalled");
        wait(std::chrono::milliseconds(1));
        continue;
      }
      require(status == spatial_audio::Read::frame && frame.sequence == cycles && frame.sample_start == cycles * 960 &&
                frame.format == spatial_audio::Format::mat_iec61937 && frame.clock == spatial_audio::Clock::carrier_48000 &&
                frame.sample_rate == 48000 && frame.duration_frames == 960,
        "Live MAT source descriptor/sequence/clock");
      spatial_audio::validate_carrier(frame.bytes);
      require(cycles < uint64_t(INT64_MAX) / 20000000, "Live MAT cycle counter overflow");
      // Pace each cycle from the moment its frame is available. The source runs
      // on the audio device clock; a fixed host-clock origin would accumulate
      // drift until the deadline check fails, or let a faster source back up
      // the pipe. A queued frame starts immediately, so backlog drains.
      const auto cycle_start = Clock::now();
      frame.epoch = session.profile.epoch;
      frame.domain = session.profile.domain;
      unsigned index = 0;
      fragment(frame, [&](const uint8_t *data, size_t) {
        auto target = cycle_start + std::chrono::microseconds(uint64_t(index++) * 20000 / MAT_WIRE_FRAGMENTS);
        while (Clock::now() < target) {
          require(!cancelled(), "Live MAT cancelled");
          wait(target - Clock::now());
        }
        require(Clock::now() - target < std::chrono::milliseconds(60), "Live MAT missed deadline; no burst recovery");
        send(data);
      });
      hash = mat_hash_update(hash, frame.bytes.data(), frame.bytes.size());
      ++cycles;
      last = Clock::now();
    }
    require(!cancelled() && cycles > 0, "Live MAT cancelled/empty; no successful EOS");
    mat_wire_make(control, MAT_WIRE_END, session.profile.epoch, session.profile.domain, cycles, 0, hash, nullptr);
    for (unsigned i = 0; i < 4; i++) send(control);
    return cycles;
  }
}  // namespace spatial_transport

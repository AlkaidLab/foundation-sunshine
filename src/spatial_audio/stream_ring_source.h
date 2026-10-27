#pragma once
#include "encoded_audio_source.h"
#include "stream_output_protocol.h"
#include <memory>

namespace spatial_audio {
  class StreamRingSource final: public EncodedAudioSource {
    spatial_stream::Output &ring_;
    uint64_t epoch_, domain_, expected_ = 0, frame_ = 0;
    State state_ = State::idle;
    std::string error_;
    std::vector<uint8_t> partial_;
    uint64_t first_qpc_ = 0, last_cycle_qpc_ = 0;
    std::unique_ptr<spatial_stream::OutputSlot> copy_ = std::make_unique<spatial_stream::OutputSlot>();

  public:
    StreamRingSource(spatial_stream::Output &ring, uint64_t epoch, uint64_t domain): ring_(ring), epoch_(epoch), domain_(domain) { partial_.reserve(carrier_bytes); }
    Descriptor
    descriptor() const override { return {}; }
    uint64_t
    capture_qpc() const { return last_cycle_qpc_; }
    bool
    start() override {
      if (state_ != State::idle) return false;
      state_ = State::preparing;
      if (ring_.magic != spatial_stream::streamMagic || ring_.version != spatial_stream::streamVersion || ring_.bytes != sizeof(ring_) || ring_.capacity != spatial_stream::slots || ring_.payloadBytes != spatial_stream::payloadCapacity || ring_.format != spatial_stream::nativeFormat || ring_.epoch != epoch_ || ring_.domain != domain_ || ring_.ownerPid != GetCurrentProcessId() || receiver::get(ring_.read) != 0) {
        error_ = "native output session/format mismatch; diagnostic payload is never MAT";
        state_ = State::fault;
        return false;
      }
      state_ = State::running;
      return true;
    }
    Read
    read(Frame &out) override {
      if (state_ == State::stopped) return Read::ended;
      if (state_ != State::running) return Read::failed;
      try {
        for (;;) {
          auto status = spatial_stream::next(ring_, *copy_, expected_);
          if (status == spatial_stream::Poll::pending) return Read::pending;
          require(status != spatial_stream::Poll::failed, "encoded output ring failed");
          if (status == spatial_stream::Poll::ended) {
            require(partial_.empty(), "partial final MAT carrier cycle");
            state_ = State::stopped;
            return Read::ended;
          }
          auto &h = copy_->header;
          require(h.sampleStart == expected_ * 480 && h.bytes == process_bytes, "MAT Process block/clock dimensions");
          ++expected_;
          if (partial_.empty()) first_qpc_ = h.completedQpc;
          partial_.insert(partial_.end(), copy_->payload, copy_->payload + h.bytes);
          if (partial_.size() < carrier_bytes) continue;
          validate_carrier(partial_);
          last_cycle_qpc_ = first_qpc_;
          Frame f;
          f.epoch = epoch_;
          f.domain = domain_;
          f.sequence = frame_;
          f.sample_start = frame_ * 960;
          f.bytes = partial_;
          partial_.clear();
          ++frame_;
          out = std::move(f);
          return Read::frame;
        }
      }
      catch (const std::exception &e) {
        error_ = e.what();
        state_ = State::fault;
        receiver::put(ring_.cancel, 1);
        return Read::failed;
      }
    }
    void
    stop() override {
      receiver::put(ring_.cancel, 1);
      if (state_ != State::fault) state_ = State::stopped;
    }
    State
    state() const override { return state_; }
    std::string
    error() const override { return error_; }
  };
}  // namespace spatial_audio

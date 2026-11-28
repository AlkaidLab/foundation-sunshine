#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace usb_stream_tunnel {

  // The iPad connects only to these services: HTTP, HTTPS, RTSP, video,
  // control, audio, and microphone, in that order.
  using ports_t = std::array<std::uint16_t, 7>;

  inline constexpr std::string_view video_launch_header = "X-SS-USB-Direct-Video";

  class video_channel {
  public:
    bool
    enqueue(std::vector<char> batch);
    bool
    next_batch(std::vector<char> &batch);
    void
    complete_batch(std::size_t bytes);
    bool
    active() const;
    bool
    wait_for_reader(std::chrono::milliseconds timeout);
    void
    reader_started();
    void
    close();

    std::atomic_bool claimed { false };

  private:
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::vector<char>> queue_;
    std::size_t queued_bytes_ = 0;
    std::size_t in_flight_bytes_ = 0;
    bool closed_ = false;
    bool reader_started_ = false;
  };

  // The USB writer publishes one token. HTTPS launch/resume consumes it once.
  std::shared_ptr<video_channel>
  claim_video(std::string_view token);

  bool
  append_video_datagram(std::vector<char> &batch, std::string_view prefix, std::string_view payload);

  void
  run(std::stop_token stop, std::string host_id, std::string host_name,
    std::string local_address, ports_t ports, std::function<std::string()> make_session);

}  // namespace usb_stream_tunnel

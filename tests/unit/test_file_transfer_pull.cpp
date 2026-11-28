/**
 * @file tests/unit/test_file_transfer_pull.cpp
 * @brief Relaying a chunk of a file a client copied, over a real listener.
 */
#include "src/config.h"
#include "src/file_transfer/file_transfer_http.h"
#include "src/file_transfer/file_transfer_pull.h"

#include <array>
#include <boost/asio.hpp>
#include <condition_variable>
#include <deque>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

namespace {
  /// One request over plain HTTP; returns the raw response.
  std::string
  raw_request(unsigned short port, const std::string &method, const std::string &path, const std::string &body = {}, const std::string &extra_headers = {}) {
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket(io);
    socket.connect({ boost::asio::ip::make_address("127.0.0.1"), port });
    std::string request = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" + extra_headers;
    if (method == "POST") request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "\r\n" + body;
    boost::asio::write(socket, boost::asio::buffer(request));
    std::string response;
    std::array<char, 65536> buffer {};
    boost::system::error_code error;
    do {
      const auto size = socket.read_some(boost::asio::buffer(buffer), error);
      response.append(buffer.data(), size);
    } while (!error);
    return response;
  }

  std::string
  body_of(const std::string &response) {
    const auto end = response.find("\r\n\r\n");
    return end == std::string::npos ? std::string {} : response.substr(end + 4);
  }

  class FileTransferPull: public testing::Test {
  protected:
    web::Server<web::HTTP> server;
    std::thread listener;
    unsigned short port = 0;
    bool previous_sync = false;
    std::string session_client;  ///< the client uuid of session 7 (empty over plain HTTP)
    std::chrono::milliseconds chunk_timeout { 0 };  ///< set before SetUp() by a test that needs a short one

    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::pair<std::uint64_t, std::vector<std::uint8_t>>> sent;

    void
    SetUp() override {
      previous_sync = config::input.clipboard_sync;
      config::input.clipboard_sync = true;
      file_transfer_pull::set_hooks({
        [this](std::uint64_t sid) -> std::optional<std::string> {
          if (sid == 7) return session_client;
          return std::nullopt;
        },
        [this](std::uint64_t sid, std::vector<std::uint8_t> payload) {
          std::lock_guard lk(mu);
          sent.emplace_back(sid, std::move(payload));
          cv.notify_all();
        },
        chunk_timeout,
      });
      server.resource[file_transfer_http::kInboundRoute]["GET"] = [](auto resp, auto req) {
        file_transfer_pull::handle_pull(std::move(resp), *req);
      };
      server.resource["^/api/v1/file-transfer/chunk/([0-9a-f]{32})$"]["POST"] = [](auto resp, auto req) {
        file_transfer_pull::handle_chunk(*resp, *req);
      };
      server.config.address = "127.0.0.1";
      server.config.port = 0;
      std::promise<unsigned short> ready;
      auto bound = ready.get_future();
      listener = std::thread([&] { server.start([&](unsigned short p) { ready.set_value(p); }); });
      ASSERT_EQ(bound.wait_for(std::chrono::seconds(10)), std::future_status::ready);
      port = bound.get();
    }

    void
    TearDown() override {
      file_transfer_pull::clear();
      server.stop();
      listener.join();
      file_transfer_pull::set_hooks({});
      config::input.clipboard_sync = previous_sync;
    }

    /// The next frame sent to a client, parsed: {sid, json}.
    std::pair<std::uint64_t, nlohmann::json>
    next_frame() {
      std::unique_lock lk(mu);
      EXPECT_TRUE(cv.wait_for(lk, std::chrono::seconds(10), [this] { return !sent.empty(); }));
      if (sent.empty()) return {};
      auto [sid, frame] = std::move(sent.front());
      sent.pop_front();
      if (frame.size() < 10) {
        ADD_FAILURE() << "frame of " << frame.size() << " bytes";
        return {};
      }
      EXPECT_EQ(frame[0], 1);
      EXPECT_EQ(frame[1], file_transfer_pull::kKindPull);
      const std::uint32_t length = frame[6] | (frame[7] << 8) | (frame[8] << 16) | (frame[9] << 24);
      EXPECT_EQ(length, frame.size() - 10);
      return { sid, nlohmann::json::parse(frame.begin() + 10, frame.end()) };
    }

    std::future<std::string>
    pull(const std::string &query, std::uint64_t sid = 7) {
      const auto path = "/api/v1/file-transfer/inbound/" + std::to_string(sid) + "/0b5e3a8c1d2f4a6b8c9d0e1f2a3b4c5d0b5e3a8c1d2f4a6b8c9d0e1f2a3b4c5d/3?" + query;
      return std::async(std::launch::async, [this, path] { return raw_request(port, "GET", path); });
    }

    std::string
    chunk(const std::string &ticket, const std::string &body, const std::string &headers = {}) {
      return raw_request(port, "POST", "/api/v1/file-transfer/chunk/" + ticket, body, headers);
    }
  };
}  // namespace

TEST_F(FileTransferPull, RelaysAChunkFromTheClient) {
  auto agent = pull("offset=5&length=10");
  const auto [sid, request] = next_frame();
  EXPECT_EQ(sid, 7u);
  EXPECT_EQ(request["id"], "0b5e3a8c1d2f4a6b8c9d0e1f2a3b4c5d0b5e3a8c1d2f4a6b8c9d0e1f2a3b4c5d");
  EXPECT_EQ(request["index"], 3);
  EXPECT_EQ(request["offset"], 5);
  EXPECT_EQ(request["length"], 10);
  const auto ticket = request["ticket"].get<std::string>();
  EXPECT_EQ(ticket.size(), 32u);

  const auto posted = chunk(ticket, "0123456789");
  EXPECT_TRUE(posted.starts_with("HTTP/1.1 204")) << posted;
  const auto answer = agent.get();
  EXPECT_TRUE(answer.starts_with("HTTP/1.1 200")) << answer.substr(0, 200);
  EXPECT_EQ(body_of(answer), "0123456789");

  // A ticket is used once.
  EXPECT_TRUE(chunk(ticket, "x").starts_with("HTTP/1.1 404"));
  EXPECT_EQ(file_transfer_pull::pending_count(), 0u);
}

TEST_F(FileTransferPull, AShortChunkPassesALongOneFails) {
  auto agent = pull("offset=0&length=4");
  EXPECT_TRUE(chunk(next_frame().second["ticket"], "ab").starts_with("HTTP/1.1 204"));
  EXPECT_EQ(body_of(agent.get()), "ab");

  agent = pull("offset=0&length=4");
  EXPECT_TRUE(chunk(next_frame().second["ticket"], "abcdef").starts_with("HTTP/1.1 413"));
  const auto answer = agent.get();
  EXPECT_TRUE(answer.starts_with("HTTP/1.1 502")) << answer.substr(0, 200);
  EXPECT_NE(answer.find("too_long"), std::string::npos);
}

TEST_F(FileTransferPull, TheClientsErrorReachesTheAgent) {
  auto agent = pull("offset=0&length=4");
  EXPECT_TRUE(chunk(next_frame().second["ticket"], "", "X-File-Transfer-Error: changed\r\n").starts_with("HTTP/1.1 204"));
  const auto answer = agent.get();
  EXPECT_TRUE(answer.starts_with("HTTP/1.1 502")) << answer.substr(0, 200);
  EXPECT_NE(answer.find("\"changed\""), std::string::npos);
}

TEST_F(FileTransferPull, RefusesBadRequests) {
  EXPECT_TRUE(pull("offset=0&length=4", 8).get().starts_with("HTTP/1.1 410"));  // no such session
  EXPECT_TRUE(pull("offset=0&length=0").get().starts_with("HTTP/1.1 400"));
  EXPECT_TRUE(pull("offset=0&length=" + std::to_string(file_transfer_pull::kMaxChunk + 1)).get().starts_with("HTTP/1.1 400"));
  EXPECT_TRUE(pull("offset=x&length=4").get().starts_with("HTTP/1.1 400"));
  EXPECT_TRUE(pull("length=4").get().starts_with("HTTP/1.1 400"));
  EXPECT_TRUE(chunk("0123456789abcdef0123456789abcdef", "x").starts_with("HTTP/1.1 404"));

  config::input.clipboard_sync = false;
  EXPECT_TRUE(pull("offset=0&length=4").get().starts_with("HTTP/1.1 403"));
  std::lock_guard lk(mu);
  EXPECT_TRUE(sent.empty());
}

TEST_F(FileTransferPull, OnlyTheSessionsClientMayAnswer) {
  session_client = "another-client";
  auto agent = pull("offset=0&length=4");
  const auto ticket = next_frame().second["ticket"].get<std::string>();
  EXPECT_TRUE(chunk(ticket, "abcd").starts_with("HTTP/1.1 404"));  // this connection has no client identity
  EXPECT_EQ(file_transfer_pull::pending_count(), 1u);

  file_transfer_pull::clear();
  const auto answer = agent.get();
  EXPECT_TRUE(answer.starts_with("HTTP/1.1 503")) << answer.substr(0, 200);
}

TEST_F(FileTransferPull, AChunkAfterItsDeadlineIsRefused) {
  file_transfer_pull::set_hooks({
    [](std::uint64_t) -> std::optional<std::string> { return std::string {}; },
    [this](std::uint64_t sid, std::vector<std::uint8_t> payload) {
      std::lock_guard lk(mu);
      sent.emplace_back(sid, std::move(payload));
      cv.notify_all();
    },
    std::chrono::milliseconds(50),
    false,  // the reaper stays out of it: the chunk reaches handle_chunk's own deadline check
  });
  auto agent = pull("offset=0&length=4");
  const auto ticket = next_frame().second["ticket"].get<std::string>();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  const auto posted = chunk(ticket, "abcd");
  EXPECT_TRUE(posted.starts_with("HTTP/1.1 410")) << posted.substr(0, 100);
  const auto answer = agent.get();
  EXPECT_TRUE(answer.starts_with("HTTP/1.1 504")) << answer.substr(0, 200);
}

TEST_F(FileTransferPull, TurningClipboardSyncOffStopsAChunkOnItsWay) {
  auto agent = pull("offset=0&length=4");
  const auto ticket = next_frame().second["ticket"].get<std::string>();
  config::input.clipboard_sync = false;
  EXPECT_TRUE(chunk(ticket, "abcd").starts_with("HTTP/1.1 403"));
  const auto answer = agent.get();
  EXPECT_TRUE(answer.starts_with("HTTP/1.1 503")) << answer.substr(0, 200);
}

TEST_F(FileTransferPull, AnEndedSessionAnswersItsWaitingChunksAtOnce) {
  auto agent = pull("offset=0&length=4");
  const auto ticket = next_frame().second["ticket"].get<std::string>();
  file_transfer_pull::session_stopped(8);  // another session
  EXPECT_EQ(file_transfer_pull::pending_count(), 1u);

  // The client of session 7 went away: no waiting for the ticket's deadline.
  file_transfer_pull::session_stopped(7);
  ASSERT_EQ(agent.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  const auto answer = agent.get();
  EXPECT_TRUE(answer.starts_with("HTTP/1.1 410")) << answer.substr(0, 200);
  EXPECT_NE(answer.find("no_session"), std::string::npos);
  EXPECT_EQ(file_transfer_pull::pending_count(), 0u);
  EXPECT_TRUE(chunk(ticket, "abcd").starts_with("HTTP/1.1 404"));
}

TEST(FileTransferPullDeadline, GrowsWithTheChunk) {
  using namespace std::chrono_literals;
  using file_transfer_pull::chunk_deadline;
  EXPECT_EQ(chunk_deadline(0), 30s);
  EXPECT_EQ(chunk_deadline(1 << 20), 46s);  // 1 MiB at 64 KiB/s
  EXPECT_EQ(chunk_deadline(file_transfer_pull::kMaxChunk), 94s);
  EXPECT_EQ(chunk_deadline(1, 50ms), 51ms);  // rounded up
  // A 1 MiB chunk over a 1 Mbit/s uplink (125000 bytes/s) needs about 8.4 s;
  // a 4 MiB one about 33.6 s, past the old fixed 30 s.
  EXPECT_GT(chunk_deadline(1 << 20) - 30s, std::chrono::milliseconds((1 << 20) * 1000 / 125000));
  EXPECT_GT(chunk_deadline(file_transfer_pull::kMaxChunk) - 30s, std::chrono::milliseconds(file_transfer_pull::kMaxChunk * 1000 / 125000));
}

class FileTransferPullReaped: public FileTransferPull {
protected:
  void
  SetUp() override {
    chunk_timeout = std::chrono::milliseconds(200);
    FileTransferPull::SetUp();
  }
};

TEST_F(FileTransferPullReaped, AClientThatNeverAnswersIsGivenUpOn) {
  auto agent = pull("offset=0&length=4");
  const auto ticket = next_frame().second["ticket"].get<std::string>();
  EXPECT_EQ(file_transfer_pull::pending_count(), 1u);
  // No chunk ever comes: the reaper answers the agent and forgets the ticket.
  ASSERT_EQ(agent.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  const auto answer = agent.get();
  EXPECT_TRUE(answer.starts_with("HTTP/1.1 504")) << answer.substr(0, 200);
  EXPECT_NE(answer.find("timeout"), std::string::npos);
  EXPECT_EQ(file_transfer_pull::pending_count(), 0u);
  EXPECT_TRUE(chunk(ticket, "abcd").starts_with("HTTP/1.1 404"));
}

TEST_F(FileTransferPullReaped, TheWaitingLimitRecoversOnceTicketsExpire) {
  std::vector<std::future<std::string>> waiting;
  for (std::size_t i = 0; i < file_transfer_pull::kMaxPending; ++i) {
    waiting.push_back(pull("offset=0&length=4"));
    next_frame();
  }
  EXPECT_EQ(file_transfer_pull::pending_count(), file_transfer_pull::kMaxPending);
  const auto busy = pull("offset=0&length=4").get();
  EXPECT_TRUE(busy.starts_with("HTTP/1.1 503")) << busy.substr(0, 200);
  EXPECT_NE(busy.find("busy"), std::string::npos);

  for (auto &w : waiting) {
    ASSERT_EQ(w.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(w.get().starts_with("HTTP/1.1 504"));
  }
  EXPECT_EQ(file_transfer_pull::pending_count(), 0u);

  auto agent = pull("offset=0&length=4");
  EXPECT_TRUE(chunk(next_frame().second["ticket"], "abcd").starts_with("HTTP/1.1 204"));
  EXPECT_EQ(body_of(agent.get()), "abcd");
}

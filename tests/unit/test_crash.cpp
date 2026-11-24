/**
 * @file tests/unit/test_crash.cpp
 * @brief Crash reports: signature, meta JSON, consent, the pending folder and the upload against a local mock (telemetry v1 /v1/crash).
 */
#include "test_temp_paths.h"
#include <src/crash/crash.h>
#include <src/crash/crash_context.h>
#include <src/crash/crash_core.h>
#include <src/crash/crash_log.h>
#include <src/crash/upload_policy.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
#include <set>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace {
  std::string
  hex(const std::uint8_t *d, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
      s += "0123456789abcdef"[d[i] >> 4];
      s += "0123456789abcdef"[d[i] & 15];
    }
    return s;
  }

  crash::frame_t
  frame(const char *module, std::uint64_t offset) {
    crash::frame_t f;
    std::strncpy(f.module, module, sizeof(f.module) - 1);
    f.offset = offset;
    return f;
  }

  fs::path
  scratch(const char *name) {
    auto dir = test_support::unique_temp_path(std::string("crash-test-") + name);
    fs::create_directories(dir);
    return dir;
  }

  void
  put(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
  }

  /// A mock /v1/crash on a plain socket: answers every request with `status`, keeps what it received.
  struct mock_server_t {
  #ifdef _WIN32
    using sock_t = SOCKET;
    static constexpr sock_t bad = INVALID_SOCKET;
    static void close_sock(sock_t s) { closesocket(s); }
  #else
    using sock_t = int;
    static constexpr sock_t bad = -1;
    static void close_sock(sock_t s) { ::close(s); }
  #endif
    sock_t listener = bad;
    std::uint16_t port = 0;
    std::thread thread;
    std::mutex mutex;
    std::vector<std::string> bodies;
    std::vector<std::string> targets;
    std::atomic<int> status { 200 };
    std::atomic<int> retry_after { 0 };  ///< sent as a Retry-After header when not 0
    std::atomic<bool> stop { false };
    std::string reply_body;  ///< sent as the JSON body of every answer; set before the request, empty = no body
    std::vector<int> script;  ///< statuses for the next requests, one each, taken from the front (under `mutex`); when empty `status` answers

    mock_server_t() {
  #ifdef _WIN32
      WSADATA wsa;
      WSAStartup(MAKEWORD(2, 2), &wsa);
  #endif
      listener = ::socket(AF_INET, SOCK_STREAM, 0);
      sockaddr_in addr {};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      EXPECT_EQ(::bind(listener, (sockaddr *) &addr, sizeof(addr)), 0);
      ::listen(listener, 8);
      socklen_t len = sizeof(addr);
      ::getsockname(listener, (sockaddr *) &addr, &len);
      port = ntohs(addr.sin_port);
      thread = std::thread([this] {
        while (!stop) {
          sock_t client = ::accept(listener, nullptr, nullptr);
          if (client == bad) break;
          if (stop) {
            close_sock(client);
            break;
          }
          std::string data;
          char chunk[8192];
          std::size_t header_end = std::string::npos;
          std::size_t length = 0;
          for (;;) {
            const auto n = ::recv(client, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            data.append(chunk, (std::size_t) n);
            if (header_end == std::string::npos) {
              header_end = data.find("\r\n\r\n");
              if (header_end != std::string::npos) {
                std::string lower = data.substr(0, header_end);
                for (auto &c : lower) c = (char) std::tolower((unsigned char) c);
                if (const auto at = lower.find("content-length:"); at != std::string::npos) length = std::stoul(lower.substr(at + 15));
              }
            }
            if (header_end != std::string::npos && data.size() >= header_end + 4 + length) break;
          }
          if (header_end != std::string::npos) {
            const auto line = data.substr(0, data.find("\r\n"));
            auto target = line.substr(line.find(' ') + 1);
            target = target.substr(0, target.find(' '));
            std::lock_guard lock(mutex);
            bodies.push_back(data.substr(header_end + 4));
            targets.push_back(target);
          }
          std::string payload;
          int answer_status = status.load();
          {
            std::lock_guard lock(mutex);
            payload = reply_body;
            if (!script.empty()) {
              answer_status = script.front();
              script.erase(script.begin());
            }
          }
          const std::string reply = "HTTP/1.1 " + std::to_string(answer_status) + " X\r\n" + (retry_after.load() ? "Retry-After: " + std::to_string(retry_after.load()) + "\r\n" : std::string()) +
                                    (payload.empty() ? std::string() : "Content-Type: application/json\r\n") + "Content-Length: " + std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload;
          ::send(client, reply.data(), (int) reply.size(), 0);
          close_sock(client);
        }
      });
    }

    ~mock_server_t() {
      stop = true;
      sock_t wake = ::socket(AF_INET, SOCK_STREAM, 0);
      sockaddr_in addr {};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr.sin_port = htons(port);
      ::connect(wake, (sockaddr *) &addr, sizeof(addr));
      close_sock(wake);
      thread.join();
      close_sock(listener);
    }

    std::string
    url() const { return "http://127.0.0.1:" + std::to_string(port); }
  };

  std::string
  write_pending(const fs::path &config, const std::string &name, bool with_dump, const char *module = "sunshine.exe", std::uint64_t offset = 0x1234, const char *product = "host") {
    crash::frame_t frames[] = { frame(module, offset), frame("sunshine.exe", 0x77) };
    crash::meta_t meta;
    meta.product = product;
    meta.version = "2026.1112.1.杂鱼";
    meta.os_version = "Windows 11 10.0.26100";
    meta.fault_code = "0xC0000005";
    meta.time = 1700000000;
    meta.frames = frames;
    meta.frame_count = 2;
    char buffer[crash::meta_buffer_size];
    const auto len = crash::render_meta(meta, buffer, sizeof(buffer));
    EXPECT_GT(len, 0u);
    const auto dir = crash::pending_dir(config);
    put(dir / (name + ".json"), std::string(buffer, len));
    if (with_dump) put(dir / (name + ".dmp"), std::string("MDMP") + std::string(100, 'x'));
    // old enough for the uploader to take it
    std::error_code ec;
    fs::last_write_time(dir / (name + ".json"), fs::file_time_type::clock::now() - std::chrono::minutes(5), ec);
    return name;
  }
}  // namespace

TEST(CrashSha256, KnownVectors) {
  std::uint8_t d[32];
  crash::sha256("", 0, d);
  EXPECT_EQ(hex(d, 32), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  crash::sha256("abc", 3, d);
  EXPECT_EQ(hex(d, 32), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";  // 56 bytes: the padding spills into a second block
  crash::sha256(two.data(), two.size(), d);
  EXPECT_EQ(hex(d, 32), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  const std::string million(1000000, 'a');
  crash::sha256(million.data(), million.size(), d);
  EXPECT_EQ(hex(d, 32), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(CrashSignature, MatchesAnIndependentComputation) {
  // python: hashlib.sha256(b"sunshine.exe+0x1234\nsunshine.exe+0x77").hexdigest()
  crash::frame_t frames[] = { frame("Sunshine.EXE", 0x1234), frame("sunshine.exe", 0x77) };
  char sig[65];
  crash::signature_hex(frames, 2, sig);
  std::uint8_t d[32];
  const std::string text = "sunshine.exe+0x1234\nsunshine.exe+0x77";
  crash::sha256(text.data(), text.size(), d);
  EXPECT_EQ(std::string(sig), hex(d, 32));
}

TEST(CrashSignature, RuntimeFramesOnTopDoNotCount) {
  crash::frame_t a[] = { frame("ntdll.dll", 0x10), frame("ucrtbase.dll", 0x20), frame("sunshine.exe", 0x500), frame("sunshine.exe", 0x600) };
  crash::frame_t b[] = { frame("kernelbase.dll", 0x99), frame("sunshine.exe", 0x500), frame("sunshine.exe", 0x600) };
  char sa[65], sb[65];
  crash::signature_hex(a, 4, sa);
  crash::signature_hex(b, 3, sb);
  EXPECT_STREQ(sa, sb);
  EXPECT_EQ(crash::first_caller_frame(a, 4), 2u);
}

TEST(CrashSignature, VersionedLinuxRuntimeFramesDoNotCount) {
  crash::frame_t a[] = { frame("libc.so.6", 0x10), frame("libstdc++.so.6.0.36", 0x20), frame("sunshine", 0x500) };
  EXPECT_EQ(crash::first_caller_frame(a, 3), 2u);
}

TEST(CrashSignature, OnlyTheFirstEightFramesCount) {
  crash::frame_t many[12];
  for (int i = 0; i < 12; ++i) many[i] = frame("sunshine.exe", 0x100 + i);
  char a[65], b[65];
  crash::signature_hex(many, 12, a);
  many[10].offset = 0xdead;
  crash::signature_hex(many, 12, b);
  EXPECT_STREQ(a, b);
  many[7].offset = 0xbeef;
  crash::signature_hex(many, 12, b);
  EXPECT_STRNE(a, b);
}

TEST(CrashSignature, DifferentOffsetsDiffer) {
  crash::frame_t x[] = { frame("sunshine.exe", 0x1) }, y[] = { frame("sunshine.exe", 0x2) };
  char sx[65], sy[65];
  crash::signature_hex(x, 1, sx);
  crash::signature_hex(y, 1, sy);
  EXPECT_STRNE(sx, sy);
  EXPECT_EQ(std::strlen(sx), 64u);
}

TEST(CrashSignature, AllRuntimeFramesFallBackToTheWholeList) {
  crash::frame_t f[] = { frame("ntdll.dll", 1), frame("kernel32.dll", 2) };
  char sig[65];
  crash::signature_hex(f, 2, sig);
  EXPECT_EQ(std::strlen(sig), 64u);
  EXPECT_EQ(crash::first_caller_frame(f, 2), 0u);
}

TEST(CrashPhase, TheProbeThreadKeepsItsWordWhileTheMainThreadMovesOn) {
  using crash::context::phase_e;
  auto &state = crash::context::state();
  crash::context::set_phase(phase_e::listeners);
  crash::context::begin_encoder_probe();
  EXPECT_EQ(state.phase.load(), static_cast<int>(phase_e::encoder_probe));
  // the main thread goes on: its words do not replace the probe's
  crash::context::set_phase(phase_e::input_wait);
  crash::context::set_phase(phase_e::tray);
  EXPECT_EQ(state.phase.load(), static_cast<int>(phase_e::encoder_probe));
  // the start is over before the probe is
  crash::context::set_phase(phase_e::none);
  EXPECT_EQ(state.phase.load(), static_cast<int>(phase_e::encoder_probe));
  crash::context::end_encoder_probe();
  EXPECT_EQ(state.phase.load(), static_cast<int>(phase_e::none));
}

TEST(CrashPhase, AProbeThatEndsBeforeTheStartDoesRestoreTheMainThreadsPhase) {
  using crash::context::phase_e;
  auto &state = crash::context::state();
  crash::context::set_phase(phase_e::listeners);
  crash::context::begin_encoder_probe();
  crash::context::set_phase(phase_e::tray);
  crash::context::end_encoder_probe();
  EXPECT_EQ(state.phase.load(), static_cast<int>(phase_e::tray));
  crash::context::set_phase(phase_e::none);
  EXPECT_EQ(state.phase.load(), static_cast<int>(phase_e::none));
}

TEST(CrashWatchClock, TheAwakeClockRunsLikeTheSteadyOneWhileTheSystemIsUp) {
  // a wrong unit (QueryUnbiasedInterruptTime counts 100 ns) would show here: the two clocks move together when nothing sleeps
  const auto awake_before = crash::awake_ms();
  const auto steady_before = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const auto awake_after = crash::awake_ms();
  const auto steady_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - steady_before).count();
  ASSERT_GE(awake_after, awake_before);
  const auto awake_delta = static_cast<long long>(awake_after - awake_before);
  EXPECT_GE(awake_delta, 200);
  EXPECT_LE(std::abs(awake_delta - steady_ms), 150);
  EXPECT_GT(awake_before, 0u);
}

TEST(CrashWatchClock, AThreadIsOverdueOnlyAfterItsTimeoutOfAwakeTime) {
  EXPECT_FALSE(crash::hang_overdue(0, 1000000, 300000));  // idle
  EXPECT_FALSE(crash::hang_overdue(1000, 1000 + 299999, 300000));
  EXPECT_TRUE(crash::hang_overdue(1000, 1000 + 300000, 300000));
  EXPECT_TRUE(crash::hang_overdue(1000, 1000 + 36000000, 300000));
  // a reading from before the thread went busy never wraps around into a hang
  EXPECT_FALSE(crash::hang_overdue(5000, 4000, 300000));
}

TEST(CrashDriverFrame, ATopFrameInADriverModuleIsADriverCrash) {
  // the null-call crash of an Intel driver: the walk starts at the caller, which lies in the driver
  crash::frame_t f[] = { frame("igd12umd64.dll", 0x4a2b10), frame("igd12umd64.dll", 0x4a1000), frame("d3d11.dll", 0x77), frame("sunshine.exe", 0x500) };
  const auto hit = crash::driver_frame(f, 4);
  EXPECT_EQ(hit.vendor, crash::gpu_driver::vendor_e::intel);
  EXPECT_EQ(hit.index, 0u);
}

TEST(CrashDriverFrame, RuntimeFramesOnTopAreSkippedLikeInTheSignature) {
  // a hang: the stuck thread waits in ntdll, called from the NVIDIA driver
  crash::frame_t f[] = { frame("ntdll.dll", 0x10), frame("KERNELBASE.dll", 0x20), frame("nvwgf2umx.dll", 0x9c0), frame("sunshine.exe", 0x500) };
  const auto hit = crash::driver_frame(f, 4);
  EXPECT_EQ(hit.vendor, crash::gpu_driver::vendor_e::nvidia);
  EXPECT_EQ(hit.index, 2u);
}

TEST(CrashDriverFrame, ADriverBelowTheHostIsNotBlamed) {
  // the host calls into the driver and crashes in its own code afterwards: the top frame is the host's
  crash::frame_t f[] = { frame("sunshine.exe", 0x500), frame("nvwgf2umx.dll", 0x9c0), frame("sunshine.exe", 0x600) };
  EXPECT_EQ(crash::driver_frame(f, 3).vendor, crash::gpu_driver::vendor_e::none);
  // nor a crash in the system's own graphics runtime
  crash::frame_t g[] = { frame("d3d11.dll", 0x10), frame("amdxx64.dll", 0x20), frame("sunshine.exe", 0x600) };
  EXPECT_EQ(crash::driver_frame(g, 3).vendor, crash::gpu_driver::vendor_e::none);
}

TEST(CrashDriverFrame, NothingToLookAt) {
  EXPECT_EQ(crash::driver_frame(nullptr, 0).vendor, crash::gpu_driver::vendor_e::none);
  crash::frame_t f[] = { frame("ntdll.dll", 1), frame("kernel32.dll", 2) };
  EXPECT_EQ(crash::driver_frame(f, 2).vendor, crash::gpu_driver::vendor_e::none);
}

TEST(CrashTime, FormatsUtc) {
  char out[24];
  crash::format_iso_utc(0, out);
  EXPECT_STREQ(out, "1970-01-01T00:00:00Z");
  crash::format_iso_utc(1700000000, out);
  EXPECT_STREQ(out, "2023-11-14T22:13:20Z");
  crash::format_iso_utc(1709164800, out);  // a leap day
  EXPECT_STREQ(out, "2024-02-29T00:00:00Z");
  crash::format_iso_utc(1793000000, out);
  EXPECT_STREQ(out, "2026-10-26T07:33:20Z");
}

TEST(CrashMeta, HasTheContractFields) {
  crash::frame_t frames[] = { frame("ntdll.dll", 0x10), frame("sunshine.exe", 0xabc), frame("libfoo.dll", 0x1) };
  crash::meta_t meta;
  meta.version = "2026.1112.1.杂鱼";
  meta.os_version = "Windows 11 10.0.26100";
  meta.fault_code = "0xC0000005";
  meta.kind = "crash";
  meta.time = 1700000000;
  meta.uptime_s = 42;
  meta.install_id = "11111111-2222-4333-8444-555555555555";
  meta.frames = frames;
  meta.frame_count = 3;
  char buffer[crash::meta_buffer_size];
  const auto len = crash::render_meta(meta, buffer, sizeof(buffer));
  ASSERT_GT(len, 0u);
  const auto j = json::parse(std::string(buffer, len));
  EXPECT_EQ(j["v"], 1);
  EXPECT_EQ(j["product"], "host");
  EXPECT_EQ(j["install_id"], "11111111-2222-4333-8444-555555555555");
  EXPECT_EQ(j["version"], "2026.1112.1.杂鱼");
  EXPECT_EQ(j["time"], "2023-11-14T22:13:20Z");
  EXPECT_EQ(j["uptime_s"], 42);
  EXPECT_EQ(j["exception_code"], "0xC0000005");
  EXPECT_EQ(j["module"], "sunshine.exe");  // the first frame that is not the runtime's
  EXPECT_EQ(j["module_offset"], "0xabc");
  ASSERT_EQ(j["frames"].size(), 3u);
  EXPECT_EQ(j["frames"][0]["module"], "ntdll.dll");
  EXPECT_EQ(j["frames"][0]["offset"], "0x10");
  EXPECT_EQ(j["kind"], "crash");
  char sig[65];
  crash::signature_hex(frames, 3, sig);
  EXPECT_EQ(j["signature"], std::string(sig));
}

TEST(CrashMeta, EscapesAndBoundsFrames) {
  crash::frame_t frames[100];
  for (auto &f : frames) f = frame("a\"b.dll", 1);
  crash::meta_t meta;
  meta.fault_code = "x\ny";
  meta.frames = frames;
  meta.frame_count = 100;
  char buffer[crash::meta_buffer_size];
  const auto len = crash::render_meta(meta, buffer, sizeof(buffer));
  ASSERT_GT(len, 0u);
  const auto j = json::parse(std::string(buffer, len));  // still valid JSON
  EXPECT_EQ(j["frames"].size(), crash::max_frames);
  EXPECT_EQ(j["frames"][0]["module"], "a\"b.dll");
  EXPECT_EQ(j["exception_code"], "x\ny");
}

TEST(CrashMeta, ReturnsZeroWhenItDoesNotFit) {
  crash::frame_t frames[] = { frame("sunshine.exe", 1) };
  crash::meta_t meta;
  meta.frames = frames;
  meta.frame_count = 1;
  char small[64];
  EXPECT_EQ(crash::render_meta(meta, small, sizeof(small)), 0u);
}

TEST(CrashConsent, FollowsTheTelemetryErrorsKey) {
  EXPECT_TRUE(crash::errors_enabled_from_conf(""));  // default on
  EXPECT_TRUE(crash::errors_enabled_from_conf("port = 47989\n"));
  EXPECT_FALSE(crash::errors_enabled_from_conf("telemetry_errors = off\n"));
  EXPECT_FALSE(crash::errors_enabled_from_conf("telemetry_errors=false"));
  EXPECT_TRUE(crash::errors_enabled_from_conf("telemetry_errors = on\n"));
  EXPECT_FALSE(crash::errors_enabled_from_conf("telemetry_errors = on\ntelemetry_errors = off\n"));  // the last one wins
  EXPECT_FALSE(crash::errors_enabled_from_conf("# telemetry_errors = on\ntelemetry_errors = \"off\"\r\n"));
}

TEST(CrashConsent, AnOldNoStaysNoUntilErrorsAreTurnedOn) {
  EXPECT_FALSE(crash::errors_enabled_from_conf("telemetry_consent = off\n"));
  EXPECT_TRUE(crash::errors_enabled_from_conf("telemetry_consent = off\ntelemetry_errors = on\n"));
  EXPECT_TRUE(crash::errors_enabled_from_conf("telemetry_consent = on\n"));
  EXPECT_TRUE(crash::errors_enabled_from_conf("telemetry_consent = ask\n"));
}

TEST(CrashEndpoint, ConfigKeyAndDefault) {
  EXPECT_EQ(crash::endpoint_from(""), "https://api.alkaidlab.com");
  EXPECT_EQ(crash::endpoint_from("telemetry_endpoint = http://127.0.0.1:9/\n"), "http://127.0.0.1:9/");
}

TEST(CrashPending, KeepsTheNewestTwentyAndDropsStrayDumps) {
  const auto config = scratch("cap");
  const auto dir = crash::pending_dir(config);
  const auto base = fs::file_time_type::clock::now() - std::chrono::hours(1);
  for (int i = 0; i < 25; ++i) {
    const auto name = "r" + std::to_string(100 + i);
    put(dir / (name + ".json"), "{}");
    put(dir / (name + ".dmp"), "d");
    fs::last_write_time(dir / (name + ".json"), base + std::chrono::seconds(i));
  }
  put(dir / "stray.dmp", "d");
  put(dir / "note.txt", "keep");
  EXPECT_EQ(crash::list_pending(dir).size(), 25u);
  EXPECT_FALSE(fs::exists(dir / "stray.dmp"));
  EXPECT_EQ(crash::trim_pending(dir), 5u);
  const auto left = crash::list_pending(dir);
  ASSERT_EQ(left.size(), crash::max_pending);
  EXPECT_EQ(left.front().meta.filename(), "r105.json");  // the five oldest went, with their dumps
  EXPECT_FALSE(fs::exists(dir / "r100.dmp"));
  EXPECT_TRUE(fs::exists(dir / "note.txt"));
  test_support::remove_tree_quiet(config);
}

TEST(CrashPending, OldReportsAgeOut) {
  const auto config = scratch("age");
  const auto dir = crash::pending_dir(config);
  put(dir / "old.json", "{}");
  put(dir / "new.json", "{}");
  fs::last_write_time(dir / "old.json", fs::file_time_type::clock::now() - std::chrono::hours(24 * 40));
  EXPECT_EQ(crash::trim_pending(dir), 1u);
  EXPECT_FALSE(fs::exists(dir / "old.json"));
  EXPECT_TRUE(fs::exists(dir / "new.json"));
  test_support::remove_tree_quiet(config);
}

TEST(CrashIdentity, CreatedOnceAndStable) {
  const auto config = scratch("id");
  EXPECT_EQ(crash::read_or_create_install_id(config, false), "");  // reading never creates
  const auto id = crash::read_or_create_install_id(config, true);
  ASSERT_EQ(id.size(), 36u);
  EXPECT_EQ(id[14], '4');  // version 4
  EXPECT_EQ(crash::read_or_create_install_id(config, true), id);
  EXPECT_EQ(crash::read_or_create_install_id(config, false), id);
  test_support::remove_tree_quiet(config);
}

TEST(CrashUpload, DeliversMetaAndDumpAndDeletesAfter2xx) {
  mock_server_t server;
  const auto config = scratch("up");
  write_pending(config, "a", true);
  write_pending(config, "b", false);
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "11111111-2222-4333-8444-555555555555"), 2);
  EXPECT_TRUE(crash::list_pending(crash::pending_dir(config)).empty());
  ASSERT_EQ(server.bodies.size(), 2u);
  EXPECT_EQ(server.targets[0], "/v1/crash");
  const auto &with_dump = server.bodies[0];
  EXPECT_NE(with_dump.find("name=\"meta\""), std::string::npos);
  EXPECT_NE(with_dump.find("name=\"dump\""), std::string::npos);
  EXPECT_NE(with_dump.find("MDMP"), std::string::npos);
  EXPECT_NE(with_dump.find("\"install_id\":\"11111111-2222-4333-8444-555555555555\""), std::string::npos);  // added at upload
  EXPECT_NE(with_dump.find("\"signature\""), std::string::npos);
  EXPECT_EQ(server.bodies[1].find("name=\"dump\""), std::string::npos);
  test_support::remove_tree_quiet(config);
}

TEST(CrashUpload, KeepsWhatTheServerCouldNotTakeYet) {
  mock_server_t server;
  const auto config = scratch("retry");
  write_pending(config, "a", true);
  for (int status : { 500, 503, 429, 408 }) {
    server.status = status;
    EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 0) << status;
    EXPECT_EQ(crash::list_pending(crash::pending_dir(config)).size(), 1u) << status;
  }
  server.status = 204;
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 1);
  EXPECT_TRUE(crash::list_pending(crash::pending_dir(config)).empty());
  test_support::remove_tree_quiet(config);
}

TEST(CrashUpload, ARateLimitOrStorageFailureStopsThePassAndKeepsEverything) {
  mock_server_t server;
  const auto config = scratch("limit");
  write_pending(config, "a", true);
  write_pending(config, "b", true);
  for (int status : { 429, 503 }) {
    server.status = status;
    server.bodies.clear();
    EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 0) << status;
    EXPECT_EQ(server.bodies.size(), 1u) << status;  // the second report was not tried
    EXPECT_EQ(crash::list_pending(crash::pending_dir(config)).size(), 2u) << status;
  }
  test_support::remove_tree_quiet(config);
}

TEST(CrashUpload, DropsWhatTheServerRejectsForGood) {
  mock_server_t server;
  const auto config = scratch("reject");
  write_pending(config, "a", true);
  server.status = 400;
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 1);
  EXPECT_TRUE(crash::list_pending(crash::pending_dir(config)).empty());
  test_support::remove_tree_quiet(config);
}

TEST(CrashUpload, NoServerLeavesTheReportAndReturnsAtOnce) {
  const auto config = scratch("down");
  write_pending(config, "a", true);
  const auto began = std::chrono::steady_clock::now();
  EXPECT_EQ(crash::upload_pending_once(config, "http://127.0.0.1:1", "x"), 0);  // nothing listens on port 1
  EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds(12));
  EXPECT_EQ(crash::list_pending(crash::pending_dir(config)).size(), 1u);
  test_support::remove_tree_quiet(config);
}

TEST(CrashUpload, SkipsAReportThatIsStillBeingWritten) {
  mock_server_t server;
  const auto config = scratch("fresh");
  const auto dir = crash::pending_dir(config);
  char tag[32];
#ifdef _WIN32
  std::snprintf(tag, sizeof(tag), "-p%08lx", (unsigned long) GetCurrentProcessId());
#else
  std::snprintf(tag, sizeof(tag), "-p%08lx", (unsigned long) getpid());
#endif
  put(dir / (std::string("1-host-hang") + tag + ".json"), "{\"v\":1}");  // this process's own, modified just now
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 0);
  EXPECT_TRUE(server.bodies.empty());
  EXPECT_EQ(crash::list_pending(dir).size(), 1u);
  test_support::remove_tree_quiet(config);
}

TEST(CrashUpload, CapsWhatIsOnDisk) {
  mock_server_t server;
  server.status = 503;
  const auto config = scratch("cap20");
  for (int i = 0; i < 24; ++i) write_pending(config, "r" + std::to_string(100 + i), false);
  crash::upload_pending_once(config, server.url(), "x");
  EXPECT_EQ(crash::list_pending(crash::pending_dir(config)).size(), crash::max_pending);
  test_support::remove_tree_quiet(config);
}

TEST(CrashUpload, UnreadableMetaIsDiscarded) {
  mock_server_t server;
  const auto config = scratch("junk");
  const auto dir = crash::pending_dir(config);
  put(dir / "bad.json", "not json");
  fs::last_write_time(dir / "bad.json", fs::file_time_type::clock::now() - std::chrono::minutes(5));
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 0);
  EXPECT_FALSE(fs::exists(dir / "bad.json"));
  EXPECT_TRUE(server.bodies.empty());
  test_support::remove_tree_quiet(config);
}

namespace {
  std::string
  wide(const std::string &ascii) {
    std::string out;
    for (char c : ascii) {
      out.push_back(c);
      out.push_back('\0');
    }
    return out;
  }
}  // namespace

TEST(CrashRedact, AccountNameBehindAHomeMarkerIsBlankedInAsciiAndUtf16) {
  const std::string ascii = std::string("MDMP\0\0", 6) + "C:\\Users\\alice\\AppData\\sunshine.exe" + std::string(3, '\0') + "/home/bob/.config" + std::string(2, '\0') + "/Users/carol Smith/x";
  std::string data = ascii + wide("C:\\Users\\dave\\Desktop\\sunshine.exe");
  const auto before = data.size();
  EXPECT_EQ(crash::redact_private_names(data, {}), 4u);
  EXPECT_EQ(data.size(), before);
  for (const char *secret : { "alice", "bob", "carol", "Smith" }) EXPECT_EQ(data.find(secret), std::string::npos) << secret;
  EXPECT_EQ(data.find(wide("dave")), std::string::npos);
  EXPECT_NE(data.find("\\Users\\xxxxx\\AppData\\sunshine.exe"), std::string::npos);  // the folders after the account stay (they say where, not who)
  EXPECT_NE(data.find(wide("\\Users\\xxxx\\Desktop\\sunshine.exe")), std::string::npos);
}

TEST(CrashRedact, NonAsciiAccountNameInUtf16) {
  // "\Users\小明\x" in UTF-16LE
  std::string data = wide("C:\\Users\\");
  data += std::string("\x0f\x5c\x0e\x66", 4);  // U+5C0F U+660E
  data += wide("\\Documents");
  crash::redact_private_names(data, {});
  EXPECT_EQ(data.find(std::string("\x0f\x5c", 2)), std::string::npos);
  EXPECT_NE(data.find(wide("\\Documents")), std::string::npos);
}

TEST(CrashRedact, KnownNamesAreBlankedAsWholeTokensOnly) {
  const std::string text = "host SKYPC-01 and skypc-01; user Sky Hua; sunshine.exe SkyHua2";
  std::string data = text + wide("SKYPC-01") + wide(" ") + wide("skyhua");
  const auto hits = crash::redact_private_names(data, { "SkyPC-01", "SkyHua", "sun", "ab" });
  EXPECT_EQ(hits, 4u);
  EXPECT_EQ(data.find("SKYPC-01"), std::string::npos);
  EXPECT_EQ(data.find("skypc-01"), std::string::npos);
  EXPECT_EQ(data.find(wide("SKYPC-01")), std::string::npos);
  EXPECT_EQ(data.find(wide("skyhua")), std::string::npos);
  EXPECT_NE(data.find("sunshine.exe"), std::string::npos);  // "sun" inside a longer word is not a hit
  EXPECT_NE(data.find("SkyHua2"), std::string::npos);  // neither is a name followed by more word characters
}

TEST(CrashRedact, LeavesADumpWithNothingPrivateAsItWas) {
  std::string data = std::string("MDMP") + std::string(200, '\x41') + "sunshine.exe" + std::string(50, '\0');
  const auto copy = data;
  EXPECT_EQ(crash::redact_private_names(data, { "alice" }), 0u);
  EXPECT_EQ(data, copy);
}

TEST(CrashUpload, TheDumpThatLeavesHasNoAccountName) {
  mock_server_t server;
  const auto config = scratch("redact");
  const auto name = write_pending(config, "d", true);
  const auto dir = crash::pending_dir(config);
  put(dir / (name + ".dmp"), std::string("MDMP") + "C:\\Users\\mallory\\AppData\\sunshine.exe" + wide("C:\\Users\\mallory\\x") + std::string(40, '\0'));
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "11111111-2222-4333-8444-555555555555"), 1);
  ASSERT_EQ(server.bodies.size(), 1u);
  EXPECT_NE(server.bodies[0].find("name=\"dump\""), std::string::npos);
  EXPECT_EQ(server.bodies[0].find("mallory"), std::string::npos);
  EXPECT_EQ(server.bodies[0].find(wide("mallory")), std::string::npos);
  EXPECT_NE(server.bodies[0].find("\\Users\\xxxxxxx\\AppData"), std::string::npos);
  test_support::remove_tree_quiet(config);
}

TEST(CrashMeta, ModulesCarryNoDirectoryEvenWhenOneIsPassedIn) {
  // the Windows resolver and the POSIX one cut a module to its file name; the meta renderer must not rely on that alone
  crash::frame_t frames[] = { frame("C:\\Users\\alice\\AppData\\Local\\Sunshine\\sunshine.exe", 0x10), frame("/home/bob/sunshine/libx.so", 0x20) };
  crash::meta_t meta;
  meta.frames = frames;
  meta.frame_count = 2;
  char buffer[crash::meta_buffer_size];
  const auto len = crash::render_meta(meta, buffer, sizeof(buffer));
  ASSERT_GT(len, 0u);
  const std::string text(buffer, len);
  EXPECT_EQ(text.find("alice"), std::string::npos);
  EXPECT_EQ(text.find("bob"), std::string::npos);
  EXPECT_NE(text.find("sunshine.exe"), std::string::npos);
  EXPECT_NE(text.find("libx.so"), std::string::npos);
}


// ---- the session log and the machine that go with a report -------------------------------------------------------------------------------------

TEST(CrashUpload, TheLogOfTheRunThatCrashedGoesWithTheReportAnonymised) {
  mock_server_t server;
  const auto config = scratch("log");
  write_pending(config, "a", true);
  const auto now = fs::file_time_type::clock::now();
  put(config / "sunshine.log", "[2026-11-14 22:00:00.000]: Info: the new run\n");
  fs::last_write_time(config / "sunshine.log", now - std::chrono::seconds(5));
  put(config / "sunshine.log.1", "[2026-11-14 21:00:00.000]: Info: client 192.168.31.57 paired in C:\\Users\\Tanaka\\x\n[2026-11-14 21:00:01.000]: Error: nvenc init failed 0x887A0005\n");
  fs::last_write_time(config / "sunshine.log.1", now - std::chrono::minutes(5));  // as old as the report: it is the run that crashed
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 1);
  ASSERT_EQ(server.bodies.size(), 1u);
  const auto &body = server.bodies[0];
  for (const char *part : { "name=\"log\"", "name=\"log_head\"", "name=\"log_tail\"", "name=\"log_lines\"", "application/gzip" }) EXPECT_NE(body.find(part), std::string::npos) << part;
  EXPECT_NE(body.find("nvenc init failed 0x887A0005"), std::string::npos);  // the sidecars are plain text
  EXPECT_NE(body.find("<ip#1>"), std::string::npos);
  EXPECT_EQ(body.find("192.168.31.57"), std::string::npos);
  EXPECT_EQ(body.find("Tanaka"), std::string::npos);
  EXPECT_EQ(body.find("the new run"), std::string::npos);
  fs::remove_all(config);
}

TEST(CrashUpload, NoLogMeansNoLogPartAndAPanelReportNeverCarriesTheHostLog) {
  mock_server_t server;
  const auto config = scratch("nolog");
  write_pending(config, "a", false);
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 1);
  EXPECT_EQ(server.bodies[0].find("name=\"log\""), std::string::npos);
  // a host log is there, the report is the control panel's: the host log does not go with it
  put(config / "sunshine.log", "[2026-11-14 22:00:00.000]: Info: the host log marker\n");
  fs::last_write_time(config / "sunshine.log", fs::file_time_type::clock::now() - std::chrono::minutes(5));
  write_pending(config, "b", false, "sunshine.exe", 0x1234, "panel");
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 1);
  ASSERT_EQ(server.bodies.size(), 2u);
  EXPECT_EQ(server.bodies[1].find("name=\"log\""), std::string::npos);
  EXPECT_EQ(server.bodies[1].find("the host log marker"), std::string::npos);
  EXPECT_NE(server.bodies[1].find("\"product\":\"panel\""), std::string::npos);
  write_pending(config, "c", false);  // the same log goes with a host report
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 1);
  EXPECT_NE(server.bodies[2].find("the host log marker"), std::string::npos);
  fs::remove_all(config);
}

TEST(CrashUpload, TheMachineIsReadAtUploadAndNeverWhenAlreadyInTheReport) {
  mock_server_t server;
  const auto config = scratch("env");
  write_pending(config, "a", false);
  crash::set_environment_provider([] { return std::string("{\"cpu\":\"Probe CPU\",\"cores\":8}"); });
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 1);
  EXPECT_NE(server.bodies[0].find("\"env\":{\"cores\":8,\"cpu\":\"Probe CPU\"}"), std::string::npos);
  crash::set_environment_provider({});
  fs::remove_all(config);
}

TEST(CrashUpload, WaitsByRetryAfterThenByDoublingFromFifteenMinutesAndResetsOnSuccess) {
  mock_server_t server;
  crash::set_upload_jitter_source([] { return 0.0; });  // the exact numbers; the random share has its own test below
  const auto config = scratch("backoff");
  write_pending(config, "a", false);
  server.status = 429;
  server.retry_after = 120;
  crash::upload_pending_once(config, server.url(), "x");
  EXPECT_EQ(crash::upload_wait_hint_s(), 120);
  server.status = 500;
  server.retry_after = 0;
  crash::upload_pending_once(config, server.url(), "x");
  const auto first = crash::upload_wait_hint_s();
  crash::upload_pending_once(config, server.url(), "x");
  EXPECT_GE(first, 15 * 60);
  EXPECT_EQ(crash::upload_wait_hint_s(), first * 2);  // doubling, as Crashpad retries
  server.status = 200;
  crash::upload_pending_once(config, server.url(), "x");
  EXPECT_EQ(crash::upload_wait_hint_s(), 0);
  crash::set_upload_jitter_source({});
  fs::remove_all(config);
}

// ---------------------------------------------------------------------------------------------------------------------
// a crash report always arrives: what the uploader does with each answer, and the wait after a failure

TEST(CrashUploadPolicy, OnlyAnAnswerAboutTheBodyItselfDropsAReport) {
  using crash::upload_policy::judge;
  using crash::upload_policy::verdict_e;
  for (const long status : { 200, 201, 204 }) EXPECT_EQ(judge(status, true), verdict_e::delivered) << status;
  for (const long status : { 400, 415, 422 }) EXPECT_EQ(judge(status, true), verdict_e::drop) << status;
  EXPECT_EQ(judge(413, true), verdict_e::send_without_dump);
  EXPECT_EQ(judge(413, false), verdict_e::drop);
  EXPECT_EQ(judge(408, true), verdict_e::keep_going);
  // a limit, a service that cannot store it yet, no answer, and the answers a proxy or an older service gives for other reasons: the report stays
  for (const long status : { 0, 429, 500, 502, 503, 504, 401, 403, 404, 405, 410, 421, 301, 302, 418, 451 }) EXPECT_EQ(judge(status, true), verdict_e::wait) << status;
}

TEST(CrashUploadPolicy, TheWaitIsRetryAfterOrFifteenMinutesDoublingToFourHoursAndSpreadByAQuarterAtMost) {
  using crash::upload_policy::wait_seconds;
  EXPECT_EQ(wait_seconds(1, 0, 0.0), 15 * 60);
  EXPECT_EQ(wait_seconds(2, 0, 0.0), 30 * 60);
  EXPECT_EQ(wait_seconds(3, 0, 0.0), 60 * 60);
  EXPECT_EQ(wait_seconds(6, 0, 0.0), 4 * 3600);
  EXPECT_EQ(wait_seconds(40, 0, 0.0), 4 * 3600);
  EXPECT_EQ(wait_seconds(1, 120, 0.0), 120);  // the service's word, not the 15 minutes
  EXPECT_EQ(wait_seconds(1, 5, 0.0), 30);  // never shorter than 30 s
  EXPECT_EQ(wait_seconds(1, 999999, 0.0), 4 * 3600);
  EXPECT_EQ(wait_seconds(1, 900, 0.5), 1013);  // an eighth more, rounded
  std::set<long> waits;
  for (int i = 0; i < 100; ++i) {
    const auto wait = wait_seconds(2, 0, i / 100.0);
    EXPECT_GE(wait, 30 * 60);
    EXPECT_LE(wait, 30 * 60 + 30 * 60 / 4);
    waits.insert(wait);
  }
  EXPECT_GT(waits.size(), 50u) << "computers that failed together come back at different moments";
}

TEST(CrashUpload, TheWaitAfterAFailureIsSpreadByTheRandomShare) {
  mock_server_t server;
  const auto config = scratch("spread");
  write_pending(config, "a", false);
  server.status = 503;
  server.retry_after = 60;
  crash::set_upload_jitter_source([] { return 0.5; });
  crash::upload_pending_once(config, server.url(), "x");
  EXPECT_EQ(crash::upload_wait_hint_s(), 68);  // 60 s and an eighth, rounded
  crash::set_upload_jitter_source([] { return 0.0; });
  crash::upload_pending_once(config, server.url(), "x");
  EXPECT_EQ(crash::upload_wait_hint_s(), 60);
  crash::set_upload_jitter_source({});
  fs::remove_all(config);
}

TEST(CrashUpload, AReportStaysWhenAProxyOrAnOlderServiceAnswersForOtherReasons) {
  mock_server_t server;
  const auto config = scratch("proxy");
  write_pending(config, "a", true);
  for (int status : { 401, 403, 404, 405, 410, 421, 302, 418 }) {
    server.status = status;
    EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 0) << status;
    EXPECT_EQ(crash::list_pending(crash::pending_dir(config)).size(), 1u) << status;
  }
  // the service is reachable again: it goes
  server.status = 200;
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 1);
  EXPECT_TRUE(crash::list_pending(crash::pending_dir(config)).empty());
  fs::remove_all(config);
}

TEST(CrashUpload, AnAnswerThatTheBodyIsNeverAcceptedDropsItForEveryRefusalOfTheContract) {
  mock_server_t server;
  const auto config = scratch("refused");
  for (int status : { 400, 415, 422 }) {
    write_pending(config, "a", true);
    server.status = status;
    EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 1) << status;
    EXPECT_TRUE(crash::list_pending(crash::pending_dir(config)).empty()) << status;
  }
  fs::remove_all(config);
}

TEST(CrashUpload, ADumpThatIsTooLargeIsLeftOutAndTheReportGoesAgainWithoutIt) {
  mock_server_t server;
  const auto config = scratch("toolarge");
  write_pending(config, "a", true);
  server.status = 413;
  // the service refuses even the small request: the report is dropped after the second try
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 1);
  ASSERT_EQ(server.bodies.size(), 2u);
  EXPECT_NE(server.bodies[0].find("name=\"dump\""), std::string::npos);
  EXPECT_EQ(server.bodies[1].find("name=\"dump\""), std::string::npos) << "the second request has no dump";
  EXPECT_TRUE(crash::list_pending(crash::pending_dir(config)).empty());
  fs::remove_all(config);
}

TEST(CrashUpload, TheSecondRequestWithoutTheDumpIsDeliveredWhenTheServiceTakesIt) {
  mock_server_t server;
  const auto config = scratch("toolarge2");
  write_pending(config, "a", true);
  server.script = { 413, 200 };  // too large with the dump, taken without it
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 1);
  ASSERT_EQ(server.bodies.size(), 2u);
  EXPECT_NE(server.bodies[0].find("name=\"dump\""), std::string::npos);
  EXPECT_EQ(server.bodies[1].find("name=\"dump\""), std::string::npos);
  EXPECT_TRUE(crash::list_pending(crash::pending_dir(config)).empty());
  fs::remove_all(config);
}

TEST(CrashPending, TheByteCapDropsTheOldestAndTheNewestReportAlwaysStays) {
  const auto config = scratch("bytes");
  for (const char *name : { "a", "b", "c", "d" }) {
    write_pending(config, name, true);
    // older reports have older times
  }
  const auto dir = crash::pending_dir(config);
  std::error_code ec;
  int age = 40;
  for (const char *name : { "a", "b", "c", "d" }) {
    fs::last_write_time(dir / (std::string(name) + ".json"), fs::file_time_type::clock::now() - std::chrono::minutes(age), ec);
    age -= 10;
  }
  const auto one = crash::list_pending(dir).front().bytes;
  ASSERT_GT(one, 0u);
  // room for two and a half: the two oldest go
  EXPECT_EQ(crash::trim_pending(dir, crash::max_pending, 30, one * 5 / 2), 2u);
  const auto left = crash::list_pending(dir);
  ASSERT_EQ(left.size(), 2u);
  EXPECT_EQ(left.back().meta.stem().string(), "d");
  // a cap below one report still keeps the newest
  EXPECT_EQ(crash::trim_pending(dir, crash::max_pending, 30, 1), 1u);
  ASSERT_EQ(crash::list_pending(dir).size(), 1u);
  EXPECT_EQ(crash::list_pending(dir).front().meta.stem().string(), "d");
  fs::remove_all(config);
}

TEST(CrashUpload, WhatWaitsOnDiskSurvivesARestartAndIsSentByTheNextRun) {
  // the uploader keeps nothing but files: a new process (a new call after the failure) finds the same reports and sends them
  mock_server_t server;
  const auto config = scratch("restart");
  write_pending(config, "a", true);
  write_pending(config, "b", false);
  server.status = 503;
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 0);
  EXPECT_EQ(crash::list_pending(crash::pending_dir(config)).size(), 2u);
  server.status = 200;
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 2);
  EXPECT_TRUE(crash::list_pending(crash::pending_dir(config)).empty());
  fs::remove_all(config);
}

TEST(CrashUpload, TheSessionLogOfAFeedbackReportGoesToUserLogAndComesBackAsARef) {
  mock_server_t server;
  const auto config = scratch("userlog");
  put(config / "sunshine.log", "[2026-11-14 22:00:00.000]: Info: mail a.b@example.co.uk sent\n[2026-11-14 22:00:01.000]: Error: frozen\n");
  EXPECT_FALSE(crash::upload_session_log_from(config, server.url(), "11111111-2222-4333-8444-555555555555", "2026.1115.1").has_value());  // the mock answers 200 without a ref: no ref, no log_ref
  ASSERT_EQ(server.targets.size(), 1u);
  EXPECT_EQ(server.targets[0], "/v1/user-log");
  EXPECT_NE(server.bodies[0].find("<email>"), std::string::npos);
  EXPECT_EQ(server.bodies[0].find("example.co.uk"), std::string::npos);
  // the service stores the log and answers with its reference: that reference comes back to the caller (it goes into the feedback report)
  {
    std::lock_guard lock(server.mutex);
    server.reply_body = R"({"ok":true,"log":"stored","ref":"logs-2026-11/abc123.log.gz"})";
  }
  const auto ref = crash::upload_session_log_from(config, server.url(), "11111111-2222-4333-8444-555555555555", "2026.1115.1");
  ASSERT_TRUE(ref.has_value());
  EXPECT_EQ(*ref, "logs-2026-11/abc123.log.gz");
  // a 2xx that does not say "stored" has no reference
  {
    std::lock_guard lock(server.mutex);
    server.reply_body = R"({"ok":true,"log":"skipped","ref":"x"})";
  }
  EXPECT_FALSE(crash::upload_session_log_from(config, server.url(), "x", "v").has_value());
  server.status = 404;  // a service without the endpoint: the report goes without a log, silently
  EXPECT_FALSE(crash::upload_session_log_from(config, server.url(), "x", "v").has_value());
  fs::remove_all(config);
}

TEST(CrashMeta, CarriesWhatTheHostWasDoingAsFixedWordsAndNumbers) {
  crash::frame_t frames[] = { frame("sunshine.exe", 0x10) };
  crash::meta_t meta;
  meta.frames = frames;
  meta.frame_count = 1;
  char buffer[crash::meta_buffer_size];
  crash::context::state().streaming = -1;
  auto idle = std::string(buffer, crash::render_meta(meta, buffer, sizeof(buffer)));
  EXPECT_EQ(idle.find("\"ctx\""), std::string::npos);  // unknown: nothing is written
  crash::context::stream_started("hevc", 3840, 2160, 120, true, "altp", "moonlight-macos");
  const auto streaming = json::parse(std::string(buffer, crash::render_meta(meta, buffer, sizeof(buffer))));
  EXPECT_EQ(streaming["ctx"], json::parse(R"({"streaming":true,"codec":"hevc","width":3840,"height":2160,"fps":120,"transport":"altp","client_family":"moonlight-macos","hdr":true})"));
  crash::context::stream_ended();
  const auto ended = json::parse(std::string(buffer, crash::render_meta(meta, buffer, sizeof(buffer))));
  EXPECT_EQ(ended["ctx"], json::parse(R"({"streaming":false})"));
  crash::context::state().streaming = -1;
}

TEST(CrashMeta, ANameForTheStartUpPhaseIsAFixedWordAndOnlyWhileTheHostIsStarting) {
  crash::frame_t frames[] = { frame("sunshine.exe", 0x10) };
  crash::meta_t meta;
  meta.frames = frames;
  meta.frame_count = 1;
  char buffer[crash::meta_buffer_size];
  auto render = [&] { return json::parse(std::string(buffer, crash::render_meta(meta, buffer, sizeof(buffer)))); };
  crash::context::state().streaming = -1;
  crash::context::set_phase(crash::context::phase_e::display_restore);
  EXPECT_EQ(render()["ctx"], json::parse(R"({"phase":"display-restore"})"));  // a start has no stream: the phase alone is a context
  crash::context::stream_started("h264", 1920, 1080, 60, false, "enet", "moonlight-pc");
  EXPECT_EQ(render()["ctx"]["phase"], "display-restore");
  EXPECT_EQ(render()["ctx"]["streaming"], true);
  crash::context::stream_ended();
  crash::context::set_phase(crash::context::phase_e::none);
  EXPECT_EQ(render()["ctx"], json::parse(R"({"streaming":false})"));
  crash::context::state().streaming = -1;
  EXPECT_FALSE(render().contains("ctx"));
  // every phase has one lowercase ASCII word; nothing outside the list has one
  for (int i = 1; i <= static_cast<int>(crash::context::phase_e::tray); ++i) {
    const char *word = crash::context::phase_word(i);
    ASSERT_NE(word, nullptr) << i;
    for (const char *c = word; *c; ++c) EXPECT_TRUE((*c >= 'a' && *c <= 'z') || *c == '-') << word;
  }
  EXPECT_EQ(crash::context::phase_word(0), nullptr);
  EXPECT_EQ(crash::context::phase_word(static_cast<int>(crash::context::phase_e::tray) + 1), nullptr);
}

// A feedback reports uploads share one deadline inside the pages 45 s wait: each gets at most 20 s of what is left, none starts with less than 2 s, and without a budget the fixed 20 s holds.
TEST(CrashFeedbackBudget, UploadsShareOneDeadline) {
  crash::end_feedback_budget();
  EXPECT_EQ(crash::feedback_upload_timeout_for_tests(), 20);
  crash::begin_feedback_budget(std::chrono::seconds { 35 });
  EXPECT_EQ(crash::feedback_upload_timeout_for_tests(), 20);
  crash::begin_feedback_budget(std::chrono::seconds { 10 });
  const auto left = crash::feedback_upload_timeout_for_tests();
  EXPECT_GE(left, 8);
  EXPECT_LE(left, 10);
  crash::begin_feedback_budget(std::chrono::seconds { 1 });
  EXPECT_EQ(crash::feedback_upload_timeout_for_tests(), 0);
  crash::end_feedback_budget();
  EXPECT_EQ(crash::feedback_upload_timeout_for_tests(), 20);
}

// The log of a feedback report is read, anonymised and packed inside the same deadline: with too little left it is not read, a slow read or a slow pack ends in "unreachable" without an upload,
// and the whole call stays inside the budget.
namespace {
  struct stage_probe_t {
    std::atomic<int> collects { 0 };
    std::atomic<int> packs { 0 };
    std::atomic<std::size_t> read_cap { 0 };
  };

  crash::log_stages_t
  stages_of(stage_probe_t &probe, std::chrono::milliseconds collect_takes, bool pack_waits_for_deadline) {
    crash::log_stages_t stages;
    stages.collect = [&probe, collect_takes](const fs::path &file, std::size_t max_bytes) {
      ++probe.collects;
      probe.read_cap = max_bytes;
      std::this_thread::sleep_for(collect_takes);
      std::ifstream in(file, std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(in), {});
    };
    stages.pack = [&probe, pack_waits_for_deadline](std::string_view text, const std::function<bool()> &expired) -> std::optional<crash::logfile::packed_t> {
      ++probe.packs;
      if (pack_waits_for_deadline)  // a packing that is slow but looks at the deadline, as the real one does
        while (!expired || !expired()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
      return crash::logfile::pack(text, crash::logfile::max_gz_bytes, expired);
    };
    return stages;
  }
}  // namespace

TEST(CrashFeedbackBudget, ALogThatCannotBeReadInTimeIsNotReadAtAll) {
  mock_server_t server;
  const auto config = scratch("logbudget-none");
  put(config / "sunshine.log", "[2026-11-24 10:00:00.000]: Info: started\n");
  stage_probe_t probe;
  const auto stages = stages_of(probe, std::chrono::milliseconds(0), false);
  crash::begin_feedback_budget(std::chrono::seconds { 5 });  // the upload keeps 5 s of it: nothing is left for the work in front of it
  const auto begun = std::chrono::steady_clock::now();
  EXPECT_FALSE(crash::upload_session_log_from(config, server.url(), "x", "v", &stages).has_value());
  crash::end_feedback_budget();
  EXPECT_LT(std::chrono::steady_clock::now() - begun, std::chrono::seconds(1));
  EXPECT_EQ(probe.collects.load(), 0);
  EXPECT_EQ(probe.packs.load(), 0);
  EXPECT_STREQ(crash::session_log_state(), "unreachable");
  EXPECT_TRUE(server.targets.empty());
  fs::remove_all(config);
}

TEST(CrashFeedbackBudget, ASlowCollectionIsNotFollowedByAPackingOrAnUploadAndStaysInsideTheBudget) {
  mock_server_t server;
  const auto config = scratch("logbudget-slow-collect");
  put(config / "sunshine.log", "[2026-11-24 10:00:00.000]: Info: started\n");
  stage_probe_t probe;
  const auto stages = stages_of(probe, std::chrono::milliseconds(2400), false);  // the work gets 2 s of the 7: the read overruns it
  crash::begin_feedback_budget(std::chrono::seconds { 7 });
  const auto begun = std::chrono::steady_clock::now();
  EXPECT_FALSE(crash::upload_session_log_from(config, server.url(), "x", "v", &stages).has_value());
  const auto took = std::chrono::steady_clock::now() - begun;
  crash::end_feedback_budget();
  EXPECT_LT(took, std::chrono::seconds(7));
  EXPECT_EQ(probe.collects.load(), 1);
  EXPECT_EQ(probe.packs.load(), 0);
  EXPECT_EQ(probe.read_cap.load(), crash::logfile::bounded_read_bytes);
  EXPECT_STREQ(crash::session_log_state(), "unreachable");
  EXPECT_TRUE(server.targets.empty());
  fs::remove_all(config);
}

TEST(CrashFeedbackBudget, ASlowPackingStopsWhereItsShareEndsAndTheReportGoesWithoutTheLog) {
  mock_server_t server;
  const auto config = scratch("logbudget-slow-pack");
  put(config / "sunshine.log", "[2026-11-24 10:00:00.000]: Info: started\n");
  stage_probe_t probe;
  const auto stages = stages_of(probe, std::chrono::milliseconds(0), true);
  crash::begin_feedback_budget(std::chrono::seconds { 7 });
  const auto begun = std::chrono::steady_clock::now();
  EXPECT_FALSE(crash::upload_session_log_from(config, server.url(), "x", "v", &stages).has_value());
  const auto took = std::chrono::steady_clock::now() - begun;
  crash::end_feedback_budget();
  EXPECT_GE(took, std::chrono::milliseconds(1500));  // it waited for its own share, not for the whole budget
  EXPECT_LT(took, std::chrono::seconds(4));
  EXPECT_EQ(probe.packs.load(), 1);
  EXPECT_STREQ(crash::session_log_state(), "unreachable");
  EXPECT_TRUE(server.targets.empty());
  fs::remove_all(config);
}

TEST(CrashFeedbackBudget, APackingThatSucceedsAfterItsShareIsNotUploaded) {
  mock_server_t server;
  const auto config = scratch("logbudget-late-pack");
  put(config / "sunshine.log", "[2026-11-24 10:00:00.000]: Info: started\n");
  stage_probe_t probe;
  auto stages = stages_of(probe, std::chrono::milliseconds(0), false);
  // a packing that ignores the deadline and still returns a result after its share ended
  stages.pack = [&probe](std::string_view text, const std::function<bool()> &expired) -> std::optional<crash::logfile::packed_t> {
    ++probe.packs;
    while (!expired || !expired()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return crash::logfile::pack(text, crash::logfile::max_gz_bytes, {});
  };
  crash::begin_feedback_budget(std::chrono::seconds { 7 });
  EXPECT_FALSE(crash::upload_session_log_from(config, server.url(), "x", "v", &stages).has_value());
  crash::end_feedback_budget();
  EXPECT_EQ(probe.packs.load(), 1);
  EXPECT_STREQ(crash::session_log_state(), "unreachable");
  EXPECT_TRUE(server.targets.empty());
  fs::remove_all(config);
}

TEST(CrashFeedbackBudget, InTimeTheLogGoesUpWithABoundedReadAndWithoutABudgetTheReadIsNotBounded) {
  mock_server_t server;
  {
    std::lock_guard lock(server.mutex);
    server.reply_body = R"({"ok":true,"log":"stored","ref":"logs-2026-11/in-time.log.gz"})";
  }
  const auto config = scratch("logbudget-fast");
  put(config / "sunshine.log", "[2026-11-24 10:00:00.000]: Info: started\n[2026-11-24 10:00:01.000]: Error: frozen\n");
  stage_probe_t probe;
  const auto stages = stages_of(probe, std::chrono::milliseconds(0), false);
  crash::begin_feedback_budget(std::chrono::seconds { 35 });
  const auto ref = crash::upload_session_log_from(config, server.url(), "x", "v", &stages);
  crash::end_feedback_budget();
  ASSERT_TRUE(ref.has_value());
  EXPECT_EQ(*ref, "logs-2026-11/in-time.log.gz");
  EXPECT_EQ(probe.read_cap.load(), crash::logfile::bounded_read_bytes);
  EXPECT_STREQ(crash::session_log_state(), "uploaded");
  EXPECT_TRUE(crash::upload_session_log_from(config, server.url(), "x", "v", &stages).has_value());  // no budget: the old behaviour
  EXPECT_EQ(probe.read_cap.load(), crash::logfile::max_read_bytes);
  fs::remove_all(config);
}

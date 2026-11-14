/**
 * @file tests/unit/test_crash.cpp
 * @brief Crash reports: signature, meta JSON, consent, the pending folder and the upload against a local mock (telemetry v1 /v1/crash).
 */
#include <src/crash/crash.h>
#include <src/crash/crash_core.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
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
    auto dir = fs::temp_directory_path() / (std::string("crash-test-") + name + "-" + crash::new_uuid_v4().substr(0, 8));
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
    std::atomic<bool> stop { false };

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
          const std::string reply = "HTTP/1.1 " + std::to_string(status.load()) + " X\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
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
  write_pending(const fs::path &config, const std::string &name, bool with_dump, const char *module = "sunshine.exe", std::uint64_t offset = 0x1234) {
    crash::frame_t frames[] = { frame(module, offset), frame("sunshine.exe", 0x77) };
    crash::meta_t meta;
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
  fs::remove_all(config);
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
  fs::remove_all(config);
}

TEST(CrashIdentity, CreatedOnceAndStable) {
  const auto config = scratch("id");
  EXPECT_EQ(crash::read_or_create_install_id(config, false), "");  // reading never creates
  const auto id = crash::read_or_create_install_id(config, true);
  ASSERT_EQ(id.size(), 36u);
  EXPECT_EQ(id[14], '4');  // version 4
  EXPECT_EQ(crash::read_or_create_install_id(config, true), id);
  EXPECT_EQ(crash::read_or_create_install_id(config, false), id);
  fs::remove_all(config);
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
  fs::remove_all(config);
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
  fs::remove_all(config);
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
  fs::remove_all(config);
}

TEST(CrashUpload, DropsWhatTheServerRejectsForGood) {
  mock_server_t server;
  const auto config = scratch("reject");
  write_pending(config, "a", true);
  server.status = 400;
  EXPECT_EQ(crash::upload_pending_once(config, server.url(), "x"), 1);
  EXPECT_TRUE(crash::list_pending(crash::pending_dir(config)).empty());
  fs::remove_all(config);
}

TEST(CrashUpload, NoServerLeavesTheReportAndReturnsAtOnce) {
  const auto config = scratch("down");
  write_pending(config, "a", true);
  const auto began = std::chrono::steady_clock::now();
  EXPECT_EQ(crash::upload_pending_once(config, "http://127.0.0.1:1", "x"), 0);  // nothing listens on port 1
  EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds(12));
  EXPECT_EQ(crash::list_pending(crash::pending_dir(config)).size(), 1u);
  fs::remove_all(config);
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
  fs::remove_all(config);
}

TEST(CrashUpload, CapsWhatIsOnDisk) {
  mock_server_t server;
  server.status = 503;
  const auto config = scratch("cap20");
  for (int i = 0; i < 24; ++i) write_pending(config, "r" + std::to_string(100 + i), false);
  crash::upload_pending_once(config, server.url(), "x");
  EXPECT_EQ(crash::list_pending(crash::pending_dir(config)).size(), crash::max_pending);
  fs::remove_all(config);
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
  fs::remove_all(config);
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
  fs::remove_all(config);
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

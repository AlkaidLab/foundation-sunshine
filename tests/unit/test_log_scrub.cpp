/**
 * @file tests/unit/test_log_scrub.cpp
 * @brief The anonymisation of a session log (src/crash/log_scrub.cpp) against the shared vectors (tests/data/log_scrub_vectors.json).
 */
#include <src/crash/log_scrub.h>

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using crash::logscrub::scrub;

TEST(LogScrubVectors, EveryVectorHolds) {
  const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "data" / "log_scrub_vectors.json";
  std::ifstream in(path);
  ASSERT_TRUE(in.good()) << path;
  const auto doc = nlohmann::json::parse(in);
  ASSERT_EQ(doc["version"], 1);
  ASSERT_GE(doc["vectors"].size(), 40u);
  for (const auto &v : doc["vectors"]) {
    const std::string name = v["name"];
    const auto out = scrub(v["input"].get<std::string>()).text;
    for (const auto &f : v["forbidden"]) EXPECT_EQ(out.find(f.get<std::string>()), std::string::npos) << name << " leaked \"" << f.get<std::string>() << "\" -> " << out;
    for (const auto &r : v["required"]) EXPECT_NE(out.find(r.get<std::string>()), std::string::npos) << name << " lost \"" << r.get<std::string>() << "\" -> " << out;
  }
}

TEST(LogScrubPlaceholders, StablePerValueAndPerLog) {
  EXPECT_EQ(scrub("client 10.0.0.5 up\nclient 10.0.0.9 up\nclient 10.0.0.5 down").text, "client <ip#1> up\nclient <ip#2> up\nclient <ip#1> down");
  EXPECT_EQ(scrub("peer 172.16.4.4").text, "peer <ip#1>");
  EXPECT_EQ(scrub("a 10.1.1.1 b fe80::1:2:3:4 c aa:bb:cc:dd:ee:ff d aa-bb-cc-dd-ee-ff").text, "a <ip#1> b <ip#2> c <mac#1> d <mac#1>");
}

TEST(LogScrubPlaceholders, AScrubbedLogIsStable) {
  std::string raw;
  for (int i = 0; i < 300; ++i) raw += "[2026-11-14 21:03:11.482]: Info: frame " + std::to_string(i) + ", client 192.168.1." + std::to_string(i % 200) + " C:\\Users\\Sky\\x.log\n";
  const auto once = scrub(raw).text;
  EXPECT_EQ(scrub(once).text, once);
}

TEST(LogScrubBounds, ACutInsideAKeyBlockDropsTheRestOfTheBlock) {
  std::string raw = "Info: start\n-----BEGIN PRIVATE KEY-----\n";
  for (int i = 0; i < 60; ++i) raw += "ABCDEFGHijklmnop" + std::to_string(i) + "\n";
  raw += "-----END PRIVATE KEY-----\nInfo: after the key\n";
  const auto r = scrub(raw, raw.size() - 400);  // the cut falls in the body
  EXPECT_TRUE(r.truncated);
  EXPECT_EQ(r.text.find("ABCDEFGH"), std::string::npos) << r.text;
  EXPECT_NE(r.text.find("Info: after the key"), std::string::npos);
}

TEST(LogScrubBounds, TailLineCutAndUtf8) {
  std::string raw;
  for (int i = 0; i < 1000; ++i) raw += "line " + std::to_string(1000 + i) + " " + std::string(40, 'x') + "\n";
  const auto r = scrub(raw, 5000);
  EXPECT_TRUE(r.truncated);
  EXPECT_LE(r.text.size(), 5000u);
  EXPECT_EQ(r.text.rfind("line ", 0), 0u);
  const auto cut_line = scrub("a\x07" "b " + std::string(2000, 'z'), 2'000'000, 100).text;
  EXPECT_LE(cut_line.size(), 102u);  // 99 bytes and the ellipsis (3 bytes in UTF-8)
  EXPECT_EQ(cut_line.substr(cut_line.size() - 3), "\xE2\x80\xA6");
  EXPECT_EQ(cut_line.find('\x07'), std::string::npos);
  // invalid UTF-8 becomes U+FFFD, a cut never splits a multi-byte character
  EXPECT_EQ(crash::logscrub::sanitize_utf8("ab\xC3(cd"), "ab\xEF\xBF\xBD(cd");
  const auto cut = scrub(std::string(98, 'a') + "\xE4\xB8\xAD\xE6\x96\x87\xE6\x96\x87", 2'000'000, 100).text;
  EXPECT_EQ(crash::logscrub::sanitize_utf8(cut), cut);
}

// ---- the packed log: gzip, head and tail, the pick by time ----------------------------------------------------------------------------
#include <src/crash/crash_log.h>

#include <chrono>
#include <zlib.h>

namespace {
  std::string
  gunzip(const std::string &gz) {
    z_stream zs {};
    EXPECT_EQ(inflateInit2(&zs, 15 + 16), Z_OK);
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(gz.data()));
    zs.avail_in = (uInt) gz.size();
    std::string out;
    char buf[1 << 16];
    int rc;
    do {
      zs.next_out = reinterpret_cast<Bytef *>(buf);
      zs.avail_out = sizeof(buf);
      rc = inflate(&zs, Z_NO_FLUSH);
      out.append(buf, sizeof(buf) - zs.avail_out);
    } while (rc == Z_OK);
    inflateEnd(&zs);
    EXPECT_EQ(rc, Z_STREAM_END) << "the gzip stream is complete and its checksum holds";
    return out;
  }
}  // namespace

TEST(CrashLogPack, GzipRoundTripAndTheEdgesAreTheStartAndTheEndOfTheSameText) {
  std::string raw;
  for (int i = 0; i < 6000; ++i) raw += "[2026-11-14 21:03:11.482]: Info: frame " + std::to_string(i) + " client 192.168." + std::to_string(i % 200) + ".7 \xE4\xB8\xAD\xE6\x96\x87\n";
  const auto p = crash::logfile::pack(raw);
  ASSERT_TRUE(p);
  const auto text = gunzip(p->gz);
  EXPECT_EQ(text.find("192.168."), std::string::npos);  // anonymised before it was compressed
  EXPECT_LE(p->head.size(), crash::logfile::edge_bytes);
  EXPECT_EQ(text.rfind(p->head, 0), 0u);  // log_head is the start of the gzip file: the service checks exactly this
  EXPECT_LE(p->tail.size(), crash::logfile::edge_bytes);
  EXPECT_EQ(text.compare(text.size() - p->tail.size(), p->tail.size(), p->tail), 0);
  EXPECT_EQ(p->lines, 6000u);
  EXPECT_FALSE(p->truncated);
  EXPECT_EQ(crash::logscrub::sanitize_utf8(p->head), p->head);  // whole characters
  EXPECT_EQ(crash::logscrub::sanitize_utf8(p->tail), p->tail);
}

TEST(CrashLogPack, OverTheLimitTheOldestLinesGoFirst) {
  std::string raw;
  unsigned x = 12345;
  for (int i = 0; i < 40000; ++i) {
    x = x * 1664525u + 1013904223u;
    raw += "line " + std::to_string(i) + " token " + std::to_string(x) + " " + std::to_string(x >> 7) + "\n";
  }
  const auto p = crash::logfile::pack(raw, 100 * 1024);
  ASSERT_TRUE(p);
  EXPECT_LE(p->gz.size(), 100u * 1024);
  EXPECT_TRUE(p->truncated);
  EXPECT_NE(gunzip(p->gz).find("line 39999 "), std::string::npos);  // the end is kept
  EXPECT_EQ(gunzip(p->gz).find("line 0 "), std::string::npos);
  EXPECT_FALSE(crash::logfile::pack("  \n\n"));
}

TEST(CrashLogPick, TheFileClosestToTheReportWins) {
  namespace fs = std::filesystem;
  const auto dir = fs::temp_directory_path() / ("crash-log-pick-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(dir);
  const auto write = [&](const char *name, const std::string &text, std::chrono::seconds age) {
    std::ofstream(dir / name, std::ios::binary) << text;
    fs::last_write_time(dir / name, fs::file_time_type::clock::now() - age);
  };
  write("sunshine.log", "the new run\n", std::chrono::seconds(5));  // started after the crash
  write("sunshine.log.1", "the run that crashed\n", std::chrono::minutes(30));  // last written when it crashed
  write("sunshine.log.2", "an older run\n", std::chrono::hours(5));
  const auto crash_time = fs::file_time_type::clock::now() - std::chrono::minutes(30);
  const auto text = crash::logfile::collect(dir / "sunshine.log", crash_time);
  EXPECT_NE(text.find("the run that crashed"), std::string::npos);
  EXPECT_EQ(text.find("the new run"), std::string::npos);
  EXPECT_NE(text.find("an older run"), std::string::npos);  // a very short log: the end of the run before it comes first
  EXPECT_EQ(crash::logfile::collect(dir / "sunshine.log", fs::file_time_type::clock::now() - std::chrono::hours(48)), "");
  EXPECT_NE(crash::logfile::collect_current(dir / "sunshine.log").find("the new run"), std::string::npos);
  // the run that crashed wrote nothing for a long time before it crashed; the next run wrote shortly after the report: the log written after the report is not the one
  const auto t0 = fs::file_time_type::clock::now();
  fs::last_write_time(dir / "sunshine.log.1", t0 - std::chrono::minutes(37));  // 7 minutes before the report: still the closest to it, within the window
  fs::last_write_time(dir / "sunshine.log", t0 - std::chrono::minutes(29));  // a minute after the report (at -30 min), so nearer in time, but written after it
  const auto older = crash::logfile::collect(dir / "sunshine.log", crash_time);
  EXPECT_NE(older.find("the run that crashed"), std::string::npos);
  EXPECT_EQ(older.find("the new run"), std::string::npos);
  fs::remove_all(dir);
}

// ---- the machine description keeps to the service's format rules -------------------------------------------------------------------------------
#include <src/crash/crash_env.h>

TEST(CrashEnv, ModelsAndVersionsKeepToTheFormatRulesOfTheService) {
  using namespace crash::env;
  EXPECT_EQ(sanitize_model("NVIDIA GeForce RTX 4090"), "NVIDIA GeForce RTX 4090");
  EXPECT_EQ(sanitize_model("13th Gen Intel(R) Core(TM) i9-13900K"), "13th Gen Intel(R) Core(TM) i9-13900K");
  EXPECT_EQ(sanitize_model("  AMD\tRadeon™  RX 7900 XTX  "), "AMD Radeon RX 7900 XTX");
  EXPECT_EQ(sanitize_model("C:\\Users\\bob\\x"), "C Users bob x");
  EXPECT_LE(sanitize_model(std::string(200, 'a')).size(), 64u);
  EXPECT_EQ(sanitize_model("***"), "");
  EXPECT_EQ(sanitize_version("32.0.15.6094"), "32.0.15.6094");
  EXPECT_EQ(sanitize_version("a b;c"), "abc");
  EXPECT_EQ(sanitize_version("..."), "");
  EXPECT_EQ(gpu_vendor_word("NVIDIA Corporation"), "nvidia");
  EXPECT_EQ(gpu_vendor_word("Advanced Micro Devices, Inc."), "amd");
  EXPECT_EQ(gpu_vendor_word("Intel(R) Arc(TM) A770"), "intel");
  EXPECT_EQ(gpu_vendor_word("Something Else"), "other");
}

#include "src/telemetry/error_report.h"
#include "src/telemetry/events.h"
#include "src/telemetry/service.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <regex>

using namespace telemetry;
using json = nlohmann::json;
namespace fs = std::filesystem;

TEST(Breadcrumbs, KeepTheLastFiftyAsFixedWordsWithTheSecondsBefore) {
  breadcrumbs_t ring;
  for (int i = 0; i < 60; ++i) ring.add(crumb_e::act, "tile." + std::to_string(i), 1000 * i);
  ring.add(crumb_e::set, "Encoder: NVENC!", 61'000);
  ring.add(crumb_e::nav, "", 61'000);  // nothing to record
  const auto shot = ring.snapshot(65'000);
  ASSERT_EQ(shot.size(), 50u);
  EXPECT_EQ(shot.back(), "4|set|encoder:_nvenc");
  EXPECT_EQ(shot.front(), "54|act|tile.11");  // the oldest ten are gone
  const std::regex shape(R"(^\d{1,5}\|(?:nav|act|set|stream|dev|state|net)\|[a-z0-9_.:+-]{1,48}$)");
  for (const auto &c : shot) EXPECT_TRUE(std::regex_match(c, shape)) << c;
}

TEST(Breadcrumbs, ATypedTextCannotBecomeALongOrOddWord) {
  EXPECT_EQ(crumb_word("My Password Is Hunter2!!"), "my_password_is_hunter2");
  EXPECT_EQ(crumb_word(std::string(100, 'a')).size(), 48u);
  EXPECT_EQ(crumb_word("名字"), "");
  EXPECT_EQ(crumb_word("a/b\\c"), "a_b_c");
}

TEST(ErrorLocation, IsRelativeToTheSourceTreeAndNeverAnAbsolutePath) {
  struct fake_t {
    const char *file;
    const char *fn;
  };
  // source_location cannot be built by hand: the function above gives this file's own, which must come out under tests/
  const auto here = location_of(std::source_location::current());
  EXPECT_TRUE(here.rfind("tests/unit/test_error_report.cpp:", 0) == 0) << here;
  EXPECT_EQ(here.find("home"), std::string::npos);
  EXPECT_EQ(here.find('\\'), std::string::npos);
  EXPECT_EQ(function_word("void __cdecl stream::session::foo(int, char *)"), "stream::session::foo");
  EXPECT_EQ(function_word("videoBroadcastThread(boost::asio::ip::udp::socket&)"), "videoBroadcastThread");
  EXPECT_EQ(function_word("auto telemetry::{lambda()#1}::operator()() const"), "telemetry::lambda");
}

TEST(ErrorFrames, OnlyFixedCharactersPass) {
  EXPECT_EQ(clean_frame("sunshine.exe+0x1a2b3c"), "sunshine.exe+0x1a2b3c");
  EXPECT_EQ(clean_frame("src/stream.cpp:3912:videoBroadcastThread"), "src/stream.cpp:3912:videoBroadcastThread");
  EXPECT_EQ(clean_frame("C:\\Users\\Eve\\x.cpp:3"), "");
  EXPECT_EQ(clean_frame("two words"), "");
  EXPECT_EQ(clean_frame(std::string(121, 'a')), "");
  EXPECT_EQ(clean_frame("名.cpp:1"), "");
}

TEST(ErrorFingerprint, IsTheSameForTheSameErrorOnAnotherBuildAndDifferentOtherwise) {
  const auto a = fingerprint_of("host", "E_X", { "src/video.cpp:4821:encoder_init", "src/video.cpp:5120:probe", "sunshine.exe+0x10" });
  const auto b = fingerprint_of("host", "E_X", { "src/video.cpp:4900:encoder_init", "src/video.cpp:5300:probe", "sunshine.exe+0x99" });  // lines moved
  EXPECT_EQ(a, b);
  EXPECT_NE(a, fingerprint_of("host", "E_Y", { "src/video.cpp:4821:encoder_init" }));
  EXPECT_NE(a, fingerprint_of("panel", "E_X", { "src/video.cpp:4821:encoder_init", "src/video.cpp:5120:probe" }));
  EXPECT_NE(a, fingerprint_of("host", "E_X", { "src/other.cpp:4821:encoder_init", "src/video.cpp:5120:probe" }));
  EXPECT_EQ(a.size(), 16u);
  const auto native = fingerprint_of("host", "E_X", { "sunshine.exe+0x10", "nvEncodeAPI64.dll+0x9f10" });
  EXPECT_EQ(native, fingerprint_of("host", "E_X", { "sunshine.exe+0x4444", "nvEncodeAPI64.dll+0x1" }));
}

TEST(ErrorReporter, TheFirstLeavesAtOnceTheRestIsCountedAndSentAsOneAggregate) {
  error_reporter_t reporter;
  error_input_t e;
  e.code = "E_ENCODER_INIT";
  e.loc = "src/video.cpp:4821:encoder_init";
  e.frames = { e.loc };
  const std::vector<std::string> crumbs { "30|act|tile.start", "3|stream|start" };
  const auto first = reporter.report(e, crumbs, 1000);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ((*first)["count"], 1);
  EXPECT_EQ((*first)["crumbs"].size(), 2u);
  EXPECT_EQ((*first)["code"], "E_ENCODER_INIT");  // case kept
  for (int i = 0; i < 9; ++i) EXPECT_FALSE(reporter.report(e, crumbs, 2000 + i).has_value());
  EXPECT_TRUE(reporter.flush(60'000).empty());  // not ten minutes yet
  const auto aggregate = reporter.flush(1000 + error_reporter_t::AGGREGATE_MS);
  ASSERT_EQ(aggregate.size(), 1u);
  EXPECT_EQ(aggregate[0]["count"], 9);
  EXPECT_EQ(aggregate[0]["fingerprint"], (*first)["fingerprint"]);
  EXPECT_TRUE(reporter.flush(1000 + 3 * error_reporter_t::AGGREGATE_MS).empty());  // nothing held any more
  error_input_t none;
  EXPECT_FALSE(reporter.report(none, {}, 5).has_value());  // no code, no report
}

TEST(ErrorReporter, NoMoreThanTwentyDifferentErrorsInAnHour) {
  error_reporter_t reporter;
  int sent = 0;
  for (int i = 0; i < 40; ++i) {
    error_input_t e;
    e.code = "E_" + std::to_string(i);
    e.loc = "src/a.cpp:" + std::to_string(i + 1) + ":f" + std::to_string(i);
    e.frames = { e.loc };
    if (reporter.report(e, {}, 1000 + i)) ++sent;
  }
  EXPECT_EQ(sent, error_reporter_t::MAX_PER_HOUR);
}

TEST(ErrorEvent, IsShapedAgainstTheAllowList) {
  json props {
    { "component", "host" },
    { "lang", "cpp" },
    { "fingerprint", "a1b2c3d4e5f60718" },
    { "code", "E_ENCODER_INIT" },
    { "severity", "error" },
    { "loc", "src/video.cpp:4821:encoder_init" },
    { "frames", json::array({ "src/video.cpp:4821:encoder_init", "sunshine.exe+0x1a2b3c" }) },
    { "crumbs", json::array({ "3|stream|start" }) },
    { "count", 4 },
    { "user_name", "Eve" },
  };
  const auto e = shape_event(kind_e::error, "error_report", props, 5);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->props["frames"].size(), 2u);
  EXPECT_FALSE(e->props.contains("user_name"));
  EXPECT_EQ(e->props["loc"], "src/video.cpp:4821:encoder_init");
  // a path in a frame, a free text in a breadcrumb: that prop is dropped, the event stays
  props["frames"] = json::array({ "C:\\Users\\Eve\\a.cpp:1" });
  props["crumbs"] = json::array({ "3|act|my password" });
  const auto bad = shape_event(kind_e::error, "error_report", props, 5);
  ASSERT_TRUE(bad);
  EXPECT_FALSE(bad->props.contains("frames"));
  EXPECT_FALSE(bad->props.contains("crumbs"));
  EXPECT_FALSE(shape_event(kind_e::error, "error_report", json { { "code", "E_X" } }, 5));  // component and fingerprint are required
}

TEST(ErrorService, ACrumbTrailGoesWithTheReportAndTheErrorChoiceGoverns) {
  const auto dir = fs::temp_directory_path() / ("er-" + std::to_string(::getpid()));
  fs::create_directories(dir);
  std::int64_t now = 1'800'000'000'000;
  consent_t consent;
  std::vector<json> posted;
  service_options_t options;
  options.dir = dir;
  service_deps_t deps;
  deps.now_ms = [&] { return now; };
  deps.consent = [&] { return consent; };
  deps.random01 = [] { return 0.0; };
  deps.post = [&](const std::string &, const std::string &body) {
    posted.push_back(json::parse(body));
    return 200;
  };
  service_t svc { options, deps };
  context_t c;
  c.version = "v2026.1114.1";
  c.channel = "stable";
  c.os = "windows";
  svc.set_context(c);
  svc.crumb(crumb_e::nav, "apps");
  now += 5000;
  svc.crumb(crumb_e::act, "tile.start_stream");
  now += 7000;
  error_input_t e;
  e.code = "E_ENCODER_INIT";
  e.loc = "src/video.cpp:4821:encoder_init";
  e.frames = { e.loc };
  svc.report_error(e);
  svc.flush(true);
  while (svc.pump() > 0) {}
  json event;
  for (const auto &b : posted)
    if (b.contains("events"))
      for (const auto &ev : b["events"])
        if (ev["name"] == "error_report") event = ev["props"];
  ASSERT_FALSE(event.is_null());
  EXPECT_EQ(event["crumbs"], json::array({ "12|nav|apps", "7|act|tile.start_stream" }));
  EXPECT_EQ(event["loc"], "src/video.cpp:4821:encoder_init");

  // error reports off: nothing leaves
  posted.clear();
  consent.errors = false;
  e.code = "E_OTHER";
  e.loc = "src/a.cpp:1:f";
  e.frames = { e.loc };
  svc.report_error(e);
  svc.flush(true);
  while (svc.pump() > 0) {}
  for (const auto &b : posted)
    if (b.contains("events"))
      for (const auto &ev : b["events"]) EXPECT_NE(ev["name"], "error_report");
  fs::remove_all(dir);
}

#if defined(__linux__) || defined(_WIN32)
TEST(ErrorFrames, TheStackOfTheCallingThreadIsModulePlusOffset) {
  const auto frames = capture_frames(0);
  ASSERT_FALSE(frames.empty());
  for (const auto &f : frames) EXPECT_FALSE(clean_frame(f).empty()) << f;
}
#endif

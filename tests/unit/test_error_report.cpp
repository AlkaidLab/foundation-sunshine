#include "src/telemetry/error_report.h"
#include "src/telemetry/events.h"
#include "src/telemetry/service.h"
#include "src/telemetry/wer_event.h"
#include "tools/installer_failure.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>

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
  for (const auto &b : posted) {
    if (b.contains("events")) {
      for (const auto &ev : b["events"]) {
        EXPECT_NE(ev["name"], "error_report");
      }
    }
  }
  fs::remove_all(dir);
}

TEST(ErrorService, AServerWaitMergesFatalErrorsOnlyAtTheSameSite) {
  struct temp_dir_t {
    fs::path path = fs::temp_directory_path() / ("fatal-sites-" + new_uuid_v4());
    ~temp_dir_t() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } dir;
  std::vector<json> posted;
  service_options_t options;
  options.dir = dir.path;
  service_deps_t deps;
  deps.now_ms = [] { return 1'800'000'000'000LL; };
  deps.consent = [] { return consent_t {}; };
  deps.post = [&](const std::string &, const std::string &body) {
    const bool first = posted.empty();
    posted.push_back(json::parse(body));
    post_result_t answer { 200 };
    if (first) answer.body = R"({"cfg":{"backoff_s":600}})";
    return answer;
  };
  service_t svc { options, deps };
  auto context = svc.context();
  context.version = "5.2.0.1";
  svc.set_context(context);
  ASSERT_TRUE(svc.emit(kind_e::usage, "app_start", json { { "cold", true } }));
  svc.flush();
  ASSERT_EQ(svc.pump(), 1);
  ASSERT_TRUE(svc.backing_off());

  for (const auto site : { "src/video.cpp:42:start", "src/video.cpp:43:stop", "src/video.cpp:42:start" }) {
    svc.note_log(5, "{} could not open encoder", "host", site);
    svc.flush();
  }
  ASSERT_EQ(svc.pump(), 1);
  ASSERT_EQ(posted.size(), 2u);
  json counts = json::object();
  std::set<std::string> codes;
  std::size_t errors = 0;
  for (const auto &event : posted.back().at("events")) {
    if (event.at("name") != "error") continue;
    ++errors;
    const auto &props = event.at("props");
    EXPECT_EQ(props.at("severity"), "fatal");
    codes.insert(props.at("code").get<std::string>());
    counts[props.at("site").get<std::string>()] = props.at("count");
  }
  EXPECT_EQ(errors, 2u);
  EXPECT_EQ(codes.size(), 1u);
  EXPECT_EQ(counts, (json { { "video.cpp:42", 2 }, { "video.cpp:43", 1 } }));
}

#if defined(__linux__) || defined(_WIN32)
TEST(ErrorFrames, TheStackOfTheCallingThreadIsModulePlusOffset) {
  const auto frames = capture_frames(0);
  ASSERT_FALSE(frames.empty());
  for (const auto &f : frames) EXPECT_FALSE(clean_frame(f).empty()) << f;
}
#endif

TEST(ErrorEvent, ALogTailIsBoundedAndScrubbedAgainByTheHost) {
  json props {
    { "component", "panel-shell" },
    { "lang", "rust" },
    { "fingerprint", "027fe2f9e2fdcceb" },
    { "code", "panic" },
    { "severity", "fatal" },
    { "log", json::array({ "[2026-11-15 10:00:00.000] ERROR clipboard: agent died", "peer 192.168.1.20 sent C:\\Users\\Eve\\a.txt token=abc123", "mail eve@example.com" }) },
  };
  const auto e = shape_event(kind_e::error, "error_report", props, 5);
  ASSERT_TRUE(e);
  ASSERT_EQ(e->props["log"].size(), 3u);
  const auto text = e->props["log"].dump();
  EXPECT_EQ(text.find("192.168"), std::string::npos) << text;
  EXPECT_EQ(text.find("Eve"), std::string::npos) << text;
  EXPECT_EQ(text.find("abc123"), std::string::npos) << text;
  EXPECT_EQ(text.find("example.com"), std::string::npos) << text;
  EXPECT_NE(text.find("agent died"), std::string::npos) << text;

  // too many lines, or a line that is not text: the prop is dropped, the event stays
  json many = json::array();
  for (int i = 0; i < 41; ++i) many.push_back("line");
  props["log"] = many;
  const auto too_many = shape_event(kind_e::error, "error_report", props, 5);
  ASSERT_TRUE(too_many);
  EXPECT_FALSE(too_many->props.contains("log"));
  props["log"] = json::array({ 7 });
  const auto not_text = shape_event(kind_e::error, "error_report", props, 5);
  ASSERT_TRUE(not_text);
  EXPECT_FALSE(not_text->props.contains("log"));

  // a long line is cut, control characters are gone
  props["log"] = json::array({ std::string(500, 'x') + "\x01\x02" });
  const auto cut = shape_event(kind_e::error, "error_report", props, 5);
  ASSERT_TRUE(cut);
  EXPECT_LE(cut->props["log"][0].get<std::string>().size(), 200u);
}

TEST(InstallerFailure, TheShellWritesAStepAndACodeWordAndTheHostReportsThem) {
  using installer_failure::step;
  EXPECT_EQ(installer_failure::code_word(step::engine, 101), "engine_exit_101");
  EXPECT_EQ(installer_failure::code_word(step::payload, 101), "payload_invalid");
  EXPECT_EQ(installer_failure::code_word(step::webview2, 5), "webview2_exit_5");
  EXPECT_EQ(installer_failure::result_fields(step::engine, 2), R"(,"failed_step":"engine","error_code":"engine_exit_2")");
  EXPECT_TRUE(installer_failure::is_failure_state("failed"));
  EXPECT_TRUE(installer_failure::is_failure_state("unknown"));
  EXPECT_FALSE(installer_failure::is_failure_state("success"));
  EXPECT_FALSE(installer_failure::is_failure_state("needs-reboot"));
  EXPECT_FALSE(installer_failure::is_failure_state("cancelled"));

  // what installer_shell.cpp writes for a failed engine, and what the host makes of it
  const std::string written = std::string { R"({"schema":1,"run":"20261115-1","state":"component-failed","result_code":101)" } + installer_failure::result_fields(step::engine, 101) + "}\n";
  const auto e = installer_failure_report(written);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->component, "installer");
  EXPECT_EQ(e->lang, "installer");
  EXPECT_EQ(e->code, "engine_exit_101");
  EXPECT_EQ(e->frames, std::vector<std::string>({ "installer/engine" }));
  EXPECT_EQ(fingerprint_of(e->component, e->code, e->frames), fingerprint_of("installer", "engine_exit_101", { "installer/engine" }));

  // no failure: a success, a restart request, a cancel, a file from an older shell, damaged text, a code word of another step, a word outside the fixed shapes
  EXPECT_FALSE(installer_failure_report(R"({"schema":1,"state":"success","result_code":0})"));
  EXPECT_FALSE(installer_failure_report(R"({"schema":1,"state":"needs-reboot","result_code":3010,"failed_step":"engine","error_code":"engine_exit_3010"})"));
  EXPECT_FALSE(installer_failure_report(R"({"schema":1,"state":"cancelled","result_code":1223,"failed_step":"engine","error_code":"engine_exit_1223"})"));
  EXPECT_FALSE(installer_failure_report(R"({"schema":1,"state":"failed","result_code":5})"));
  EXPECT_FALSE(installer_failure_report("not json"));
  EXPECT_FALSE(installer_failure_report(R"({"state":"failed","failed_step":"engine","error_code":"webview2_exit_5"})"));
  EXPECT_FALSE(installer_failure_report(R"({"state":"failed","failed_step":"engine","error_code":"engine_exit_5; C:\\Users\\Eve"})"));
  EXPECT_FALSE(installer_failure_report(R"({"state":"failed","failed_step":"C:\\Users\\Eve","error_code":"payload_invalid"})"));
}

TEST(InstallerFailure, EachFailedRunIsReportedOnceFromTheRunFolder) {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / ("alk-install-runs-" + std::to_string(::getpid()));
  fs::remove_all(root);
  const auto put = [&](const char *run, const std::string &text) {
    fs::create_directories(root / run);
    std::ofstream(root / run / "install-result.json") << text;
  };
  put("a-failed", R"({"schema":1,"state":"failed","result_code":3,"failed_step":"engine","error_code":"engine_exit_3"})");
  put("b-success", R"({"schema":1,"state":"success","result_code":0})");
  put("c-payload", R"({"schema":1,"state":"failed","result_code":101,"failed_step":"payload","error_code":"payload_invalid"})");
  fs::create_directories(root / "d-empty");
  const auto first = take_installer_failures(root);
  ASSERT_EQ(first.size(), 2u);
  std::vector<std::string> codes { first[0].code, first[1].code };
  std::sort(codes.begin(), codes.end());
  EXPECT_EQ(codes, std::vector<std::string>({ "engine_exit_3", "payload_invalid" }));
  EXPECT_FALSE(fs::exists(root / "a-failed" / "install-result.json"));
  EXPECT_TRUE(fs::exists(root / "a-failed" / "install-result.reported.json"));
  EXPECT_TRUE(fs::exists(root / "b-success" / "install-result.reported.json")) << "a success is taken too, and never looked at again";
  EXPECT_TRUE(take_installer_failures(root).empty());
  EXPECT_TRUE(take_installer_failures(root / "no-such-folder").empty());
  fs::remove_all(root);
}

namespace {
  std::string
  wer_xml(const std::string &module, const std::string &code, const std::string &offset, const std::string &id = "1000") {
    // as EvtRender writes it: single-quoted attributes, the Application Error data in its fixed order
    return "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='Application Error'/><EventID Qualifiers='0'>" + id +
           "</EventID><Level>2</Level><TimeCreated SystemTime='2026-11-15T13:20:11.123456700Z'/><Channel>Application</Channel><Computer>DESKTOP-ABC1234</Computer></System>"
           "<EventData><Data>WUDFHost.exe</Data><Data>10.0.26100.1</Data><Data>6a1b2c3d</Data><Data>" +
           module + "</Data><Data>1.0.0.0</Data><Data>6a1b2c3e</Data><Data>" + code + "</Data><Data>" +
           offset + "</Data><Data>1234</Data><Data>01dc5a0000000000</Data><Data>C:\\Windows\\System32\\WUDFHost.exe</Data><Data>C:\\Users\\Eve\\x\\" + module + "</Data><Data/></EventData></Event>";
  }
}  // namespace

TEST(WerEvent, OnlyTheVirtualDisplayDriversOwnFaultIsTakenAsFixedFacts) {
  const auto fault = parse_wer_event(wer_xml("ZakoVDD.dll", "c0000005", "000000000001A2B3"));
  ASSERT_TRUE(fault);
  EXPECT_EQ(fault->event_id, 1000);
  EXPECT_EQ(fault->module, "zakovdd.dll");
  EXPECT_EQ(fault->exception_code, "c0000005");
  EXPECT_EQ(fault->offset, "0x1a2b3");
  EXPECT_EQ(fault->time_ms, 1794748811123LL);  // 2026-11-15T13:20:11.123Z (date -u -d ... +%s = 1794748811)
  const auto props = wer_props(*fault);
  EXPECT_EQ(props, (json { { "event_id", "1000" }, { "kind", "crash" }, { "module", "zakovdd.dll" }, { "exception_code", "c0000005" }, { "offset", "0x1a2b3" } }));
  // nothing of the machine is in what leaves: not the host process path, not the user folder, not the computer name
  const auto text = props.dump();
  for (const char *leak : { "Users", "Eve", "DESKTOP", "WUDFHost", "System32" }) EXPECT_EQ(text.find(leak), std::string::npos) << leak;
}

TEST(WerEvent, AnotherModuleAnotherEventOrAnOddShapeIsNotTaken) {
  EXPECT_FALSE(parse_wer_event(wer_xml("sunshine.exe", "c0000005", "1a2b3")));
  EXPECT_FALSE(parse_wer_event(wer_xml("ZakoVDD.dll.exe", "c0000005", "1a2b3")));
  EXPECT_FALSE(parse_wer_event(wer_xml("ZakoVDD.dll", "c0000005", "1a2b3", "1002")));
  EXPECT_FALSE(parse_wer_event(wer_xml("ZakoVDD.dll", "access violation", "1a2b3")));
  EXPECT_FALSE(parse_wer_event(wer_xml("ZakoVDD.dll", "c0000005", "C:\\Users\\Eve")));
  EXPECT_FALSE(parse_wer_event("<Event><System><EventID>1000</EventID></System><EventData><Data>a</Data></EventData></Event>"));
  EXPECT_FALSE(parse_wer_event(""));
  EXPECT_FALSE(parse_wer_event(std::string(20000, 'x')));
  // both quote styles for the time
  auto xml = wer_xml("ZakoVDD.dll", "C0000374", "0x10");
  std::replace(xml.begin(), xml.end(), '\'', '"');
  const auto doubled = parse_wer_event(xml);
  ASSERT_TRUE(doubled);
  EXPECT_EQ(doubled->exception_code, "c0000374");
  EXPECT_EQ(doubled->offset, "0x10");
  EXPECT_EQ(doubled->time_ms, 1794748811123LL);
}

TEST(WerEvent, TheHostEventIsShapedAgainstTheAllowList) {
  const auto fault = parse_wer_event(wer_xml("ZakoVDD.dll", "c0000005", "1a2b3"));
  ASSERT_TRUE(fault);
  const auto e = shape_event(kind_e::error, "wer_event", wer_props(*fault), 5);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->props["module"], "zakovdd.dll");
  EXPECT_EQ(e->props["offset"], "0x1a2b3");
}

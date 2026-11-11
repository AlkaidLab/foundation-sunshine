/**
 * @file tests/unit/test_telemetry.cpp
 * @brief Telemetry v1: event shapes against the contract, scrubbing, consent, queue limits, retry and a fake stream session.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <atomic>
#include <regex>
#include <set>
#include <thread>

#include "src/telemetry/events.h"
#include "src/telemetry/queue.h"
#include "src/telemetry/service.h"
#include "src/telemetry/stream_stats.h"

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace telemetry;

namespace {
  struct temp_dir_t {
    fs::path path;
    temp_dir_t() {
      path = fs::temp_directory_path() / ("telemetry-test-" + new_uuid_v4());
      fs::create_directories(path);
    }
    ~temp_dir_t() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  };

  struct harness_t {
    temp_dir_t dir;
    std::int64_t now = 1'800'000'000'000;
    consent_t consent;
    std::vector<json> posted;
    std::vector<std::string> urls;
    int next_status = 200;
    std::unique_ptr<service_t> svc;

    explicit harness_t(service_options_t options = {}) {
      options.dir = dir.path;
      service_deps_t deps;
      deps.now_ms = [this] { return now; };
      deps.consent = [this] { return consent; };
      deps.random01 = [] { return 0.0; };
      deps.post = [this](const std::string &url, const std::string &body) {
        urls.push_back(url);
        posted.push_back(json::parse(body));
        return next_status;
      };
      svc = std::make_unique<service_t>(options, deps);
      context_t c;
      c.version = "5.2.0.1.杂鱼";
      c.channel = "stable";
      c.os = "windows";
      c.os_version = "10.0.26100";
      c.arch = "x86_64";
      c.gpu_vendor = "nvidia";
      c.locale = "zh-CN";
      svc->set_context(c);
    }
    int sent_events() const {
      int n = 0;
      for (const auto &b : posted) n += static_cast<int>(b["events"].size());
      return n;
    }
  };

  std::set<std::string> names_of(const std::vector<json> &batches) {
    std::set<std::string> out;
    for (const auto &b : batches)
      for (const auto &e : b["events"]) out.insert(e["name"].get<std::string>());
    return out;
  }
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// event shapes

TEST(TelemetryEvents, StreamStartKeepsAllowedDimensions) {
  json props {
    { "client_kind", "moonlight-macos" }, { "client_os", "macos" }, { "codec", "hevc" }, { "width", 3840 }, { "height", 2160 }, { "fps", 120 }, { "hdr", true },
    { "audio_codec", "opus" }, { "transport", "altp" }, { "app_kind", "sparxie" }, { "sparxie_game", "genshin" },
  };
  const auto e = shape_event(kind_e::usage, "stream_start", props, 1000);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->props, props);
  EXPECT_EQ(e->t, 1000);
  EXPECT_EQ(e->kind, kind_e::usage);
}

TEST(TelemetryEvents, UnknownNamesAndKindsAreRefused) {
  EXPECT_FALSE(shape_event(kind_e::usage, "made_up", json::object(), 0));
  EXPECT_FALSE(shape_event(kind_e::error, "stream_start", json::object(), 0));  // right name, wrong kind
  EXPECT_FALSE(shape_event(kind_e::experience, "app_start", json::object(), 0));
}

TEST(TelemetryEvents, UnknownPropsAreDroppedAndValuesNormalised) {
  const auto e = shape_event(kind_e::usage, "stream_start",
    json { { "codec", "vp9" }, { "client_os", "Plan9" }, { "width", 99999 }, { "fps", -5 }, { "hdr", "yes" }, { "host_name", "SKYPC" }, { "client_kind", "Has Space" }, { "app_kind", "desktop" } }, 0);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->props["codec"], "other");
  EXPECT_EQ(e->props["client_os"], "other");
  EXPECT_EQ(e->props["width"], 16384);  // clamped
  EXPECT_EQ(e->props["fps"], 0);
  EXPECT_FALSE(e->props.contains("hdr"));  // not a boolean
  EXPECT_FALSE(e->props.contains("host_name"));  // not in the list
  EXPECT_FALSE(e->props.contains("client_kind"));  // not a catalogue id
  EXPECT_EQ(e->props["app_kind"], "desktop");
}

TEST(TelemetryEvents, RequiredPropsMustBeValid) {
  EXPECT_FALSE(shape_event(kind_e::usage, "feature_used", json::object(), 0));
  EXPECT_FALSE(shape_event(kind_e::usage, "feature_used", json { { "feature", "Has Space And Caps" } }, 0));
  EXPECT_TRUE(shape_event(kind_e::usage, "feature_used", json { { "feature", "sparxie.import" } }, 0));
  EXPECT_FALSE(shape_event(kind_e::usage, "stream_end", json { { "duration_s", 10 } }, 0));  // result is required
  EXPECT_FALSE(shape_event(kind_e::experience, "panel_page", json { { "page", "home" } }, 0));
  const auto page = shape_event(kind_e::experience, "panel_page", json { { "page", "apps" }, { "time_to_content_ms", 123.4 } }, 0);
  ASSERT_TRUE(page);
  EXPECT_EQ(page->props["time_to_content_ms"], 123);
}

TEST(TelemetryEvents, AnErrorEventCarriesNoText) {
  const std::string long_text(500, 'x');
  const auto e = shape_event(kind_e::error, "error",
    json { { "component", "stream" }, { "code", "e1" }, { "severity", "error" }, { "count", 3 }, { "message", "cannot open C:\\Users\\Alice\\Videos\\clip.mp4 from 10.0.1.20 " + long_text } }, 0);
  ASSERT_TRUE(e);
  EXPECT_FALSE(e->props.contains("message"));  // free text exists in one place only: user_report
  EXPECT_EQ(e->props.dump().find("Alice"), std::string::npos);
  EXPECT_EQ(e->props["count"], 3);
}

TEST(TelemetryEvents, StreamQualityPropsAreNumbersInRange) {
  const auto e = shape_event(kind_e::experience, "stream_quality",
    json { { "fps_p50", 59.96 }, { "fps_p5", 1e9 }, { "frame_drop_pct", -3 }, { "bitrate_kbps_avg", 49000.4 }, { "stalls", 2 }, { "loss_pct", "high" } }, 0);
  ASSERT_TRUE(e);
  EXPECT_DOUBLE_EQ(e->props["fps_p50"].get<double>(), 60.0);
  EXPECT_DOUBLE_EQ(e->props["fps_p5"].get<double>(), 1000.0);
  EXPECT_DOUBLE_EQ(e->props["frame_drop_pct"].get<double>(), 0.0);
  EXPECT_EQ(e->props["bitrate_kbps_avg"], 49000);
  EXPECT_FALSE(e->props.contains("loss_pct"));
}

TEST(TelemetryEvents, SettingAllowListAndBuckets) {
  EXPECT_TRUE(setting_key_allowed("encoder"));
  EXPECT_FALSE(setting_key_allowed("sunshine_name"));
  EXPECT_FALSE(setting_key_allowed("credentials_file"));
  EXPECT_FALSE(setting_key_allowed("adapter_name"));
  EXPECT_FALSE(setting_key_allowed("telemetry_consent"));
  EXPECT_EQ(setting_value_bucket("encoder", "NVENC"), "nvenc");
  EXPECT_EQ(setting_value_bucket("encoder", "my-weird-thing"), "other");
  EXPECT_EQ(setting_value_bucket("minimum_fps_target", "37"), "30");
  EXPECT_EQ(setting_value_bucket("minimum_fps_target", "abc"), "other");
  EXPECT_EQ(setting_value_bucket("encoder", ""), "auto");
  EXPECT_EQ(setting_value_bucket("not_a_key", "x"), "");
}

// ---------------------------------------------------------------------------------------------------------------------
// scrubbing

TEST(TelemetryScrub, RemovesNamesAddressesAndSecrets) {
  const auto t = scrub_text("user C:\\Users\\Bob\\AppData\\x.dll at /home/carol/.config/a on 10.0.0.5 mail bob@example.com mac aa:bb:cc:dd:ee:ff token=abcdef123456 https://my.host:57984/api?pin=1234&x=2 fe80::1ff:fe23:4567:890a");
  EXPECT_EQ(t.find("Bob"), std::string::npos);
  EXPECT_EQ(t.find("carol"), std::string::npos);
  EXPECT_EQ(t.find("10.0.0.5"), std::string::npos);
  EXPECT_EQ(t.find("example.com"), std::string::npos);
  EXPECT_EQ(t.find("aa:bb"), std::string::npos);
  EXPECT_EQ(t.find("abcdef123456"), std::string::npos);
  EXPECT_EQ(t.find("my.host"), std::string::npos);
  EXPECT_EQ(t.find("pin=1234"), std::string::npos);
  EXPECT_EQ(t.find("fe80"), std::string::npos);
}

TEST(TelemetryScrub, KeepsVersionsAndCodeScopes) {
  EXPECT_EQ(scrub_text("driver 551.23.1.4 std::vector<int> v1.2.3.4"), "driver 551.23.1.4 std::vector<int> v1.2.3.4");
  EXPECT_NE(scrub_text("connect 1.2.3.4").find("<ip>"), std::string::npos);
  EXPECT_EQ(scrub_text("time 12:30:45 ok"), "time 12:30:45 ok");
}

TEST(TelemetryScrub, HomePathsAndUncAreGone) {
  EXPECT_EQ(scrub_text("/Users/dave/Library/x").find("dave"), std::string::npos);
  EXPECT_EQ(scrub_text("\\\\NAS01\\share\\file.txt").find("NAS01"), std::string::npos);
  EXPECT_EQ(scrub_text("laptop.local failed").find("laptop"), std::string::npos);
}

TEST(TelemetryScrub, CutsToCodePointsAndDropsControls) {
  std::string text = std::string("a\x01") + "b\u202e" + "c";
  for (int i = 0; i < 100; ++i) text += " ab";
  const auto t = scrub_text(text, 10);
  EXPECT_EQ(t.size(), 10u);
  EXPECT_EQ(t.substr(0, 3), "abc");
  // 4 code points of 3 bytes are cut to 3 whole characters
  EXPECT_EQ(scrub_text("杂鱼杂鱼", 3), "杂鱼杂");
}

TEST(TelemetryScrub, ApostropheIsNotAQuote) {
  const auto t = scrub_log_message("Couldn't open file for client 'Alice's iPad' because it can't be read");
  EXPECT_EQ(t.find("Alice"), std::string::npos);
  EXPECT_NE(t.find("Couldn't open file"), std::string::npos);
  EXPECT_NE(t.find("can't be read"), std::string::npos);
}

TEST(TelemetryScrub, WholePathsAreOneToken) {
  EXPECT_EQ(scrub_text("at C:\\Users\\Eve\\x.js line 3"), "at <path> line 3");
  EXPECT_EQ(scrub_text("fetch https://10.0.1.5:47990/api/apps at C:\\Users\\Alice\\x.js"), "fetch <origin>/api/apps at <path>");
}

TEST(TelemetryScrub, LogMessageDropsWhatStandsBetweenQuotes) {
  const auto t = scrub_log_message("Client \"Alice's iPhone\" refused app [Cyberpunk 2077] code [E12] {x}");
  EXPECT_EQ(t.find("Alice"), std::string::npos);
  EXPECT_EQ(t.find("Cyberpunk"), std::string::npos);
  EXPECT_NE(t.find("[E12]"), std::string::npos);
}

// ---------------------------------------------------------------------------------------------------------------------
// envelope

TEST(TelemetryEnvelope, SplitsAtFiveHundredEvents) {
  context_t c;
  c.install_id = new_uuid_v4();
  c.session_id = new_uuid_v4();
  c.version = "1.0";
  std::vector<event_t> events;
  for (int i = 0; i < 1200; ++i) events.push_back(*shape_event(kind_e::usage, "feature_used", json { { "feature", "x" } }, i));
  const auto batches = build_envelopes(c, "host", events);
  ASSERT_EQ(batches.size(), 3u);
  EXPECT_EQ(batches[0]["events"].size(), 500u);
  EXPECT_EQ(batches[2]["events"].size(), 200u);
  for (const auto &b : batches) {
    EXPECT_EQ(b["v"], 1);
    EXPECT_EQ(b["product"], "host");
    EXPECT_LE(b.dump().size(), MAX_BATCH_BYTES);
  }
}

TEST(TelemetryEnvelope, SplitsAtTheByteLimit) {
  context_t c;
  c.install_id = new_uuid_v4();
  c.version = "1.0";
  std::vector<event_t> events;
  // events a page could never send (a valid event is far smaller): the byte limit still holds for what the queue is handed
  for (int i = 0; i < 400; ++i) {
    event_t e;
    e.t = i;
    e.name = "error";
    e.kind = kind_e::error;
    e.props = json { { "message", std::string(1000, 'm') } };
    events.push_back(e);
  }
  const auto batches = build_envelopes(c, "host", events);
  EXPECT_GT(batches.size(), 1u);
  for (const auto &b : batches) EXPECT_LE(b.dump().size(), MAX_BATCH_BYTES);
}

TEST(TelemetryEnvelope, ProductVersionStartsItsOwnEnvelope) {
  context_t c;
  c.install_id = new_uuid_v4();
  c.version = "1.0.0-host";
  std::vector<event_t> events;
  auto a = *shape_event(kind_e::usage, "app_start", json { { "cold", true } }, 1);
  a.product = "panel";
  a.version = "9.1.0-panel";
  auto b = *shape_event(kind_e::usage, "app_start", json { { "cold", true } }, 2);
  b.product = "panel";
  events = { a, b };
  const auto batches = build_envelopes(c, "panel", events);
  ASSERT_EQ(batches.size(), 2u);
  std::set<std::string> versions { batches[0]["version"], batches[1]["version"] };
  EXPECT_TRUE(versions.contains("9.1.0-panel"));
  EXPECT_TRUE(versions.contains("1.0.0-host"));
}

// ---------------------------------------------------------------------------------------------------------------------
// consent

TEST(TelemetryConsent, ConfigKeysFollowTheSharedRules) {
  EXPECT_TRUE(consent_from_config("", "", false, false).usage);
  EXPECT_TRUE(consent_from_config("", "", false, false).errors);
  EXPECT_FALSE(consent_from_config("off", "", true, false).usage);
  EXPECT_FALSE(consent_from_config("off", "", true, false).errors);  // the old single answer "no" stays no for error reports
  EXPECT_TRUE(consent_from_config("ask", "", true, false).usage);  // 'ask' reads as the default
  EXPECT_FALSE(consent_from_config("on", "off", true, true).errors);
  EXPECT_TRUE(consent_from_config("on", "off", true, true).usage);
  EXPECT_TRUE(consent_from_config("off", "on", true, true).errors);
  EXPECT_FALSE(consent_from_config("false", "", true, false).usage);
}

TEST(TelemetryConsent, UsageOffSendsNoUsageOrExperience) {
  harness_t h;
  h.consent = { false, true };
  EXPECT_FALSE(h.svc->emit(kind_e::usage, "feature_used", json { { "feature", "a" } }));
  EXPECT_FALSE(h.svc->emit(kind_e::experience, "startup", json { { "host_start_ms", 1 } }));
  EXPECT_TRUE(h.svc->emit(kind_e::error, "error", json { { "component", "x" }, { "message", "boom" } }));
  h.svc->flush();
  h.svc->pump();
  ASSERT_EQ(h.posted.size(), 1u);
  EXPECT_EQ(names_of(h.posted), std::set<std::string> { "error" });
}

TEST(TelemetryConsent, ErrorsOffSendsNoErrors) {
  harness_t h;
  h.consent = { true, false };
  EXPECT_FALSE(h.svc->emit(kind_e::error, "error", json { { "component", "x" } }));
  h.svc->note_log(4, "an error line", 13);
  EXPECT_TRUE(h.svc->emit(kind_e::usage, "feature_used", json { { "feature", "a" } }));
  h.svc->flush(true);
  h.svc->pump();
  EXPECT_EQ(names_of(h.posted), std::set<std::string> { "feature_used" });
}

TEST(TelemetryConsent, BothOffSendsNothingAndCreatesNoIdentity) {
  harness_t h;
  h.consent = { false, false };
  EXPECT_FALSE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  EXPECT_FALSE(h.svc->emit(kind_e::error, "error", json { { "component", "x" } }));
  h.svc->flush(true);
  EXPECT_EQ(h.svc->pump(), 0);
  EXPECT_TRUE(h.posted.empty());
  EXPECT_FALSE(h.svc->identity());
  EXPECT_TRUE(h.svc->install_id().empty());
  EXPECT_FALSE(fs::exists(h.dir.path / "install_id"));
  EXPECT_EQ(h.svc->queued_files(), 0u);
}

TEST(TelemetryConsent, TurningOffDropsWhatWasWaiting) {
  harness_t h;
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  ASSERT_TRUE(h.svc->emit(kind_e::error, "error", json { { "component", "x" } }));
  h.svc->flush();  // both waiting on disk, in one batch
  ASSERT_EQ(h.svc->queued_files(), 1u);
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_exit", json { { "uptime_s", 5 } }));  // and one in memory
  h.consent = { false, true };
  h.svc->consent_changed();
  EXPECT_EQ(h.svc->buffered(), 0u);
  h.svc->flush();
  h.svc->pump();
  EXPECT_EQ(names_of(h.posted), std::set<std::string> { "error" });
  h.consent = { false, false };
  h.svc->emit(kind_e::error, "error", json { { "component", "x" } });
  h.svc->consent_changed();
  EXPECT_EQ(h.svc->queued_files(), 0u);
  EXPECT_FALSE(fs::exists(h.dir.path / "install_id"));
}

TEST(TelemetryConsent, ConsentOffAfterQueueingStopsTheSendAtPumpTime) {
  harness_t h;
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  h.consent = { false, true };  // turned off between flush and send; nobody called consent_changed
  EXPECT_EQ(h.svc->pump(), 0);
  EXPECT_TRUE(h.posted.empty());
  EXPECT_EQ(h.svc->queued_files(), 0u);
}

// ---------------------------------------------------------------------------------------------------------------------
// identity

TEST(TelemetryIdentity, InstallIdIsAStableUuidV4) {
  harness_t h;
  const auto id = h.svc->install_id();
  ASSERT_EQ(id.size(), 36u);
  EXPECT_EQ(id[14], '4');
  EXPECT_NE(std::string("89ab").find(id[19]), std::string::npos);
  EXPECT_EQ(h.svc->install_id(), id);
  // a second service over the same folder reads the same id; the session id is new
  service_deps_t deps;
  deps.consent = [] { return consent_t {}; };
  service_options_t options;
  options.dir = h.dir.path;
  service_t other(options, deps);
  EXPECT_EQ(other.install_id(), id);
  EXPECT_NE(other.session_id(), h.svc->session_id());
  const auto rotated = h.svc->rotate_install_id();
  EXPECT_NE(rotated, id);
  const auto identity = h.svc->identity();
  ASSERT_TRUE(identity);
  EXPECT_EQ((*identity)["install_id"], rotated);
  EXPECT_TRUE((*identity)["usage"].get<bool>());
}

TEST(TelemetryIdentity, EnvelopeCarriesTheIdentityAndContext) {
  harness_t h;
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true }, { "start_ms", 1200 } }));
  h.svc->flush();
  ASSERT_EQ(h.svc->pump(), 1);
  const auto &b = h.posted.at(0);
  EXPECT_EQ(b["v"], 1);
  EXPECT_EQ(b["install_id"], h.svc->install_id());
  EXPECT_EQ(b["session_id"], h.svc->session_id());
  EXPECT_EQ(b["product"], "host");
  EXPECT_EQ(b["version"], "5.2.0.1.杂鱼");
  EXPECT_EQ(b["os"], "windows");
  EXPECT_EQ(b["gpu_vendor"], "nvidia");
  EXPECT_EQ(h.urls.at(0), "https://api.alkaidlab.com/v1/events");
}

// ---------------------------------------------------------------------------------------------------------------------
// queue

TEST(TelemetryQueue, FileCapDropsTheOldest) {
  temp_dir_t dir;
  disk_queue_t q(dir.path, queue_limits_t { 5, 1 << 20, 300 * 1024 });
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(q.push(json { { "install_id", "x" }, { "events", json::array({ json { { "n", i } } }) } }));
  EXPECT_EQ(q.files(), 5u);
  EXPECT_EQ(q.dropped(), 3u);
  const auto first = q.front();
  ASSERT_TRUE(first);
  EXPECT_EQ(first->batch["events"][0]["n"], 3);  // 0, 1, 2 were the oldest
}

TEST(TelemetryQueue, ByteCapDropsTheOldest) {
  temp_dir_t dir;
  disk_queue_t q(dir.path, queue_limits_t { 100, 3000, 300 * 1024 });
  const std::string pad(1000, 'p');
  for (int i = 0; i < 6; ++i) ASSERT_TRUE(q.push(json { { "install_id", "x" }, { "events", json::array() }, { "pad", pad }, { "n", i } }));
  EXPECT_LE(q.bytes(), 3000u);
  EXPECT_LE(q.files(), 2u);
  const auto last = q.front();
  ASSERT_TRUE(last);
  EXPECT_GE(last->batch["n"].get<int>(), 4);
}

TEST(TelemetryQueue, CorruptFilesAreDeletedAndTemporariesCleaned) {
  temp_dir_t dir;
  {
    std::ofstream(dir.path / "0000000000001-000000.json") << "{ not json";
    std::ofstream(dir.path / "0000000000002-000000.json") << R"({"hello":1})";
    std::ofstream(dir.path / "0000000000003-000000.json.tmp") << "half";
  }
  disk_queue_t q(dir.path);
  EXPECT_FALSE(fs::exists(dir.path / "0000000000003-000000.json.tmp"));
  EXPECT_FALSE(q.front());
  EXPECT_EQ(q.files(), 0u);
}

TEST(TelemetryQueue, FilteringByConsentKeepsOnlyAllowedEvents) {
  json batch { { "install_id", "x" }, { "events", json::array({ json { { "kind", "usage" } }, json { { "kind", "error" } }, json { { "kind", "experience" } }, json { { "kind", "weird" } } }) } };
  auto copy = batch;
  EXPECT_EQ(filter_by_consent(copy, true, false), 2u);
  copy = batch;
  EXPECT_EQ(filter_by_consent(copy, false, true), 1u);
  copy = batch;
  EXPECT_EQ(filter_by_consent(copy, false, false), 0u);
}

TEST(TelemetryQueue, BackoffDoublesToAnHour) {
  EXPECT_EQ(backoff_seconds(0, 0), 0);
  EXPECT_EQ(backoff_seconds(1, 0), 30);
  EXPECT_EQ(backoff_seconds(2, 0), 60);
  EXPECT_EQ(backoff_seconds(3, 0), 120);
  EXPECT_EQ(backoff_seconds(20, 0), 3600);
  EXPECT_GT(backoff_seconds(3, 0.9), 120);
  EXPECT_LE(backoff_seconds(20, 0.999), 4320);
  EXPECT_TRUE(status_retryable(0));
  EXPECT_TRUE(status_retryable(429));
  EXPECT_TRUE(status_retryable(503));
  EXPECT_FALSE(status_retryable(400));
  EXPECT_FALSE(status_retryable(413));
}

// ---------------------------------------------------------------------------------------------------------------------
// sender

TEST(TelemetrySender, FailureBacksOffThenRecovers) {
  harness_t h;
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  h.next_status = 503;
  EXPECT_EQ(h.svc->pump(), 0);
  EXPECT_EQ(h.posted.size(), 1u);
  EXPECT_EQ(h.svc->consecutive_failures(), 1);
  EXPECT_EQ(h.svc->retry_not_before_ms(), h.now + 30'000);
  EXPECT_EQ(h.svc->queued_files(), 1u);
  // too early: no request at all
  h.now += 10'000;
  EXPECT_EQ(h.svc->pump(), 0);
  EXPECT_EQ(h.posted.size(), 1u);
  // second failure doubles the wait
  h.now += 25'000;
  EXPECT_EQ(h.svc->pump(), 0);
  EXPECT_EQ(h.posted.size(), 2u);
  EXPECT_EQ(h.svc->retry_not_before_ms(), h.now + 60'000);
  // the network is back
  h.now += 61'000;
  h.next_status = 200;
  EXPECT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(h.svc->queued_files(), 0u);
  EXPECT_EQ(h.svc->consecutive_failures(), 0);
  EXPECT_EQ(h.svc->counts().sent_batches, 1u);
}

TEST(TelemetrySender, NoAnswerAndTooManyRequestsAreRetriedButRefusalIsDropped) {
  harness_t h;
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  h.next_status = 0;
  h.svc->pump();
  EXPECT_EQ(h.svc->queued_files(), 1u);
  h.now += 100'000;
  h.next_status = 429;
  h.svc->pump();
  EXPECT_EQ(h.svc->queued_files(), 1u);
  h.now += 100'000;
  h.next_status = 400;
  h.svc->pump();
  EXPECT_EQ(h.svc->queued_files(), 0u);
  EXPECT_EQ(h.svc->counts().rejected_batches, 1u);
}

TEST(TelemetrySender, WaitingBatchesSurviveARestart) {
  harness_t h;
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  h.svc.reset();  // the host exited with the batch still on disk
  service_deps_t deps;
  std::vector<json> posted;
  deps.consent = [] { return consent_t {}; };
  deps.post = [&](const std::string &, const std::string &body) {
    posted.push_back(json::parse(body));
    return 200;
  };
  service_options_t options;
  options.dir = h.dir.path;
  service_t next(options, deps);
  EXPECT_EQ(next.pump(), 1);
  ASSERT_EQ(posted.size(), 1u);
  EXPECT_EQ(posted[0]["events"][0]["name"], "app_start");
}

TEST(TelemetrySender, BufferIsBoundedAndDropsTheOldest) {
  service_options_t options;
  options.ring_max = 10;
  harness_t h(options);
  for (int i = 0; i < 25; ++i) h.svc->emit(kind_e::usage, "feature_used", json { { "feature", "f" + std::to_string(i) } });
  EXPECT_EQ(h.svc->buffered(), 10u);
  EXPECT_EQ(h.svc->counts().dropped, 15u);
  h.svc->flush();
  h.svc->pump();
  ASSERT_EQ(h.posted.size(), 1u);
  EXPECT_EQ(h.posted[0]["events"][0]["props"]["feature"], "f15");
}

TEST(TelemetrySender, TransportThatThrowsIsAFailureNotACrash) {
  harness_t h;
  service_deps_t deps;
  deps.consent = [] { return consent_t {}; };
  deps.post = [](const std::string &, const std::string &) -> int { throw std::runtime_error("socket"); };
  service_options_t options;
  options.dir = h.dir.path / "other";
  service_t s(options, deps);
  ASSERT_TRUE(s.emit(kind_e::usage, "app_start", json { { "cold", true } }));
  s.flush();
  EXPECT_NO_THROW(s.pump());
  EXPECT_EQ(s.consecutive_failures(), 1);
}

TEST(TelemetrySender, UnknownProductIsRefused) {
  harness_t h;
  EXPECT_FALSE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }, "evil"));
}

// ---------------------------------------------------------------------------------------------------------------------
// pages through the host API

TEST(TelemetryIngest, PagesSendOnlyTheirOwnNames) {
  harness_t h;
  const json events = json::array({
    json { { "kind", "experience" }, { "name", "panel_page" }, { "props", json { { "page", "apps" }, { "time_to_content_ms", 340 } } } },
    json { { "kind", "usage" }, { "name", "feature_used" }, { "props", json { { "feature", "panel.pair_new" } } } },
    json { { "kind", "usage" }, { "name", "stream_start" }, { "props", json { { "codec", "hevc" } } } },  // host only: never from a page
    json { { "kind", "experience" }, { "name", "stream_quality" }, { "props", json { { "fps_p50", 60 } } } },
    json { { "kind", "error" }, { "name", "log_summary" }, { "props", json { { "component", "x" } } } },
    json { { "kind", "error" }, { "name", "error" }, { "props", json { { "component", "panel" }, { "message", "TypeError at C:\\Users\\Eve\\x.js" } } } },
    json { { "kind", "nonsense" }, { "name", "feature_used" } },
    json("not an object"),
  });
  EXPECT_EQ(h.svc->ingest("panel", "1.2.3.4.杂鱼", events), 3u);
  h.svc->flush();
  h.svc->pump();
  ASSERT_EQ(h.posted.size(), 1u);
  EXPECT_EQ(h.posted[0]["product"], "panel");
  EXPECT_EQ(h.posted[0]["version"], "1.2.3.4.杂鱼");
  EXPECT_EQ(names_of(h.posted), (std::set<std::string> { "panel_page", "feature_used", "error" }));
  EXPECT_EQ(h.posted[0].dump().find("Eve"), std::string::npos);
}

TEST(TelemetryIngest, AWebPageCannotBePosingAsTheHost) {
  harness_t h;
  EXPECT_EQ(h.svc->ingest("host", "", json::array({ json { { "kind", "usage" }, { "name", "app_start" }, { "props", json { { "cold", true } } } } })), 0u);
  EXPECT_EQ(h.svc->ingest("web", "", json::array({ json { { "kind", "usage" }, { "name", "app_start" }, { "props", json { { "cold", true } } } } })), 1u);
}

TEST(TelemetryIngest, ConsentAppliesToPages) {
  harness_t h;
  h.consent = { false, true };
  const json events = json::array({ json { { "kind", "usage" }, { "name", "feature_used" }, { "props", json { { "feature", "a" } } } } });
  EXPECT_EQ(h.svc->ingest("web", "", events), 0u);
}

TEST(TelemetryIngest, APageNamesAFeatureWithItsOwnIdOnly) {
  harness_t h;
  const auto one = [&](const char *product, const char *feature) {
    return h.svc->ingest(product, "", json::array({ json { { "kind", "usage" }, { "name", "feature_used" }, { "props", json { { "feature", feature } } } } }));
  };
  EXPECT_EQ(one("panel", "panel.pair_open"), 1u);
  EXPECT_EQ(one("web", "web.app_added"), 1u);
  EXPECT_EQ(one("panel", "web.app_added"), 0u);  // another product's id
  EXPECT_EQ(one("web", "evaphone"), 0u);  // a name rides in as a feature
  EXPECT_EQ(one("web", "web.evaphone-of-eve"), 1u);  // the prefix is the rule; the pages build ids from fixed words, and the Worker keeps tokens only
}

// ---------------------------------------------------------------------------------------------------------------------
// the log

TEST(TelemetryLog, HourlySummaryAndScrubbedErrors) {
  harness_t h;
  h.svc->note_log(3, "a warning", 9);
  h.svc->note_log(3, "a warning", 9);
  const std::string line = "Failed to open C:\\Users\\Zed\\Games\\game.exe for client \"Zed's laptop\" at 10.1.2.3";
  h.svc->note_log(4, line.c_str(), line.size());
  h.svc->note_log(4, line.c_str(), line.size());
  h.svc->note_log(5, "fatal thing", 11);
  h.svc->flush(true);
  h.svc->pump();
  json summary, error;
  for (const auto &b : h.posted)
    for (const auto &e : b["events"]) {
      if (e["name"] == "log_summary") summary = e;
      if (e["name"] == "error" && e["props"]["severity"] == "error") error = e;
    }
  ASSERT_FALSE(summary.is_null());
  EXPECT_EQ(summary["props"]["warning"], 2);
  EXPECT_EQ(summary["props"]["error"], 2);
  EXPECT_EQ(summary["props"]["fatal"], 1);
  ASSERT_FALSE(error.is_null());
  EXPECT_EQ(error["props"]["count"], 2);
  EXPECT_FALSE(error["props"].contains("message"));  // the line never leaves the computer
  EXPECT_EQ(error["props"]["code"].get<std::string>().size(), 9u);
  for (const auto &b : h.posted) {
    const auto text = b.dump();
    for (const char *planted : { "Zed", "10.1.2.3", "Games", "game.exe", "laptop" }) EXPECT_EQ(text.find(planted), std::string::npos) << planted;
  }
}

TEST(TelemetryLog, ErrorCodeIsStableAcrossNamesAndPaths) {
  harness_t h;
  const std::string a = "Failed to open \"Alice's iPad\" at C:\\Users\\Alice\\x.mp4", b = "Failed to open \"Bob's phone\" at /home/bob/y.mp4";
  h.svc->note_log(4, a.c_str(), a.size());
  h.svc->flush(true);
  h.svc->note_log(4, b.c_str(), b.size());
  h.svc->flush(true);
  h.svc->pump();
  std::set<std::string> codes;
  for (const auto &bt : h.posted)
    for (const auto &e : bt["events"])
      if (e["name"] == "error") codes.insert(e["props"]["code"]);
  EXPECT_EQ(codes.size(), 1u);
}

TEST(TelemetryLog, ErrorCodeIsStableAcrossNumbers) {
  harness_t h;
  const std::string a = "Encoder failed after 12 frames", b = "Encoder failed after 987 frames";
  h.svc->note_log(4, a.c_str(), a.size());
  h.svc->flush(true);
  h.svc->note_log(4, b.c_str(), b.size());
  h.svc->flush(true);
  h.svc->pump();
  std::set<std::string> codes;
  for (const auto &bt : h.posted)
    for (const auto &e : bt["events"])
      if (e["name"] == "error") codes.insert(e["props"]["code"]);
  EXPECT_EQ(codes.size(), 1u);
}

// ---------------------------------------------------------------------------------------------------------------------
// a fake stream session from start to end

TEST(TelemetryStream, ClassifiersUseOnlyAllowListedValues) {
  EXPECT_EQ(classify_app_kind("Desktop", ""), "desktop");
  EXPECT_EQ(classify_app_kind("Steam Big Picture", "steam://open/bigpicture"), "bigscreen");
  EXPECT_EQ(classify_app_kind("Genshin", "\"C:\\x\\sparxie-launch.exe\" --id g"), "sparxie");
  EXPECT_EQ(classify_app_kind("Half-Life", "steam://rungameid/70"), "steam");
  EXPECT_EQ(classify_app_kind("Notepad", "notepad.exe"), "other");
  EXPECT_EQ(result_of_stop_reason("control_timeout"), "timeout");
  EXPECT_EQ(result_of_stop_reason("protocol_error"), "error");
  EXPECT_EQ(result_of_stop_reason("host_terminate"), "kicked");
  EXPECT_EQ(result_of_stop_reason("client_cancel"), "normal");
  EXPECT_EQ(codec_of_video_format(1), "hevc");
  EXPECT_EQ(codec_of_video_format(3), "other");  // the Worker takes h264 | hevc | av1 | other
  EXPECT_EQ(codec_of_video_format(9), "other");
  EXPECT_EQ(client_os_of("macOS 15.2"), "macos");
  EXPECT_EQ(client_os_of("Windows 11"), "windows");
  EXPECT_EQ(client_os_of(""), "other");
  EXPECT_EQ(client_kind_of("moonlight-macos-enhanced"), "moonlight-macos-enhanced");
  EXPECT_EQ(client_kind_of("Moonlight macOS"), "other");  // only the families the host recognises
  EXPECT_EQ(client_kind_of("EvaPhone"), "other");  // never a name a person gave a device
  EXPECT_EQ(client_kind_of("<<weird>>"), "other");
  EXPECT_EQ(sparxie_game_id("Genshin"), "genshin");
  EXPECT_EQ(sparxie_game_id("my-private-game"), "other");
  EXPECT_EQ(sparxie_game_id(""), "");
}

TEST(TelemetryStream, FakeSessionProducesTheContractEvents) {
  harness_t h;
  stream_dims_t dims;
  dims.client_kind = "moonlight-macos";
  dims.client_os = "macos";
  dims.codec = "hevc";
  dims.width = 2560;
  dims.height = 1440;
  dims.fps = 120;
  dims.hdr = true;
  dims.transport = "altp";
  dims.app_kind = "desktop";
  h.svc->stream_begin(dims, 7, "client-a");
  EXPECT_TRUE(h.svc->streams_active());
  // 60 s of a healthy stream at 120 fps with a short stall in the middle, sampled every 2 s
  std::uint64_t frame = 0;
  for (int tick = 0; tick < 30; ++tick) {
    const int fps = (tick >= 14 && tick <= 17) ? 10 : 120;
    for (int i = 0; i < fps * 2; ++i) {
      h.svc->record_encoded_frame(7);
      h.svc->record_encode_ms(7, 3.0 + (frame++ % 10) * 0.2);
      h.svc->record_capture_ms(7, 1.5);
    }
    session_probe_t probe;
    probe.session_id = 7;
    probe.rtt_ms = 12 + tick % 5;
    probe.loss_pct = tick % 10 == 0 ? 1.0 : 0.0;
    probe.dropped_frames = tick * 2;
    probe.measured_kbps = 48000;
    h.now += 2000;
    h.svc->tick_streams({ probe });
  }
  h.svc->stream_end(7, "client_cancel");
  EXPECT_FALSE(h.svc->streams_active());
  h.svc->flush();
  ASSERT_EQ(h.svc->pump(), 1);
  const auto &events = h.posted.at(0)["events"];
  json start, end, quality, startup;
  for (const auto &e : events) {
    if (e["name"] == "stream_start") start = e;
    if (e["name"] == "stream_end") end = e;
    if (e["name"] == "stream_quality") quality = e;
    if (e["name"] == "startup") startup = e;
  }
  ASSERT_FALSE(start.is_null());
  EXPECT_EQ(start["kind"], "usage");
  EXPECT_EQ(start["props"]["codec"], "hevc");
  EXPECT_EQ(start["props"]["transport"], "altp");
  EXPECT_EQ(start["props"]["fps"], 120);
  ASSERT_FALSE(end.is_null());
  EXPECT_EQ(end["props"]["result"], "normal");
  EXPECT_NEAR(end["props"]["duration_s"].get<double>(), 60.0, 12.0);
  ASSERT_FALSE(quality.is_null());
  EXPECT_EQ(quality["kind"], "experience");
  const auto &q = quality["props"];
  EXPECT_NEAR(q["fps_p50"].get<double>(), 120.0, 1.5);
  EXPECT_LT(q["fps_p5"].get<double>(), 30.0);  // the stall is in the low end
  EXPECT_NEAR(q["encode_ms_p50"].get<double>(), 4.0, 0.6);
  EXPECT_NEAR(q["capture_ms_p95"].get<double>(), 1.5, 0.6);
  EXPECT_GE(q["network_rtt_ms_p50"].get<double>(), 12.0);
  EXPECT_LE(q["network_rtt_ms_p95"].get<double>(), 18.0);
  EXPECT_NEAR(q["loss_pct"].get<double>(), 0.1, 0.05);
  EXPECT_EQ(q["bitrate_kbps_avg"], 48000);
  EXPECT_EQ(q["stalls"], 1);
  EXPECT_EQ(q["reconnects"], 0);
  EXPECT_GT(q["frame_drop_pct"].get<double>(), 0.0);
  ASSERT_FALSE(startup.is_null());
  EXPECT_LE(startup["props"]["first_frame_ms"].get<int>(), 100);
  // every event of the batch is valid against the allow-list as sent: shaping it again changes nothing
  for (const auto &e : events) {
    const auto again = shape_event(*parse_kind(e["kind"].get<std::string>()), e["name"].get<std::string>(), e["props"], e["t"].get<std::int64_t>());
    ASSERT_TRUE(again) << e.dump();
    EXPECT_EQ(again->props, e["props"]) << e.dump();
  }
}

TEST(TelemetryStream, ReconnectWithinThirtySecondsIsCounted) {
  harness_t h;
  stream_dims_t dims;
  dims.client_kind = "c";
  h.svc->stream_begin(dims, 1, "client-a");
  h.svc->stream_end(1, "control_disconnect");
  h.now += 5000;
  h.svc->stream_begin(dims, 2, "client-a");
  h.now += 1000;
  h.svc->stream_end(2, "client_cancel");
  h.svc->flush();
  h.svc->pump();
  std::vector<int> reconnects;
  for (const auto &e : h.posted.at(0)["events"])
    if (e["name"] == "stream_quality") reconnects.push_back(e["props"]["reconnects"]);
  ASSERT_EQ(reconnects.size(), 2u);
  EXPECT_EQ(reconnects[0], 0);
  EXPECT_EQ(reconnects[1], 1);
}

TEST(TelemetryStream, NothingIsCountedForAStreamWithConsentOff) {
  harness_t h;
  h.consent = { false, false };
  stream_dims_t dims;
  h.svc->stream_begin(dims, 3, "c");
  h.svc->stream_end(3, "client_cancel");
  h.svc->flush(true);
  EXPECT_EQ(h.svc->pump(), 0);
  EXPECT_TRUE(h.posted.empty());
}

TEST(TelemetryStats, PercentilesAndHistogram) {
  EXPECT_DOUBLE_EQ(percentile({ 5, 1, 3, 2, 4 }, 50), 3);
  EXPECT_DOUBLE_EQ(percentile({}, 50), 0);
  EXPECT_DOUBLE_EQ(percentile({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }, 95), 10);
  histogram_t hist(100.0, 100);
  for (int i = 1; i <= 100; ++i) hist.add(i - 0.5);
  EXPECT_NEAR(hist.percentile(50), 50.0, 1.0);
  EXPECT_NEAR(hist.percentile(95), 95.0, 1.0);
  EXPECT_EQ(hist.count(), 100u);
  hist.add(-1);
  hist.add(std::nan(""));
  EXPECT_EQ(hist.count(), 100u);
}

// ---------------------------------------------------------------------------------------------------------------------
// what the Worker accepts (spec: "Worker additions")

TEST(TelemetryWorkerRules, UpdateSourceAndErrorCodes) {
  const auto start = shape_event(kind_e::usage, "app_start", json { { "cold", true }, { "update_source", "mirror" } }, 0);
  ASSERT_TRUE(start);
  EXPECT_EQ(start->props["update_source"], "mirror");
  EXPECT_EQ(shape_event(kind_e::usage, "app_start", json { { "update_source", "cnb" } }, 0)->props["update_source"], "other");
  const auto e = shape_event(kind_e::error, "error", json { { "component", "stream" }, { "code", "E_DXGI_LOST" }, { "severity", "info" } }, 0);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->props["code"], "E_DXGI_LOST");  // the case is kept
  EXPECT_EQ(e->props["severity"], "info");
  EXPECT_EQ(shape_event(kind_e::error, "error", json { { "component", "x" }, { "code", "0xC0000005" } }, 0)->props["code"], "0xC0000005");
  EXPECT_FALSE(shape_event(kind_e::error, "error", json { { "component", "x" }, { "code", "has space" } }, 0)->props.contains("code"));
  EXPECT_FALSE(shape_event(kind_e::error, "error", json { { "component", "x" }, { "code", std::string(65, 'A') } }, 0)->props.contains("code"));
  EXPECT_EQ(shape_event(kind_e::usage, "stream_start", json { { "codec", "pyrowave" } }, 0)->props["codec"], "other");
}

TEST(TelemetryWorkerRules, SettingKeysAndRangeBuckets) {
  for (const char *key : { "encoder", "capture", "hevc_mode", "av1_mode", "sw_preset", "sw_tune", "nvenc_preset", "nvenc_twopass", "nvenc_spatial_aq", "qp", "min_threads", "fec_percentage", "max_bitrate", "minimum_fps_target", "gamepad", "upnp", "lan_encryption_mode", "wan_encryption_mode" })
    EXPECT_TRUE(setting_key_allowed(key)) << key;
  EXPECT_EQ(setting_value_bucket("max_bitrate", "12000"), "8000-15000");
  EXPECT_EQ(setting_value_bucket("max_bitrate", "0"), "0-8000");
  EXPECT_EQ(setting_value_bucket("max_bitrate", "999999"), "200000+");
  EXPECT_EQ(setting_value_bucket("qp", "28"), "25");
  EXPECT_EQ(setting_value_bucket("nvenc_twopass", "Full_Res"), "full_res");
  // every bucket is a token the Worker accepts: [a-z0-9][a-z0-9_.:+-]*, at most 24 characters
  for (const char *v : { "12000", "999999", "0", "-5", "abc" }) {
    const auto b = setting_value_bucket("max_bitrate", v);
    EXPECT_TRUE(std::regex_match(b, std::regex("^[a-z0-9][a-z0-9_.:+-]{0,23}$"))) << b;
  }
  for (const auto key : setting_keys()) {
    const auto shaped = shape_event(kind_e::usage, "setting_changed", json { { "key", std::string(key) }, { "value", setting_value_bucket(key, "1") } }, 0);
    EXPECT_TRUE(shaped) << key;
  }
}

TEST(TelemetryWorkerRules, VersionKeepsTheFullStringOrFallsBackToTheHosts) {
  harness_t h;
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }, "panel", "2026.1112.0.杂鱼"));
  // not a version (a path, a name, text with punctuation): it is not cleaned and sent, the batch carries the host's own version
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_exit", json { { "uptime_s", 3 } }, "panel", "2026.1112.0.杂鱼 (x)\n"));
  h.svc->flush();
  h.svc->pump();
  std::set<std::string> versions;
  for (const auto &b : h.posted) versions.insert(b["version"]);
  EXPECT_EQ(versions, (std::set<std::string> { "2026.1112.0.杂鱼", "5.2.0.1.杂鱼" }));
}

TEST(TelemetryWorkerRules, RetryAfterIsBelieved) {
  harness_t h;
  service_deps_t deps;
  deps.consent = [] { return consent_t {}; };
  std::int64_t now = 1'800'000'000'000;
  deps.now_ms = [&] { return now; };
  deps.random01 = [] { return 0.0; };
  deps.post = [](const std::string &, const std::string &) { return post_result_t { 429, 300 }; };
  service_options_t options;
  options.dir = h.dir.path / "ra";
  service_t s(options, deps);
  ASSERT_TRUE(s.emit(kind_e::usage, "app_start", json { { "cold", true } }));
  s.flush();
  s.pump();
  EXPECT_EQ(s.retry_not_before_ms(), now + 300'000);  // not the 30 s of the backoff
  EXPECT_EQ(s.queued_files(), 1u);
}

TEST(TelemetryWorkerRules, StaysUnderTheRequestLimit) {
  service_options_t options;
  options.max_batches_per_pump = 100;
  harness_t h(options);
  for (int i = 0; i < 40; ++i) {
    h.svc->emit(kind_e::usage, "feature_used", json { { "feature", "f" } });
    h.svc->flush();  // one batch per flush
  }
  ASSERT_EQ(h.svc->queued_files(), 40u);
  h.svc->pump();
  EXPECT_EQ(h.posted.size(), 15u);  // 15 requests a minute, the Worker allows 20
  h.now += 61'000;
  h.svc->pump();
  EXPECT_EQ(h.posted.size(), 30u);
}

// ---------------------------------------------------------------------------------------------------------------------
// threads: the streaming threads emit and count while the sender thread flushes and sends

TEST(TelemetryThreads, EmittersAndSenderDoNotRace) {
  temp_dir_t dir;
  std::atomic<std::int64_t> clock { 1'800'000'000'000 };
  std::atomic<int> posts { 0 };
  service_deps_t deps;
  deps.now_ms = [&] { return clock.load(); };
  deps.consent = [] { return consent_t {}; };
  deps.post = [&](const std::string &, const std::string &) {
    ++posts;
    return post_result_t { 200 };
  };
  service_options_t options;
  options.dir = dir.path;
  options.max_posts_per_minute = 100000;
  service_t svc(options, deps);
  context_t c;
  c.version = "1";
  svc.set_context(c);
  stream_dims_t dims;
  svc.stream_begin(dims, 5, "c");
  std::atomic<bool> stop { false };
  std::vector<std::thread> threads;
  for (int t = 0; t < 3; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 2000 && !stop; ++i) {
        svc.record_encoded_frame(5);
        svc.record_encode_ms(5, 3.0);
        svc.record_capture_ms(5, 1.0);
        svc.emit(kind_e::usage, "feature_used", json { { "feature", "x" } });
        svc.note_log(4, "an error", 8);
      }
    });
  }
  threads.emplace_back([&] {
    while (!stop) {
      svc.tick_streams({});
      svc.flush();
      svc.pump();
      clock += 1000;
    }
  });
  threads.emplace_back([&] {
    for (int i = 0; i < 20 && !stop; ++i) {
      svc.consent_changed();
      svc.identity();
      svc.preview(10);
    }
  });
  for (std::size_t i = 0; i + 1 < threads.size(); ++i)
    if (i < 3) threads[i].join();
  threads[4].join();
  stop = true;
  threads[3].join();
  svc.stream_end(5, "client_cancel");
  svc.flush(true);
  svc.pump();
  EXPECT_GT(posts.load(), 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// reports sent by hand, the Sparxie game and the transport

// ordinary text of about n characters (a run of 32 or more letters would be taken for a token and replaced)
static std::string words(std::size_t n) {
  std::string out;
  while (out.size() < n) out += "word ";
  return out;
}

TEST(TelemetryUserReport, ShapeFollowsTheContract) {
  const std::string att = "att-2026-11/" + std::string(64, 'a') + ".zip";
  const auto e = shape_event(kind_e::error, "user_report",
    json { { "category", "performance" }, { "component", "panel" }, { "page", "display-modes" }, { "error_code", "E_DXGI_LOST" }, { "attachment", att }, { "message", words(1500) + " at C:\\Users\\Eve\\x for 10.0.0.1" }, { "contact", "me@example.org" } }, 0);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->kind, kind_e::error);
  EXPECT_EQ(e->props["category"], "performance");
  EXPECT_FALSE(e->props.contains("contact"));  // no contact field exists on purpose
  EXPECT_EQ(e->props["attachment"], att);
  EXPECT_EQ(e->props["error_code"], "E_DXGI_LOST");
  const auto message = e->props["message"].get<std::string>();
  EXPECT_GT(message.size(), 1500u);  // longer than an ordinary error message
  EXPECT_LE(message.size(), 2000u);
  EXPECT_EQ(message.find("Eve"), std::string::npos);
  EXPECT_EQ(message.find("10.0.0.1"), std::string::npos);
  // the component is a token (the host lower-cases nothing: a wrong shape is refused as a whole, component is required)
  EXPECT_FALSE(shape_event(kind_e::error, "user_report", json { { "component", "Panel" }, { "message", "m" } }, 0));
  // a long message is cut at 2000
  EXPECT_LE(shape_event(kind_e::error, "user_report", json { { "component", "panel" }, { "message", words(5000) } }, 0)->props["message"].get<std::string>().size(), 2000u);
  EXPECT_FALSE(shape_event(kind_e::error, "user_report", json { { "component", "panel" } }, 0));  // the message is required
}

TEST(TelemetryUserReport, CategoryAndAttachmentAreChecked) {
  const auto base = [](json extra) {
    json p { { "component", "web" }, { "message", "m" } };
    p.update(extra);
    return shape_event(kind_e::error, "user_report", p, 0);
  };
  EXPECT_EQ(base(json { { "category", "nonsense" } })->props["category"], "other");
  for (const char *c : { "bug", "performance", "compat", "suggestion", "other" }) EXPECT_EQ(base(json { { "category", c } })->props["category"], c);
  for (const char *bad : { "https://evil.example/a.zip", "C:\\x\\a.zip", "att-2026-11/short.zip", "other-2026-11/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.zip" })
    EXPECT_FALSE(base(json { { "attachment", bad } })->props.contains("attachment")) << bad;
  EXPECT_FALSE(base(json { { "error_code", "has space" } })->props.contains("error_code"));
}

TEST(TelemetryUserReport, FollowsTheErrorReportChoice) {
  harness_t h;
  const json events = json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "panel" }, { "message", "it broke" } } } } });
  h.consent = { true, false };
  EXPECT_EQ(h.svc->ingest("panel", "1.0", events), 0u);  // error reports off: refused
  h.consent = { false, true };
  EXPECT_EQ(h.svc->ingest("panel", "1.0", events), 1u);
  h.svc->flush();
  ASSERT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(h.posted.at(0)["events"][0]["name"], "user_report");
  // turned off while waiting: dropped
  h.svc->ingest("web", "1", events);
  h.consent = { true, false };
  h.svc->consent_changed();
  h.svc->flush();
  EXPECT_EQ(h.svc->pump(), 0);
}

TEST(TelemetryUserReport, OnlyPagesMaySendIt) {
  harness_t h;
  EXPECT_EQ(h.svc->ingest("host", "", json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "component", "x" }, { "message", "m" } } } } })), 0u);
  EXPECT_FALSE(shape_event(kind_e::usage, "user_report", json::object(), 0));
  EXPECT_FALSE(shape_event(kind_e::error, "report", json { { "component", "x" } }, 0));  // the old name is gone
}

TEST(TelemetryStream, SparxieGameComesFromTheAppsJsonEntryOfTheApp) {
  const std::string apps = R"({"apps":[
    {"id":"1","name":"Desktop"},
    {"id":"2","name":"Genshin","cmd":"C:\\Games\\GenshinImpact.exe","sparxie":{"install":"abc","game":"genshin"}},
    {"id":3,"name":"HSR","sparxie":{"game":"StarRail"}},
    {"id":"4","name":"Odd","sparxie":{"game":"Some Free Text!"}},
    {"id":"5","name":"No game","sparxie":{}}]})";
  EXPECT_FALSE(sparxie_app_of_apps(apps, "1").is_sparxie);
  const auto g = sparxie_app_of_apps(apps, "2");
  EXPECT_TRUE(g.is_sparxie);
  EXPECT_EQ(g.game, "genshin");
  EXPECT_EQ(sparxie_app_of_apps(apps, "3").game, "starrail");  // an integer id, a game written with capitals
  EXPECT_TRUE(sparxie_app_of_apps(apps, "4").is_sparxie);
  EXPECT_EQ(sparxie_app_of_apps(apps, "4").game, "other");  // free text is never passed on: a game outside the catalogue is "other"
  EXPECT_TRUE(sparxie_app_of_apps(apps, "5").is_sparxie);
  EXPECT_EQ(sparxie_app_of_apps(apps, "5").game, "");
  EXPECT_FALSE(sparxie_app_of_apps(apps, "99").is_sparxie);
  EXPECT_FALSE(sparxie_app_of_apps("not json", "2").is_sparxie);
  EXPECT_FALSE(sparxie_app_of_apps(apps, "").is_sparxie);
}

TEST(TelemetryStream, TransportIsDecidedInOnePlace) {
  EXPECT_EQ(transport_of(false), "enet");
  EXPECT_EQ(transport_of(true), "altp");
  harness_t h;
  stream_dims_t d;
  d.app_kind = "sparxie";
  d.sparxie_game = "zzz";
  d.transport = transport_of(false);
  h.svc->stream_begin(d, 9, "c");
  h.svc->stream_end(9, "client_cancel");
  h.svc->flush();
  h.svc->pump();
  for (const auto &e : h.posted.at(0)["events"])
    if (e["name"] == "stream_start") {
      EXPECT_EQ(e["props"]["sparxie_game"], "zzz");
      EXPECT_EQ(e["props"]["transport"], "enet");
      EXPECT_EQ(e["props"]["app_kind"], "sparxie");
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// the shared scrub vectors: one file, three implementations (C++ here, JS in m3e, TS in the Worker)

TEST(TelemetryScrubVectors, EveryVectorHoldsForTheTextAndTheLogScrubber) {
  const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "data" / "telemetry_scrub_vectors.json";
  std::ifstream in(path);
  ASSERT_TRUE(in.good()) << path;
  const auto doc = nlohmann::json::parse(in);
  ASSERT_GE(doc["vectors"].size(), 30u);
  for (const auto &v : doc["vectors"]) {
    const std::string input = v["input"];
    const std::string name = v["name"];
    for (int which = 0; which < 2; ++which) {
      const auto out = which == 0 ? scrub_text(input, 400) : scrub_log_message(input, 400);
      for (const auto &f : v["forbidden"]) EXPECT_EQ(out.find(f.get<std::string>()), std::string::npos) << (which ? "log: " : "text: ") << name << " leaked  << f.get<std::string>() <<  -> " << out;
      // the log scrubber also drops what a person chose between quotes or brackets, so only the text scrubber has to keep every required word
      if (which == 0)
        for (const auto &r : v["required"]) EXPECT_NE(out.find(r.get<std::string>()), std::string::npos) << name << " lost  << r.get<std::string>() <<  -> " << out;
    }
  }
}


// ---------------------------------------------------------------------------------------------------------------------
// what the envelope may carry: nothing a person typed

TEST(TelemetryEnvelope, VersionOsAndLocaleAreCheckedBeforeTheyLeave) {
  EXPECT_EQ(safe_version("2026.1112.0.杂鱼"), "2026.1112.0.杂鱼");
  EXPECT_EQ(safe_version("C:\\Users\\Eve\\x"), "");
  EXPECT_EQ(safe_version("EvaSky"), "");  // a name is not a version
  EXPECT_EQ(safe_version("CUsersEvaSky"), "");
  EXPECT_EQ(safe_version("1.0 me@example.org"), "");
  EXPECT_EQ(safe_version(std::string(65, '1')), "");
  EXPECT_EQ(safe_os_version("10.0.26100"), "10.0.26100");
  EXPECT_EQ(safe_os_version("Windows 11 Pro"), "Windows 11 Pro");
  EXPECT_EQ(safe_os_version("DESKTOP-EVE\\Eve"), "");
  EXPECT_EQ(safe_locale("zh-CN"), "zh-CN");
  EXPECT_EQ(safe_locale("pt_BR"), "pt_BR");
  EXPECT_EQ(safe_locale("my own words"), "");
  EXPECT_EQ(safe_locale("<script>"), "");
  context_t context;
  context.install_id = "11111111-2222-4333-8444-555555555555";
  context.session_id = "66666666-7777-4888-8999-aaaaaaaaaaaa";
  context.version = "2026.1112.0.杂鱼";
  context.os_version = "C:\\Users\\Eve";
  context.locale = "Eve's PC";
  event_t e;
  e.name = "app_exit";
  e.kind = kind_e::usage;
  e.props = json { { "uptime_s", 5 } };
  const auto batches = build_envelopes(context, "host", { e });
  ASSERT_EQ(batches.size(), 1u);
  EXPECT_EQ(batches[0]["os_version"], "");
  EXPECT_EQ(batches[0]["locale"], "");
  EXPECT_EQ(batches[0].dump().find("Eve"), std::string::npos);
}

TEST(TelemetryEnvelope, TheFieldsOfABatchAreWhatThePersonSees) {
  context_t context;
  context.install_id = "11111111-2222-4333-8444-555555555555";
  context.session_id = "66666666-7777-4888-8999-aaaaaaaaaaaa";
  context.version = "2026.1112.0.杂鱼";
  context.channel = "stable";
  context.os = "windows";
  context.os_version = "10.0.26100";
  context.arch = "x64";
  context.gpu_vendor = "nvidia";
  context.locale = "zh-CN";
  const auto fields = envelope_fields(context, "host", context.version);
  EXPECT_FALSE(fields.contains("events"));
  EXPECT_EQ(fields["version"], "2026.1112.0.杂鱼");
  EXPECT_EQ(fields["gpu_vendor"], "nvidia");
  // the same fields lead every batch
  const auto batches = build_envelopes(context, "host", { *shape_event(kind_e::usage, "app_exit", json { { "uptime_s", 3 } }, 1) });
  ASSERT_EQ(batches.size(), 1u);
  for (const auto &[key, value] : fields.items()) EXPECT_EQ(batches[0][key], value) << key;
}

// ---------------------------------------------------------------------------------------------------------------------
// the planted-values gate: whatever a person, a device or a log line carries, none of it reaches a batch

TEST(TelemetryPrivacyGate, PlantedNamesPathsAddressesAndTokensNeverReachABatch) {
  const std::vector<std::string> planted = { "EvaSky", "EVA-GAMING-PC", "10.0.31.57", "00:1A:2B:3C:4D:5E", "550e8400-e29b-41d4-a716-446655440000", "eva.sky@example.com", "ghp_0123456789abcdefghijklmnopqrstuvwxyz", "Honkai Secret Mod", "EvaPhone", "76561198012345678" };
  harness_t h;
  context_t c;
  c.version = "5.2.0.1.杂鱼";
  c.channel = "stable";
  c.os = "windows";
  c.os_version = "C:\\Users\\EvaSky";  // a path in a field that must be a version
  c.arch = "x86_64";
  c.gpu_vendor = "nvidia";
  c.locale = "EvaSky's PC";  // a typed word where a language tag belongs
  h.svc->set_context(c);

  const std::string line = "Cannot open \"EvaPhone\" at C:\\Users\\EvaSky\\Honkai Secret Mod\\x.exe from 10.0.31.57 mac 00:1A:2B:3C:4D:5E id 550e8400-e29b-41d4-a716-446655440000 mail eva.sky@example.com token=ghp_0123456789abcdefghijklmnopqrstuvwxyz steam 76561198012345678 host EVA-GAMING-PC.local";
  h.svc->note_log(4, line.c_str(), line.size());
  h.svc->note_log(5, line.c_str(), line.size());

  // a stream whose client and game came from a person's own names
  stream_dims_t dims;
  dims.client_kind = client_kind_of("EvaPhone");
  dims.client_os = client_os_of("EvaSky OS");
  dims.codec = "hevc";
  dims.app_kind = classify_app_kind("Honkai Secret Mod", "C:\\Users\\EvaSky\\Honkai Secret Mod\\x.exe");
  dims.sparxie_game = sparxie_game_id("Honkai Secret Mod");
  dims.transport = "enet";
  h.svc->stream_begin(dims, 1, "client-1");
  h.svc->stream_end(1, "client_cancel");

  // the one free text, written by the person: a name typed in plain words cannot be told from any other word, so the planted values here are the ones a scrubber can know (paths, addresses, ids, mail, tokens)
  const std::string typed = "It broke at C:\\Users\\EvaSky\\x.exe from 10.0.31.57 mac 00:1A:2B:3C:4D:5E id 550e8400-e29b-41d4-a716-446655440000 mail eva.sky@example.com token=ghp_0123456789abcdefghijklmnopqrstuvwxyz steam 76561198012345678 host EVA-GAMING-PC.local";

  // what the pages may send, with names in every place a name could go
  const json page = json::array({
    json { { "kind", "error" }, { "name", "error" }, { "props", json { { "component", "panel" }, { "code", "load_failed" }, { "severity", "error" }, { "count", 1 }, { "message", line } } } },
    json { { "kind", "usage" }, { "name", "setting_changed" }, { "props", json { { "key", "locale" }, { "value", "C:\\Users\\EvaSky" } } } },
    json { { "kind", "usage" }, { "name", "feature_used" }, { "props", json { { "feature", "EvaSky" } } } },
    json { { "kind", "usage" }, { "name", "setting_changed" }, { "props", json { { "key", "EvaSky" }, { "value", "x" } } } },
    json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "web" }, { "message", typed }, { "contact", "eva.sky@example.com" } } } },
  });
  h.svc->ingest("web", "C:\\Users\\EvaSky", page);
  h.svc->flush(true);
  h.svc->pump();
  ASSERT_FALSE(h.posted.empty());
  std::string all;
  for (const auto &b : h.posted) all += b.dump();
  const auto around = [&](const std::string &secret) {
    const auto at = all.find(secret);
    return at == std::string::npos ? std::string {} : all.substr(at > 100 ? at - 100 : 0, 220);
  };
  for (const auto &secret : planted) EXPECT_EQ(all.find(secret), std::string::npos) << secret << " reached a batch, around: " << around(secret);
  EXPECT_EQ(all.find("EvaSky"), std::string::npos) << around("EvaSky");
  EXPECT_EQ(all.find("Users"), std::string::npos) << around("Users");
}

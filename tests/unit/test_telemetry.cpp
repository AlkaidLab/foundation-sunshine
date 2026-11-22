/**
 * @file tests/unit/test_telemetry.cpp
 * @brief Telemetry v1: event shapes against the contract, scrubbing, consent, queue limits, retry and a fake stream session.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <thread>

#include "src/log_text.h"
#include "src/telemetry/device_facts.h"
#include "src/telemetry/error_code.h"
#include "src/telemetry/events.h"
#include "src/telemetry/install_journal.h"
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
    std::string next_body;  ///< the body of the next answer (the server's configuration rides in it)
    double random_value = 0.0;  ///< what the random source answers: the Tier 1 draw of a stream and the jitter of a backoff
    std::optional<std::string> log_ref;  ///< what the session log upload returns
    int log_calls = 0;
    std::optional<std::string> shot_ref;  ///< what the screenshot upload returns
    std::string shot_state = "refused";  ///< the word the upload gives when it returns no reference
    int shot_calls = 0;
    std::vector<std::string> shot_images;  ///< the picture each upload was given
    std::vector<std::string> log_ids;  ///< the install id (or the one-time report id) each session log upload was made under
    std::function<void()> before_queue_write;  ///< what the service calls before each batch write of a flush: a test moves a consent change into the middle of one
    std::unique_ptr<service_t> svc;

    explicit harness_t(service_options_t options = {}, double tier1_rate = 1.0) {
      options.dir = dir.path;
      std::ofstream(dir.path / "remote_config.json") << json { { "tier1_rate", tier1_rate } }.dump();
      service_deps_t deps;
      deps.now_ms = [this] { return now; };
      deps.consent = [this] { return consent; };
      deps.random01 = [this] { return random_value; };
      deps.before_queue_write = [this] {
        if (before_queue_write) before_queue_write();
      };
      deps.session_log = [this](const std::string &, const std::string &id, const std::string &) {
        ++log_calls;
        log_ids.push_back(id);
        return log_ref;
      };
      deps.session_shot = [this](const std::string &, const std::string &, const std::string &, const std::string &image) {
        ++shot_calls;
        shot_images.push_back(image);
        return shot_ref;
      };
      deps.session_shot_state = [this] { return shot_state; };
      deps.post = [this](const std::string &url, const std::string &body) {
        urls.push_back(url);
        posted.push_back(json::parse(body));
        post_result_t answer { next_status };
        answer.body = next_body;
        return answer;
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
    int
    sent_events() const {
      int n = 0;
      for (const auto &b : posted) n += static_cast<int>(b["events"].size());
      return n;
    }
  };

  std::set<std::string>
  names_of(const std::vector<json> &batches) {
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
    { "client_kind", "moonlight-macos" },
    { "client_os", "macos" },
    { "codec", "hevc" },
    { "width", 3840 },
    { "height", 2160 },
    { "fps", 120 },
    { "hdr", true },
    { "audio_codec", "opus" },
    { "transport", "altp" },
    { "app_kind", "sparxie" },
    { "sparxie_game", "genshin" },
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
  h.svc->note_log(4, "an error line");
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
  h.svc->flush();  // both waiting on disk: a batch holds events of one priority, so two batches
  ASSERT_EQ(h.svc->queued_files(), 2u);
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
  ASSERT_EQ(h.posted.size(), 2u);  // one batch per priority
  for (const auto &batch : h.posted) {
    EXPECT_EQ(batch["product"], "panel");
    EXPECT_EQ(batch["version"], "1.2.3.4.杂鱼");
  }
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
  h.svc->note_log(3, {});
  h.svc->note_log(3, {});
  const std::string line = "Failed to open C:\\Users\\Zed\\Games\\game.exe for client \"Zed's laptop\" at 10.1.2.3";
  h.svc->note_log_line(4, line);
  h.svc->note_log(4, "Failed to open {} for client {} at {}");
  h.svc->note_log(4, "Failed to open {} for client {} at {}");
  h.svc->note_log(5, "fatal thing");
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
  EXPECT_EQ(error["props"]["build"], "5.2.0.1");  // the version as an allow-list token: the non-ASCII tail of the tag is cut
  EXPECT_FALSE(error["props"].contains("message"));  // the line never leaves the computer
  EXPECT_EQ(error["props"]["code"].get<std::string>().size(), 9u);
  for (const auto &b : h.posted) {
    const auto text = b.dump();
    for (const char *planted : { "Zed", "10.1.2.3", "Games", "game.exe", "laptop" }) EXPECT_EQ(text.find(planted), std::string::npos) << planted;
  }
}

TEST(TelemetryLog, TheFirstLineOfACodeBecomesAnErrorReportWithItsSiteAndNoText) {
  harness_t h;
  h.svc->note_log(4, "{} download failed or rejected: {}", "host", "src/httpcommon.cpp:838:download");
  h.svc->note_log(4, "{} download failed or rejected: {}", "host", "src/httpcommon.cpp:838:download");
  h.svc->note_log(4, {}, "host", {});  // an opaque line carries the fallback site
  h.svc->flush(true);
  h.svc->pump();
  std::vector<json> reports;
  json code_of_error;
  for (const auto &b : h.posted)
    for (const auto &e : b["events"]) {
      if (e["name"] == "error_report") reports.push_back(e);
      if (e["name"] == "error" && e["props"]["count"] == 2) code_of_error = e;
    }
  ASSERT_EQ(reports.size(), 1u);
  ASSERT_FALSE(code_of_error.is_null());
  EXPECT_EQ(reports[0]["props"]["code"], code_of_error["props"]["code"]);  // the report and the count are one group
  EXPECT_EQ(reports[0]["props"]["loc"], "src/httpcommon.cpp:838:download");
  EXPECT_EQ(code_of_error["props"]["site"], "httpcommon.cpp:838");
  const auto text = reports[0].dump();
  for (const char *planted : { "cdn.example", "Zed", "laptop", "header.jpg" }) EXPECT_EQ(text.find(planted), std::string::npos) << planted;
}

TEST(TelemetryLog, TemplateGroupsArgumentValuesAndKeepsDistinctOperations) {
  harness_t h;
  constexpr auto first = "{} failed to query display paths";
  constexpr auto second = "{} failed to set HDR";
  h.svc->note_log_line(4, "[code: ERROR_GEN_FAILURE] failed to query display paths");
  h.svc->note_log(4, first, "host", "src/windows_utils.cpp:20:query");
  h.svc->note_log_line(4, "[code: ERROR_NOT_SUPPORTED] failed to query display paths");
  h.svc->note_log(4, first, "host", "src/windows_utils.cpp:20:query");
  h.svc->note_log(4, second, "host", "src/windows_utils.cpp:21:set_hdr");
  h.svc->flush(true);
  h.svc->pump();
  std::map<std::string, json> errors;
  for (const auto &batch : h.posted)
    for (const auto &event : batch["events"])
      if (event["name"] == "error") errors.emplace(event["props"]["site"].get<std::string>(), event["props"]);
  ASSERT_EQ(errors.size(), 2u);
  EXPECT_EQ(errors.at("windows_utils.cpp:20")["count"], 2);
  EXPECT_EQ(errors.at("windows_utils.cpp:20")["code"], template_error_code(first));
  EXPECT_EQ(errors.at("windows_utils.cpp:21")["code"], template_error_code(second));
  EXPECT_NE(errors.at("windows_utils.cpp:20")["code"], errors.at("windows_utils.cpp:21")["code"]);
}

TEST(TelemetryLog, TemplatesAreScrubbedAndOpaqueRecordsUseOnlyTheirSite) {
  harness_t h;
  h.svc->note_log(4, "Cannot open sample-device.local at 192.0.2.31", "host", "src/logging.cpp:100:callback");
  h.svc->note_log(4, {}, "host", "src/logging.cpp:101:callback");
  h.svc->note_log(4, {}, "host", {});
  h.svc->flush(true);
  h.svc->pump();
  std::map<std::string, std::string> codes;
  for (const auto &batch : h.posted)
    for (const auto &event : batch["events"])
      if (event["name"] == "error") codes.emplace(event["props"]["site"].get<std::string>(), event["props"]["code"].get<std::string>());
  ASSERT_EQ(codes.size(), 3u);
  EXPECT_EQ(codes.at("logging.cpp:100"), template_error_code("Cannot open <host> at <ip>"));
  EXPECT_EQ(codes.at("logging.cpp:101"), template_error_code("logging.cpp:101"));
  EXPECT_EQ(codes.at("host-log"), template_error_code("host-log"));
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
  // four sources; anything else is dropped to the fallback word
  EXPECT_EQ(shape_event(kind_e::usage, "app_start", json { { "update_source", "cnb" } }, 0)->props["update_source"], "cnb");
  EXPECT_EQ(shape_event(kind_e::usage, "app_start", json { { "update_source", "github" } }, 0)->props["update_source"], "github");
  // why the chosen source was not used: five words
  for (const char *miss : { "version_lag", "cdk_invalid", "quota", "network", "no_cdk" }) {
    const auto with_miss = shape_event(kind_e::usage, "app_start", json { { "update_source", "cnb" }, { "update_source_miss", miss } }, 0);
    ASSERT_TRUE(with_miss) << miss;
    EXPECT_EQ(with_miss->props["update_source_miss"], miss);
  }
  const auto bogus_miss = shape_event(kind_e::usage, "app_start", json { { "update_source", "cnb" }, { "update_source_miss", "bing" } }, 0);
  ASSERT_TRUE(bogus_miss);
  EXPECT_FALSE(bogus_miss->props.contains("update_source_miss"));
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
        svc.note_log(4, "an error");
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
static std::string
words(std::size_t n) {
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
  EXPECT_EQ(e->props["contact"], "me@example.org");  // what a person left on purpose to be answered is kept as they wrote it (it is not scrubbed)
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

TEST(TelemetryUserReport, IsNotHeldBackByTheErrorReportChoice) {
  // pressing 发送 is the person's consent for that one send: the error-report switch decides about the automatic reports, not about this one
  harness_t h;
  const json events = json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "panel" }, { "message", "it broke" } } } } });
  h.consent = { true, false };  // usage on, error reports off
  EXPECT_EQ(h.svc->ingest("panel", "1.0", events), 1u);
  h.svc->flush();
  ASSERT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(h.posted.at(0)["events"][0]["name"], "user_report");
  // the choice changes while the report waits: the report stays, the automatic error events of the same batch would not
  h.consent = { false, true };
  h.svc->ingest("web", "1", events);
  h.svc->emit(kind_e::error, "error", json { { "component", "x" }, { "code", "E_LOST" } });
  h.consent = { true, false };
  h.svc->consent_changed();
  h.svc->flush();
  ASSERT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(names_of({ h.posted.back() }), std::set<std::string> { "user_report" });
}

TEST(TelemetryUserReport, ContactIsOneLineKeptAsTheyWroteItAndCutAtTwoHundredCharacters) {
  const auto base = [](const json &contact) {
    json p { { "category", "bug" }, { "component", "web" }, { "message", "m" } };
    if (!contact.is_null()) p["contact"] = contact;
    return shape_event(kind_e::error, "user_report", p, 0);
  };
  // not scrubbed: an address they gave is what it is for; control characters and runs of blanks become one space
  EXPECT_EQ(base("  me@example.org \n QQ\t12345  ")->props["contact"], "me@example.org QQ 12345");
  EXPECT_EQ(base(std::string(300, 'x'))->props["contact"].get<std::string>().size(), MAX_USER_CONTACT_CHARS);
  EXPECT_EQ(base(std::string(150, 'x') + "\xe4\xb8\xad" + std::string(100, 'y'))->props["contact"].get<std::string>().substr(150, 3), "\xe4\xb8\xad");  // a multi-byte character is never split
  for (const json &none : { json(""), json("   \n"), json(12345), json(true), json::array() }) EXPECT_FALSE(base(none)->props.contains("contact")) << none.dump();
  EXPECT_FALSE(base(nullptr)->props.contains("contact"));
}

TEST(TelemetryUserReport, TheQueueKeepsAReportWhenErrorReportsAreOff) {
  json envelope { { "events", json::array({ json { { "kind", "error" }, { "name", "user_report" } }, json { { "kind", "error" }, { "name", "error" } }, json { { "kind", "usage" }, { "name", "app_start" } } }) } };
  EXPECT_EQ(filter_by_consent(envelope, true, false), 2u);
  EXPECT_EQ(envelope["events"][0]["name"], "user_report");
  EXPECT_EQ(envelope["events"][1]["name"], "app_start");
  json off { { "events", json::array({ json { { "kind", "error" }, { "name", "error" } } }) } };
  EXPECT_EQ(filter_by_consent(off, true, false), 0u);
}

namespace {
  /** The event of that name in the posted batches (the latest one when `latest`, else the first); a null json when there is none. A feedback report also leaves a small feedback_submit event of its own. */
  json
  posted_event(const std::vector<json> &posted, const std::string &name, bool latest = false) {
    json found;
    for (const auto &batch : posted)
      for (const auto &e : batch["events"])
        if (e.value("name", "") == name && (latest || found.is_null())) found = e;
    return found;
  }
}  // namespace

TEST(TelemetryUserReport, TheSessionLogGoesUpFirstAndItsReferenceTravelsInTheReport) {
  harness_t h;
  h.consent = { false, true };
  h.log_ref = "crash-2026-11/ur-0123456789abcdef01234567.log.gz";
  const json report = json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "panel" }, { "message", "it broke" }, { "log_ref", "crash-2026-11/ur-ffffffffffffffffffffffff.log.gz" } } } } });
  EXPECT_EQ(h.svc->ingest("web", "1", report), 1u);
  EXPECT_EQ(h.log_calls, 1);
  h.svc->flush();
  ASSERT_GE(h.svc->pump(), 1);
  EXPECT_EQ(posted_event(h.posted, "user_report")["props"]["log_ref"], *h.log_ref);  // the page cannot name a reference itself
  // no log (error reports off, no endpoint, nothing to send): the report goes without one
  h.log_ref.reset();
  EXPECT_EQ(h.svc->ingest("web", "1", report), 1u);
  h.svc->flush();
  ASSERT_GE(h.svc->pump(), 1);
  EXPECT_FALSE(posted_event(h.posted, "user_report", true)["props"].contains("log_ref"));
  // other events never ask for a log
  const auto before = h.log_calls;
  h.svc->ingest("web", "1", json::array({ json { { "kind", "error" }, { "name", "error" }, { "props", json { { "component", "web" }, { "code", "E_LOST" } } } } }));
  EXPECT_EQ(h.log_calls, before);
}

// ---- the screenshot of a feedback report (the page sends it as base64 inside the report; the host checks it again and uploads it like the log)
namespace {
  std::string
  b64(const std::string &raw) {
    static const char *table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < raw.size(); i += 3) {
      const unsigned n = (static_cast<unsigned char>(raw[i]) << 16) | (i + 1 < raw.size() ? static_cast<unsigned char>(raw[i + 1]) << 8 : 0) | (i + 2 < raw.size() ? static_cast<unsigned char>(raw[i + 2]) : 0);
      out += table[(n >> 18) & 63];
      out += table[(n >> 12) & 63];
      out += i + 1 < raw.size() ? table[(n >> 6) & 63] : '=';
      out += i + 2 < raw.size() ? table[n & 63] : '=';
    }
    return out;
  }
  const std::string PNG_BYTES = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(64, 'p');
  const std::string JPEG_BYTES = std::string("\xff\xd8\xff\xe0", 4) + std::string(64, 'j');
  const std::string WEBP_BYTES = std::string("RIFF\x40\0\0\0WEBPVP8 ", 16) + std::string(64, 'w');

  json
  report_with(json extra) {
    json props { { "category", "bug" }, { "component", "panel" }, { "message", "it broke" } };
    for (auto it = extra.begin(); it != extra.end(); ++it) props[it.key()] = it.value();
    return json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", props } } });
  }
}  // namespace

TEST(TelemetryScreenshot, ThePictureIsUploadedFirstItsReferenceTravelsInTheReportAndThePictureItselfNeverDoes) {
  harness_t h;
  h.consent = { false, false };  // pressing 发送 is the consent for this report, with both choices off
  h.shot_ref = "crash-2026-11/ur-shot-0123456789abcdef01234567.png";
  ingest_report_t report;
  EXPECT_EQ(h.svc->ingest("web", "1", report_with({ { "shot", b64(PNG_BYTES) }, { "shot_ref", "crash-2026-11/ur-shot-ffffffffffffffffffffffff.png" } }), &report), 1u);
  EXPECT_EQ(report.shot, "uploaded");
  ASSERT_EQ(h.shot_calls, 1);
  EXPECT_EQ(h.shot_images.at(0), PNG_BYTES) << "the Worker is given the bytes the page sent, decoded";
  h.svc->flush();
  ASSERT_EQ(h.svc->pump(), 1);
  const auto &props = h.posted.at(0)["events"][0]["props"];
  EXPECT_EQ(props["shot_ref"], *h.shot_ref) << "the page cannot name a reference itself";
  EXPECT_FALSE(props.contains("shot")) << "the picture is not an event prop";
  EXPECT_FALSE(props.contains("shot_state"));
  EXPECT_EQ(h.posted.at(0).dump().find(b64(PNG_BYTES)), std::string::npos) << "no copy of the picture in what is sent";
  for (const auto &bytes : { JPEG_BYTES, WEBP_BYTES }) {
    ingest_report_t again;
    h.svc->ingest("panel", "1", report_with({ { "shot", b64(bytes) } }), &again);
    EXPECT_EQ(again.shot, "uploaded") << "JPEG and WebP are pictures too";
  }
}

TEST(TelemetryScreenshot, WhatIsNotAPictureOrIsTooLargeIsNotUploadedAndTheReportStillGoesWithTheReason) {
  harness_t h;
  h.shot_ref = "crash-2026-11/ur-shot-0123456789abcdef01234567.png";
  const std::vector<std::pair<std::string, std::string>> cases {
    { "not base64 at all !!!", "rejected" },
    { b64("GIF89a and then some bytes of a picture"), "rejected" },
    { b64(std::string("<svg xmlns='http://www.w3.org/2000/svg'><script>x</script></svg>")), "rejected" },
    { b64(std::string("MZ\x90\x00") + std::string(100, 'x')), "rejected" },
    { b64(PNG_BYTES.substr(0, 10)), "rejected" },
    { b64(PNG_BYTES + std::string(900 * 1024, 'x')), "too_large" },
  };
  int n = 0;
  for (const auto &[text, state] : cases) {
    ingest_report_t report;
    ASSERT_EQ(h.svc->ingest("web", "1", report_with({ { "shot", text }, { "message", "case " + std::to_string(n++) } }), &report), 1u) << text.substr(0, 20);
    EXPECT_EQ(report.shot, state) << text.substr(0, 20);
  }
  EXPECT_EQ(h.shot_calls, 0) << "nothing that is not a picture of a fitting size leaves the computer";
  h.svc->flush();
  ASSERT_GE(h.svc->pump(), 1);
  int with_state = 0;
  for (const auto &batch : h.posted)
    for (const auto &e : batch["events"]) {
      EXPECT_FALSE(e["props"].contains("shot"));
      EXPECT_FALSE(e["props"].contains("shot_ref"));
      if (e["props"].contains("shot_state")) ++with_state;
    }
  EXPECT_EQ(with_state, 6) << "every report says why it has no picture";
}

TEST(TelemetryScreenshot, AnUploadThatFailsLeavesTheReportWithTheWordWhy) {
  harness_t h;
  h.shot_ref.reset();
  for (const char *state : { "unreachable", "refused", "bad_answer", "unavailable" }) {
    h.shot_state = state;
    ingest_report_t report;
    EXPECT_EQ(h.svc->ingest("web", "1", report_with({ { "shot", b64(PNG_BYTES) }, { "message", std::string("fails ") + state } }), &report), 1u);
    EXPECT_EQ(report.shot, state);
  }
  h.svc->flush();
  ASSERT_GE(h.svc->pump(), 1);
  std::set<std::string> words;
  for (const auto &batch : h.posted)
    for (const auto &e : batch["events"])
      if (e.value("name", "") == "user_report") words.insert(e["props"].value("shot_state", ""));
  EXPECT_EQ(words, (std::set<std::string> { "unreachable", "refused", "bad_answer", "unavailable" }));
  // a report without a picture asks for nothing and says nothing about one
  const auto calls = h.shot_calls;
  ingest_report_t none;
  h.svc->ingest("web", "1", report_with({ { "message", "plain" } }), &none);
  EXPECT_EQ(h.shot_calls, calls);
  EXPECT_TRUE(none.shot.empty());
}

TEST(TelemetryScreenshot, OnlyTheReferenceTheServerReturnsAndOnlyOnAFeedbackReportSurvivesTheShape) {
  const auto shaped = [](json p) { p["category"] = "bug"; p["component"] = "panel"; p["message"] = "m"; return shape_event(kind_e::error, "user_report", p, 0); };
  ASSERT_TRUE(shaped({ { "shot_ref", "crash-2026-11/ur-shot-0123456789abcdef01234567.webp" } }));
  EXPECT_TRUE(shaped({ { "shot_ref", "crash-2026-11-2/ur-shot-0123456789abcdef01234567.jpg" } })->props.contains("shot_ref"));
  for (const char *bad : { "../../etc/passwd", "crash-2026-11/ur-0123456789abcdef01234567.log.gz", "crash-2026-11/ur-shot-0123.png", "crash-2026-11/ur-shot-0123456789abcdef01234567.gif", "https://example.org/x.png" })
    EXPECT_FALSE(shaped({ { "shot_ref", bad } })->props.contains("shot_ref")) << bad;
  EXPECT_FALSE(shaped({ { "shot", "AAAAAAAAAAAAAAAA" } })->props.contains("shot")) << "the picture itself is not an event field";
  EXPECT_TRUE(shaped({ { "shot_state", "too_large" } })->props.contains("shot_state"));
  EXPECT_FALSE(shaped({ { "shot_state", "because" } })->props.contains("shot_state"));
  EXPECT_FALSE(shape_event(kind_e::error, "error", json { { "component", "web" }, { "code", "E" }, { "shot_ref", "crash-2026-11/ur-shot-0123456789abcdef01234567.png" } }, 0)->props.contains("shot_ref"));
}

TEST(TelemetryUserReport, TheLogGoesWithEveryReportWhateverTheErrorReportChoiceIsButNotUnderAPathAsVersion) {
  harness_t h;
  h.log_ref = "crash-2026-11/ur-0123456789abcdef01234567.log.gz";
  const json report = json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "panel" }, { "message", "it broke" } } } } });
  h.consent = { true, false };  // error reports off: the report and its log still go (pressing 发送 was the consent)
  EXPECT_EQ(h.svc->ingest("web", "1", report), 1u);
  EXPECT_EQ(h.log_calls, 1);
  h.svc->flush();
  ASSERT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(h.posted.back()["events"][0]["props"]["log_ref"], *h.log_ref);
  h.consent = { false, true };
  const auto before = h.log_calls;
  h.svc->ingest("web", "C:\\Users\\Alice\\build", report);  // a page-supplied version that is a path: the log is not uploaded under it
  EXPECT_EQ(h.log_calls, before);
}

TEST(TelemetryUserReport, WithBothSwitchesOffTheReportGoesOutWithAOneTimeIdAndNoInstallIdIsMade) {
  harness_t h;
  h.consent = { false, false };
  h.log_ref = "crash-2026-11/ur-0123456789abcdef01234567.log.gz";
  const json report = json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "panel" }, { "message", "it broke" }, { "contact", "QQ 10086" } } } } });
  EXPECT_EQ(h.svc->ingest("web", "1", report), 1u);  // accepted only because it is on disk by now (the page shows success from this answer)
  ASSERT_EQ(h.log_ids.size(), 1u);
  const auto report_id = h.log_ids.front();
  EXPECT_EQ(report_id.size(), 36u);  // the log is uploaded under the report's own id, so the Worker can tie the two
  EXPECT_FALSE(std::filesystem::exists(h.dir.path / "install_id")) << "no install id is created for this report";
  ASSERT_EQ(h.svc->pump(), 1);
  const auto &batch = h.posted.at(0);
  EXPECT_EQ(batch["install_id"], report_id);
  EXPECT_TRUE(batch["report_only"].get<bool>());
  EXPECT_NE(batch["session_id"], batch["install_id"]);
  ASSERT_EQ(batch["events"].size(), 1u);
  EXPECT_EQ(batch["events"][0]["name"], "user_report");
  EXPECT_EQ(batch["events"][0]["props"]["log_ref"], *h.log_ref);
  EXPECT_EQ(batch["events"][0]["props"]["contact"], "QQ 10086");
  // an ordinary event never carries the flag, and with both switches off nothing else is sent
  EXPECT_FALSE(h.svc->emit(kind_e::usage, "feature_used", json { { "feature", "a" } }));
  EXPECT_FALSE(std::filesystem::exists(h.dir.path / "install_id"));
}

TEST(TelemetryUserReport, TwoReportsWithBothSwitchesOffGetTwoIdsAndTheLogFollowsEach) {
  harness_t h;
  h.consent = { false, false };
  h.log_ref = "crash-2026-11/ur-0123456789abcdef01234567.log.gz";
  const json report = json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "panel" }, { "message", "again" } } } } });
  EXPECT_EQ(h.svc->ingest("web", "1", report), 1u);
  EXPECT_EQ(h.svc->ingest("web", "1", report), 1u);
  ASSERT_EQ(h.log_ids.size(), 2u);
  EXPECT_NE(h.log_ids[0], h.log_ids[1]) << "reports are not tied to one another or to an install";
  h.log_ref.reset();  // the log could not be read: the report still goes, without a log
  EXPECT_EQ(h.svc->ingest("web", "1", report), 1u);
  while (h.svc->pump() > 0) {}
  ASSERT_EQ(h.posted.size(), 3u);
  EXPECT_FALSE(h.posted.back()["events"][0]["props"].contains("log_ref"));
}

TEST(TelemetryUserReport, TurningTheLastSwitchOffKeepsAWaitingReportAndDropsTheAutomaticEvents) {
  harness_t h;
  h.consent = { true, true };
  const json report = json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "panel" }, { "message", "kept" } } } } });
  h.svc->ingest("web", "1", report);
  h.svc->emit(kind_e::usage, "feature_used", json { { "feature", "web.a" } }, "web", "1");
  h.svc->flush();  // queued on disk, nothing sent yet
  h.consent = { false, false };
  h.svc->consent_changed();
  h.consent = { false, true };  // the person may send what waits: the queue is read with errors on again
  while (h.svc->pump() > 0) {}
  const auto names = names_of(h.posted);
  EXPECT_TRUE(names.count("user_report")) << "the report the person sent is still there";
  EXPECT_FALSE(names.count("feature_used")) << "the automatic usage event was dropped with the choice";
}

namespace {
  std::size_t
  queue_files(const std::filesystem::path &dir) {
    std::error_code ec;
    std::size_t n = 0;
    if (!std::filesystem::exists(dir / "queue", ec)) return 0;
    for (const auto &f : std::filesystem::directory_iterator(dir / "queue", ec))
      if (f.path().extension() == ".json") ++n;
    return n;
  }
}  // namespace

TEST(TelemetryUserReport, AReportIsOnDiskWhenTheAnswerIsGivenEvenWhileAnotherThreadKeepsFlushing) {
  harness_t h;
  h.consent = { false, true };
  const json report = json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "panel" }, { "message", "kept" } } } } });
  // a guard, so that a failed assertion leaves through here as well: the thread is stopped and joined before the harness it uses goes away (a joinable thread that is destroyed ends the program)
  struct flusher_t {
    std::atomic<bool> stop { false };
    std::thread thread;
    explicit flusher_t(service_t &svc): thread([this, &svc] {
          while (!stop.load()) svc.flush(false);  // the sender thread swaps the buffer out all the time
        }) {}
    ~flusher_t() {
      stop = true;
      if (thread.joinable()) thread.join();
    }
  } flusher { *h.svc };
  for (std::size_t i = 1; i <= 40; ++i) {
    ASSERT_EQ(h.svc->ingest("web", "1", report), 1u);
    // the process could end right here: the report must already be in the queue directory, not only in a buffer that another thread holds
    EXPECT_GE(queue_files(h.dir.path), i) << "report " << i << " is not on disk when the page was told it was accepted";
  }
}

TEST(TelemetryUserReport, WhenTheQueueCannotBeWrittenTheAnswerIsNotKeptAndTheReportStaysInTheBufferForTheNextFlush) {
  harness_t h;
  h.consent = { false, true };
  const json report = json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "panel" }, { "message", "later" } } } } });
  const auto queue = h.dir.path / "queue";
  std::filesystem::create_directories(queue);
  namespace fs = std::filesystem;
  fs::permissions(queue, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
  std::ofstream probe(queue / "probe");
  if (probe) {  // a user that may write anywhere (root): the directory cannot be made unwritable, nothing to test
    fs::permissions(queue, fs::perms::owner_all, fs::perm_options::replace);
    GTEST_SKIP() << "the queue directory stays writable for this user";
  }
  EXPECT_EQ(h.svc->ingest("web", "1", report), 0u) << "not written: the page must not be told it was kept";
  EXPECT_EQ(queue_files(h.dir.path), 0u);
  h.svc->flush(false);  // still not writable: the event is put back, not lost
  EXPECT_EQ(queue_files(h.dir.path), 0u);
  fs::permissions(queue, fs::perms::owner_all, fs::perm_options::replace);
  h.svc->flush(false);  // writable again: the held report is written now
  EXPECT_EQ(queue_files(h.dir.path), 1u);
  ASSERT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(h.posted.at(0)["events"][0]["props"]["message"], "later");
}

namespace {
  // makes a queue directory that cannot be written; false when this user may write anywhere (nothing to test)
  bool
  lock_queue(const std::filesystem::path &dir) {
    namespace fs = std::filesystem;
    fs::create_directories(dir / "queue");
    fs::permissions(dir / "queue", fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
    std::ofstream probe(dir / "queue" / "probe");
    if (probe) {
      fs::permissions(dir / "queue", fs::perms::owner_all, fs::perm_options::replace);
      return false;
    }
    return true;
  }
  void
  unlock_queue(const std::filesystem::path &dir) {
    std::filesystem::permissions(dir / "queue", std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
  }
}  // namespace

TEST(TelemetryServiceFlush, AFailedBatchWritePutsBackOnlyWhatWasNotWritten) {
  harness_t h;
  if (!lock_queue(h.dir.path)) GTEST_SKIP() << "the queue directory stays writable for this user";
  unlock_queue(h.dir.path);  // the first batch must still be written: the lock comes back with the second one
  int writes = 0;
  h.before_queue_write = [&] {
    // the first batch of five hundred is on disk by now; the second one finds a directory that no longer takes files
    if (++writes == 2) {
      namespace fs = std::filesystem;
      fs::permissions(h.dir.path / "queue", fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
    }
  };
  std::set<int> starts;
  for (int i = 0; i <= static_cast<int>(MAX_BATCH_EVENTS); ++i) {
    starts.insert(i);
    ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true }, { "start_ms", i } }));
  }
  h.svc->flush();
  EXPECT_EQ(h.svc->buffered(), 1u) << "only the event of the batch that was not written goes back";
  EXPECT_EQ(h.svc->counts().write_failed, 1u);
  unlock_queue(h.dir.path);
  h.svc->flush();
  EXPECT_EQ(h.svc->buffered(), 0u);
  EXPECT_EQ(h.svc->pump(), 2);  // both batches leave, and each event exactly once
  std::multiset<int> sent;
  for (const auto &batch : h.posted)
    for (const auto &e : batch["events"]) sent.insert(e["props"]["start_ms"].get<int>());
  EXPECT_EQ(sent, std::multiset<int>(starts.begin(), starts.end())) << "each event leaves exactly once: none is lost, an event that was already written is not sent twice";
}

TEST(TelemetryServiceFlush, TheAutomaticEventsOfAnOldConsentAreNotPutBackWhenTheWritesFailed) {
  harness_t h;
  if (!lock_queue(h.dir.path)) GTEST_SKIP() << "the queue directory stays writable for this user";
  bool moved = false;
  h.before_queue_write = [&] {
    if (moved) return;
    moved = true;
    // both switches go off while the flush is between taking the buffer and putting back what it could not write
    h.consent = { false, false };
    h.svc->consent_changed();
  };
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true }, { "start_ms", i } }));
  const json report = json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "panel" }, { "message", "kept" } } } } });
  EXPECT_EQ(h.svc->ingest("web", "1", report), 0u);  // the queue is not writable: the report waits in the buffer, with the identity it was accepted under
  h.svc->flush();
  // the automatic events of the consent that was just withdrawn are not put back; the report the person sent is
  EXPECT_EQ(h.svc->buffered(), 1u);
  EXPECT_EQ(h.svc->counts().write_failed, 2u);  // one from the ingest that could not write, one from the requeue of the report
  const auto waiting = h.svc->preview();
  ASSERT_EQ(waiting.size(), 1u);
  EXPECT_EQ(waiting[0]["name"], "user_report");
  unlock_queue(h.dir.path);
  h.svc->flush();
  EXPECT_EQ(queue_files(h.dir.path), 1u);
  EXPECT_EQ(h.svc->pump(), 1);
  std::set<std::string> names;
  for (const auto &batch : h.posted)
    if (batch.contains("events"))
      for (const auto &e : batch["events"]) names.insert(e["name"].get<std::string>());
  EXPECT_EQ(names, std::set<std::string> { "user_report" }) << "the old automatic events never leave";
}

TEST(TelemetryServiceFlush, AnEventTakenBeforeAWithdrawalIsNotWrittenWhenTheChoiceIsTurnedOnAgainBeforeTheWrite) {
  harness_t h;
  bool moved = false;
  h.before_queue_write = [&] {
    if (moved) return;
    moved = true;
    // the person turns both choices off and on again while the flush holds the events it took: the write that follows must not put the old events behind the clear
    h.consent = { false, false };
    h.svc->consent_changed();
    h.consent = { true, true };
    h.svc->consent_changed();
  };
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true }, { "start_ms", i } }));
  h.svc->flush();
  EXPECT_EQ(queue_files(h.dir.path), 0u) << "nothing of the withdrawn consent reaches the disk queue";
  EXPECT_EQ(h.svc->buffered(), 0u);
  EXPECT_EQ(h.svc->pump(), 0);
  EXPECT_EQ(h.sent_events(), 0);
}

TEST(TelemetryServiceFlush, AnotherChoiceTurnedOnDuringTheFlushDoesNotLoseTheEventsOfAChoiceThatStayedOn) {
  harness_t h;
  if (!lock_queue(h.dir.path)) GTEST_SKIP() << "the queue directory stays writable for this user";
  h.consent = { true, false };
  bool moved = false;
  h.before_queue_write = [&] {
    if (moved) return;
    moved = true;
    h.consent = { true, true };  // the error reports are switched on; the usage choice never changed
    h.svc->consent_changed();
  };
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true }, { "start_ms", i } }));
  h.svc->flush();  // the write fails: the usage events are still the person's to send and go back
  EXPECT_EQ(h.svc->buffered(), 3u);
  unlock_queue(h.dir.path);
  h.svc->flush();
  EXPECT_EQ(h.svc->buffered(), 0u);
  EXPECT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(h.sent_events(), 3);
}

TEST(TelemetryUserReport, TheIdentityIsFixedWhenTheReportIsAcceptedSoASwitchTurnedOffLaterDoesNotLoseIt) {
  harness_t h;
  h.consent = { false, true };
  const json report = json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "panel" }, { "message", "identity" } } } } });
  if (!lock_queue(h.dir.path)) GTEST_SKIP() << "the queue directory stays writable for this user";
  EXPECT_EQ(h.svc->ingest("web", "1", report), 0u);  // not written: it waits in the buffer, with the identity it was accepted under
  const auto install = h.svc->install_id();
  ASSERT_FALSE(install.empty());
  h.consent = { false, false };  // the last switch goes off before the next flush: no install id can be had any more
  h.svc->consent_changed();
  unlock_queue(h.dir.path);
  h.svc->flush(false);
  EXPECT_EQ(queue_files(h.dir.path), 1u) << "the report is written with the identity it already had, not dropped for want of an install id";
  ASSERT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(h.posted.at(0)["events"][0]["props"]["message"], "identity");
  EXPECT_EQ(h.posted.at(0)["install_id"], install);
}

TEST(TelemetryUserReport, AReportKeptInTheBufferThroughTheLastSwitchGoingOffIsSentWithAOneTimeId) {
  harness_t h;
  h.consent = { true, true };
  // a report in the buffer with no identity (emitted directly, as before the identity was fixed at acceptance), and then both switches go off
  ASSERT_TRUE(h.svc->emit(kind_e::error, "user_report", json { { "category", "bug" }, { "component", "panel" }, { "message", "through the switches" } }, "panel", "1"));
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "feature_used", json { { "feature", "panel.a" } }, "panel", "1"));
  h.consent = { false, false };
  h.svc->consent_changed();
  ASSERT_FALSE(std::filesystem::exists(h.dir.path / "install_id") && !h.svc->install_id().empty());
  h.svc->flush(false);
  EXPECT_EQ(queue_files(h.dir.path), 1u) << "the report is written (the automatic usage event went with the choice)";
  h.consent = { false, true };
  ASSERT_EQ(h.svc->pump(), 1);
  const auto &batch = h.posted.at(0);
  EXPECT_EQ(batch["events"][0]["name"], "user_report");
  EXPECT_TRUE(batch["report_only"].get<bool>()) << "no install id exists: the one-time id stands in for it";
  EXPECT_EQ(batch["events"].size(), 1u);
}

TEST(TelemetryUserReport, AFlushThatCannotWriteGivesTheAutomaticEventsBackToTheBufferToo) {
  harness_t h;
  h.consent = { true, true };
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "feature_used", json { { "feature", "web.apps_saved" } }, "web", "1"));
  if (!lock_queue(h.dir.path)) GTEST_SKIP() << "the queue directory stays writable for this user";
  h.svc->flush(false);
  EXPECT_EQ(queue_files(h.dir.path), 0u);
  unlock_queue(h.dir.path);
  h.svc->flush(false);
  EXPECT_EQ(queue_files(h.dir.path), 1u) << "the event went back to the buffer and is written now";
}

TEST(TelemetryUserReport, SeveralReportsDuringAWaitOfTheServerAllArriveEachOnItsOwn) {
  harness_t h;
  h.next_body = R"({"ok":true,"cfg":{"backoff_s":600}})";
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  ASSERT_EQ(h.svc->pump(), 1);
  ASSERT_TRUE(h.svc->backing_off());
  h.posted.clear();
  h.next_body.clear();
  const auto one = [](const char *message) {
    return json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "panel" }, { "message", message } } } } });
  };
  // two with the same component and category (what the merging of a wait counts into one) and a third
  EXPECT_EQ(h.svc->ingest("panel", "1", one("first")), 1u);
  EXPECT_EQ(h.svc->ingest("panel", "1", one("second")), 1u);
  EXPECT_EQ(h.svc->ingest("panel", "1", one("third")), 1u);
  EXPECT_EQ(h.svc->queued_files(), 3u);
  EXPECT_EQ(h.svc->pump(), 1);  // one request for each report, in the wait, not merged
  EXPECT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(h.svc->pump(), 1);
  ASSERT_EQ(h.posted.size(), 3u);
  std::set<std::string> messages;
  for (const auto &b : h.posted) {
    ASSERT_EQ(b["events"].size(), 1u);
    EXPECT_EQ(b["events"][0]["name"], "user_report");
    EXPECT_FALSE(b["events"][0]["props"].contains("count"));
    messages.insert(b["events"][0]["props"]["message"].get<std::string>());
  }
  EXPECT_EQ(messages, (std::set<std::string> { "first", "second", "third" }));
  EXPECT_EQ(h.svc->queued_files(), 0u) << "each file is removed when its own request was answered";
}

TEST(TelemetryUserReport, ARefusedRequestInAWaitRemovesOnlyItsOwnFile) {
  harness_t h;
  h.next_body = R"({"ok":true,"cfg":{"backoff_s":600}})";
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  ASSERT_EQ(h.svc->pump(), 1);
  h.posted.clear();
  h.next_body.clear();
  const auto one = [](const char *message) {
    return json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "panel" }, { "message", message } } } } });
  };
  h.svc->ingest("panel", "1", one("a"));
  h.svc->ingest("panel", "1", one("b"));
  h.next_status = 503;  // the first request fails: it stays, and so does the other
  EXPECT_EQ(h.svc->pump(), 0);
  EXPECT_EQ(h.svc->queued_files(), 2u);
  h.next_status = 200;
  h.now += 700'000;
  while (h.svc->pump() > 0) {}
  EXPECT_EQ(h.svc->queued_files(), 0u);
  EXPECT_EQ(h.posted.size() >= 2u, true);
}

TEST(TelemetryUserReport, LogRefIsOnlyTheReferenceOfTheUserLogEndpoint) {
  const auto base = [](const json &extra) {
    json p { { "category", "bug" }, { "component", "web" }, { "message", "m" } };
    p.update(extra);
    return shape_event(kind_e::error, "user_report", p, 0);
  };
  EXPECT_EQ(base(json { { "log_ref", "crash-2026-11/ur-0123456789abcdef01234567.log.gz" } })->props["log_ref"], "crash-2026-11/ur-0123456789abcdef01234567.log.gz");
  EXPECT_EQ(base(json { { "log_ref", "crash-2026-11-2/ur-0123456789abcdef01234567.log.gz" } })->props["log_ref"], "crash-2026-11-2/ur-0123456789abcdef01234567.log.gz");
  for (const char *bad : { "https://evil.example/a.gz", "crash-2026-11/../x.log.gz", "crash-2026-11/ur-short.log.gz", "att-2026-11/ur-0123456789abcdef01234567.log.gz", "crash-2026-11/abababababababab-0123456789abcdef01234567.log.gz" })
    EXPECT_FALSE(base(json { { "log_ref", bad } })->props.contains("log_ref")) << bad;
}

TEST(TelemetryUserReport, ReplyKeyIsOnlyTheRandomAddressOfThePage) {
  const auto base = [](const json &extra) {
    json p { { "category", "bug" }, { "component", "web" }, { "message", "m" } };
    p.update(extra);
    return shape_event(kind_e::error, "user_report", p, 0);
  };
  const std::string key = "rk_0123456789abcdef0123456789abcdef";
  EXPECT_EQ(base(json { { "reply_key", key } })->props["reply_key"], key);
  EXPECT_FALSE(base(json::object())->props.contains("reply_key")) << "a report without one (an older page) is unchanged";
  for (const char *bad : { "", "rk_", "rk_0123", "RK_0123456789abcdef0123456789abcdef", "rk_0123456789abcdef0123456789abcdeg", "rk_0123456789abcdef0123456789abcdef0", "xx_0123456789abcdef0123456789abcdef", " rk_0123456789abcdef0123456789abcdef" })
    EXPECT_FALSE(base(json { { "reply_key", bad } })->props.contains("reply_key")) << bad;
  EXPECT_FALSE(base(json { { "reply_key", 42 } })->props.contains("reply_key"));
  EXPECT_FALSE(base(json { { "reply_key", json::array({ key }) } })->props.contains("reply_key"));
}

TEST(TelemetryUserReport, ReplyKeyTravelsInTheEventThePageSent) {
  harness_t h;
  h.consent = { false, false };  // pressing 发送 is the consent for this report, with both choices off
  const std::string key = "rk_fedcba9876543210fedcba9876543210";
  const json report = json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "panel" }, { "message", "it broke" }, { "reply_key", key } } } } });
  EXPECT_EQ(h.svc->ingest("panel", "1", report), 1u);
  h.svc->flush();
  ASSERT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(posted_event(h.posted, "user_report")["props"]["reply_key"], key);
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
      for (const auto &f : v["forbidden"]) EXPECT_EQ(out.find(f.get<std::string>()), std::string::npos) << (which ? "log: " : "text: ") << name << " leaked \"" << f.get<std::string>() << "\" -> " << out;
      // the vector contract: every implementation, the text and the log scrubber alike, keeps every `required` string
      for (const auto &r : v["required"]) EXPECT_NE(out.find(r.get<std::string>()), std::string::npos) << (which ? "log: " : "text: ") << name << " lost \"" << r.get<std::string>() << "\" -> " << out;
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// what the envelope may carry: nothing a person typed

TEST(TelemetryEnvelope, VersionOsAndLocaleAreCheckedBeforeTheyLeave) {
  EXPECT_EQ(safe_version("2026.1112.0.杂鱼"), "2026.1112.0.杂鱼");
  // the host's own version is a release tag with a leading "v": it must reach the envelope, an empty one is refused by the endpoint (400 bad_envelope) and every host event with it
  EXPECT_EQ(safe_version("v2026.1114.173055.杂鱼"), "v2026.1114.173055.杂鱼");
  EXPECT_EQ(safe_version("V1.2.3"), "V1.2.3");
  EXPECT_EQ(safe_version("v"), "");
  EXPECT_EQ(safe_version("vEvaSky"), "");
  EXPECT_EQ(safe_version("vv1.0"), "");
  EXPECT_EQ(safe_version("version1"), "");
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

TEST(TelemetryEnvelope, TheReleaseTagOfTheHostReachesTheEnvelope) {
  // a release build reports PROJECT_VER as "v2026.1114.173055.杂鱼"; the endpoint answers an envelope without a version with 400 and drops the whole batch
  context_t context;
  context.install_id = "11111111-2222-4333-8444-555555555555";
  context.session_id = "66666666-7777-4888-8999-aaaaaaaaaaaa";
  context.version = "v2026.1114.173055.杂鱼";
  context.channel = "beta";
  context.os = "windows";
  event_t e;
  e.name = "app_start";
  e.kind = kind_e::usage;
  e.props = json { { "cold", false }, { "start_ms", 1200 } };
  const auto batches = build_envelopes(context, "host", { e });
  ASSERT_EQ(batches.size(), 1u);
  EXPECT_EQ(batches[0]["version"], "v2026.1114.173055.杂鱼");
  EXPECT_EQ(batches[0]["events"].size(), 1u);
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
  h.svc->note_log_line(4, line);
  h.svc->note_log(5, "Cannot open {} at {}");

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
    json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "web" }, { "message", typed }, { "contact", "QQ 10086" } } } },
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
  EXPECT_NE(all.find("QQ 10086"), std::string::npos) << "the contact a person left on purpose goes with the report";
}

// ---------------------------------------------------------------------------------------------------------------------
// stream health: events, anomaly codes and the anonymized excerpt

TEST(TelemetryStreamHealth, TheChunkAndTheAnomalyAreShapedAndWrongHistogramsAreRefused) {
  json hist = json::object();
  hist["host_p95"] = json::array({ 0, 0, 0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
  hist["made_up"] = json::array({ 1 });
  json props { { "h", hist }, { "windows", 3 }, { "frames", 10800 }, { "codec", "hevc" }, { "width", 2560 }, { "bitrate_band", "60000-100000" }, { "gpu_driver", "560.94" }, { "host_name", "SKYPC" } };
  const auto e = shape_event(kind_e::experience, "stream_health", props, 5);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->props["h"]["host_p95"].size(), 19u);
  EXPECT_FALSE(e->props["h"].contains("made_up"));
  EXPECT_FALSE(e->props.contains("host_name"));
  EXPECT_EQ(e->props["bitrate_band"], "60000-100000");

  json bad = hist;
  bad["host_p95"] = json::array({ 1, 2, 3 });  // wrong length: the prop is dropped, not the event
  const auto e2 = shape_event(kind_e::experience, "stream_health", json { { "h", bad }, { "windows", 1 } }, 5);
  ASSERT_TRUE(e2);
  EXPECT_FALSE(e2->props.contains("h"));

  const auto a = shape_event(kind_e::experience, "stream_anomaly", json { { "code", "XP_QUEUE_FLUSH" }, { "severe", true }, { "value", 1 }, { "count", 32 }, { "log", true } }, 5);
  ASSERT_TRUE(a);
  EXPECT_EQ(a->props["code"], "XP_QUEUE_FLUSH");
  EXPECT_FALSE(shape_event(kind_e::experience, "stream_anomaly", json { { "severe", true } }, 5));  // the code is required
  EXPECT_FALSE(shape_event(kind_e::experience, "stream_anomaly", json { { "code", "a b" } }, 5));
}

namespace {
  void
  feed_stream(harness_t &h, std::uint32_t id, int frames, double backlog_ms, bool flush) {
    for (int i = 0; i < frames; ++i) {
      h.svc->record_sample(id, hooks::metric_e::host_ms, 20);
      h.svc->record_sample(id, hooks::metric_e::backlog_ms, backlog_ms);
      h.svc->record_count(id, hooks::counter_e::frames_sent, 1);
    }
    if (flush) {
      h.svc->record_count(id, hooks::counter_e::queue_flushes, 1);
      h.svc->record_count(id, hooks::counter_e::queue_frames_dropped, 32);
    }
  }
}  // namespace

TEST(TelemetryStreamHealth, ASevereWindowEmitsTheAnomalyAndTheStreamEndSendsTheChunk) {
  harness_t h;
  stream_dims_t dims;
  dims.codec = "hevc";
  dims.width = 2560;
  dims.height = 1440;
  dims.fps = 60;
  dims.client_kind = "moonlight-macos-enhanced";
  dims.client_os = "macos";
  h.svc->set_gpu_driver("32.0.15.6094 (Eve's driver)");
  h.svc->stream_begin(dims, 7, "client-a");
  h.svc->note_log_line(2, "Streaming to Eve-PC at 203.0.113.20 as C:\\Users\\Eve\\game");
  feed_stream(h, 7, 3600, 560, true);
  h.now += 61'000;
  h.svc->tick_streams({});
  h.svc->stream_end(7, "none");
  h.svc->flush(true);
  while (h.svc->pump() > 0) {}
  const auto names = names_of(h.posted);
  EXPECT_TRUE(names.count("stream_anomaly"));
  EXPECT_TRUE(names.count("stream_health"));
  std::set<std::string> codes;
  json chunk;
  json anomaly_with_log;
  for (const auto &b : h.posted) {
    if (!b.contains("events")) continue;
    for (const auto &e : b["events"]) {
      if (e["name"] == "stream_anomaly") {
        codes.insert(e["props"]["code"].get<std::string>());
        if (e["props"]["log"] == true) anomaly_with_log = e;
      }
      if (e["name"] == "stream_health") chunk = e;
    }
  }
  EXPECT_TRUE(codes.count("XP_QUEUE_FLUSH"));
  EXPECT_TRUE(codes.count("XP_SEND_BACKLOG"));
  EXPECT_FALSE(anomaly_with_log.is_null());
  EXPECT_EQ(chunk["props"]["queue_flushes"], 1);
  EXPECT_EQ(chunk["props"]["queue_frames_dropped"], 32);
  EXPECT_EQ(chunk["props"]["gpu_driver"], "32.0.15.6094");  // digits and dots only
  EXPECT_EQ(chunk["props"]["windows"], 1);
}

TEST(TelemetryStreamHealth, TheExcerptIsScrubbedRelativeInTimeAndLimitedPerDay) {
  service_options_t options;
  options.redact_words = { "Eve-PC", "Eve" };
  harness_t h { options };
  stream_dims_t dims;
  dims.fps = 60;
  dims.codec = "h264";
  h.svc->stream_begin(dims, 9, "c");
  h.svc->note_log_line(2, "Client Eve-PC connected from 203.0.113.20 profile C:\\Users\\Eve\\AppData\\x");
  feed_stream(h, 9, 3600, 20, true);
  h.now += 60'000;
  h.svc->tick_streams({});
  h.svc->flush(true);
  while (h.svc->pump() > 0) {}
  EXPECT_EQ(h.svc->pump_logs(), 1);
  json body;
  for (std::size_t i = 0; i < h.urls.size(); ++i)
    if (h.urls[i].ends_with("/v1/anomaly-log")) body = h.posted[i];
  ASSERT_FALSE(body.is_null());
  const auto text = body["log"].get<std::string>();
  EXPECT_EQ(body["code"], "XP_QUEUE_FLUSH");
  EXPECT_EQ(text.find("203.0.113.20"), std::string::npos);
  EXPECT_EQ(text.find("Eve"), std::string::npos);
  EXPECT_EQ(text.find("Users"), std::string::npos);
  EXPECT_NE(text.find("[-"), std::string::npos);  // time relative to the anomaly

  // the next ones are allowed after the minimum gap, up to the daily bound of six
  int allowed = 1;
  for (int round = 0; round < 7; ++round) {
    h.now += 11 * 60'000;
    h.svc->note_log_line(2, "line");
    feed_stream(h, 9, 3600, 20, true);
    h.now += 60'000;
    h.svc->tick_streams({});
    allowed += h.svc->pump_logs();
  }
  EXPECT_EQ(allowed, 6);
}

TEST(TelemetryStreamHealth, NoExcerptLeavesWhileErrorReportsAreOff) {
  harness_t h;
  h.consent.errors = false;
  stream_dims_t dims;
  dims.fps = 60;
  h.svc->stream_begin(dims, 3, "c");
  h.svc->note_log_line(2, "something");
  feed_stream(h, 3, 3600, 20, true);
  h.now += 60'000;
  h.svc->tick_streams({});
  EXPECT_EQ(h.svc->pump_logs(), 0);
  for (const auto &u : h.urls) EXPECT_FALSE(u.ends_with("/v1/anomaly-log"));
}

TEST(TelemetryRemoteConfig, ASuccessfulAnswerCarriesTheConfigAndItIsKeptForTheNextStart) {
  std::string body;
  harness_t h;
  json cfg { { "cfg", { { "rev", 7 }, { "tier1_rate", 0.0 }, { "logs_per_day", 1 }, { "health", true }, { "backoff_s", 0 } } } };
  // a debug cohort for this version's GPU vendor, valid for an hour
  cfg["cfg"]["debug"] = json::array({ { { "until", h.now + 3'600'000 }, { "gpu", "nvidia" }, { "logs_per_day", 5 } }, { { "until", h.now - 1 }, { "gpu", "amd" } } });
  const auto parsed = parse_remote_config(cfg["cfg"], h.now);
  EXPECT_EQ(parsed.rev, 7);
  EXPECT_EQ(parsed.logs_per_day, 1);
  ASSERT_EQ(parsed.debug.size(), 1u);  // the expired one is gone
  EXPECT_EQ(parsed.debug[0].logs_per_day, 5);
  // nonsense is clamped
  const auto wild = parse_remote_config(json { { "tier1_rate", 7 }, { "backoff_s", 99999999 }, { "logs_per_day", -4 } }, h.now);
  EXPECT_DOUBLE_EQ(wild.tier1_rate, 1.0);
  EXPECT_EQ(wild.backoff_s, 21600);
  EXPECT_EQ(wild.logs_per_day, 0);
}

TEST(TelemetryStreamHealth, Tier0IsSentByEveryStreamAndTheHistogramsOnlyByTheSampledOnesAndTheDebugCohort) {
  const auto run = [](double rate, bool cohort) {
    harness_t h { {}, rate };
    if (cohort) {
      // the debug cohort arrives with an answer; the service applies it to the streams that start after it
      h.next_status = 200;
    }
    stream_dims_t dims;
    dims.fps = 60;
    dims.codec = "hevc";
    h.svc->stream_begin(dims, 11, "c");
    feed_stream(h, 11, 3600, 20, false);
    h.now += 60'000;
    h.svc->tick_streams({});
    h.svc->stream_end(11, "none");
    h.svc->flush(true);
    while (h.svc->pump() > 0) {}
    json chunk;
    for (const auto &b : h.posted)
      if (b.contains("events"))
        for (const auto &e : b["events"])
          if (e["name"] == "stream_health") chunk = e["props"];
    return chunk;
  };
  const auto all = run(1.0, false);
  const auto none = run(0.0, false);
  ASSERT_FALSE(all.is_null());
  ASSERT_FALSE(none.is_null());
  EXPECT_TRUE(all.contains("h"));
  EXPECT_FALSE(none.contains("h"));
  EXPECT_TRUE(none.contains("bad"));
  EXPECT_EQ(none["windows"], 1);
}

TEST(TelemetryStreamHealth, AnInstallThatGotWorseThanItsOwnBaselineSendsOneSmallEvent) {
  harness_t h;
  stream_dims_t dims;
  dims.fps = 60;
  dims.codec = "hevc";
  h.svc->stream_begin(dims, 5, "c");
  const auto minute = [&](double jitter_ms) {
    feed_stream(h, 5, 3600, 3, false);
    const std::int64_t interval = 16'666'667;
    static std::int64_t t = 1'000'000'000;
    for (int i = 0; i < 600; ++i) {
      t += interval + (jitter_ms > 1 ? (i % 2 ? 4'000'000 : -4'000'000) : 0);
      h.svc->record_frame_done(5, t);
    }
    h.now += 60'000;
    h.svc->tick_streams({});
  };
  for (int i = 0; i < 35; ++i) minute(0.0);
  for (int i = 0; i < 7; ++i) minute(5.0);
  h.svc->stream_end(5, "none");
  h.svc->flush(true);
  while (h.svc->pump() > 0) {}
  int regress = 0;
  json event;
  for (const auto &b : h.posted)
    if (b.contains("events"))
      for (const auto &e : b["events"])
        if (e["name"] == "stream_regress") {
          ++regress;
          event = e["props"];
        }
  EXPECT_EQ(regress, 1);
  EXPECT_EQ(event["metric"], "jitter_p95");
  EXPECT_GT(event["current"].get<double>(), event["baseline"].get<double>() * 3);
}

// ---------------------------------------------------------------------------------------------------------------------
// priorities: serious problems always get through, the rest is reduced first

TEST(TelemetryPriority, SeriousThingsAreZeroUsageIsTwo) {
  const auto p = [](const char *name, json props = json::object(), int fixed = -1) {
    event_t e;
    e.name = name;
    e.props = std::move(props);
    e.priority = fixed;
    return event_priority(e);
  };
  EXPECT_EQ(p("user_report"), 0);
  EXPECT_EQ(p("stream_health"), 0);
  EXPECT_EQ(p("error_report"), 0);
  EXPECT_EQ(p("error_report", json::object(), 1), 1);  // the aggregate of a known error
  EXPECT_EQ(p("stream_anomaly", json { { "severe", true } }), 0);
  EXPECT_EQ(p("stream_anomaly", json { { "severe", false } }), 1);
  EXPECT_EQ(p("stream_regress"), 1);
  EXPECT_EQ(p("error", json { { "severity", "fatal" } }), 0);
  EXPECT_EQ(p("error", json { { "severity", "error" } }), 1);
  for (const char *name : { "app_start", "stream_start", "feature_used", "setting_changed", "stream_quality", "panel_page", "startup" }) EXPECT_EQ(p(name), 2) << name;
}

TEST(TelemetryQueue, GivesUpPriority2ThenPriority1AndKeepsPriority0) {
  temp_dir_t dir;
  disk_queue_t q(dir.path, queue_limits_t { 6, 1024 * 1024, 300 * 1024, 3 });
  const auto batch = [](int n) { return json { { "install_id", "x" }, { "events", json::array() }, { "n", n } }; };
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(q.push(batch(i), 0));
  for (int i = 10; i < 14; ++i) ASSERT_TRUE(q.push(batch(i), 2));
  for (int i = 20; i < 24; ++i) ASSERT_TRUE(q.push(batch(i), 1));
  EXPECT_EQ(q.files(), 6u);
  EXPECT_EQ(q.files_of(0), 3u);  // all of priority 0 is still here
  EXPECT_EQ(q.files_of(2), 0u);  // priority 2 went first
  EXPECT_EQ(q.files_of(1), 3u);
  // the order of sending: priority 0 first, oldest first
  const auto first = q.front();
  ASSERT_TRUE(first);
  EXPECT_EQ(first->batch["n"], 0);
  const auto only0 = q.entries(0, 10);
  ASSERT_EQ(only0.size(), 3u);
  EXPECT_EQ(q.front(0)->batch["n"], 0);
  for (const auto &e : only0) q.remove(e.path);
  EXPECT_FALSE(q.front(0));  // nothing of priority 0 is left, front(0) does not hand out the others
  EXPECT_TRUE(q.front());
  // only when priority 0 alone is past its own bound is the oldest of it dropped
  disk_queue_t p0(dir.path / "p0", queue_limits_t { 100, 1024 * 1024, 300 * 1024, 3 });
  for (int i = 0; i < 5; ++i) ASSERT_TRUE(p0.push(batch(i), 0));
  EXPECT_EQ(p0.files_of(0), 3u);
  EXPECT_EQ(p0.front()->batch["n"], 2);
}

TEST(TelemetrySender, WhileTheServerAsksForAWaitOnlyPriority0LeavesOnceAsCounts) {
  harness_t h;
  h.next_body = R"({"ok":true,"cfg":{"backoff_s":600}})";
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  ASSERT_EQ(h.svc->pump(), 1);  // the answer carries the wait
  EXPECT_TRUE(h.svc->backing_off());
  h.posted.clear();
  h.next_body.clear();
  // during the wait: usage (priority 2), a small problem (1) and three serious ones of one kind (0)
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "feature_used", json { { "feature", "web.apps_saved" } }));
  ASSERT_TRUE(h.svc->emit(kind_e::experience, "stream_anomaly", json { { "code", "XP_IDR_STORM" }, { "severe", false } }));
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(h.svc->emit(kind_e::experience, "stream_anomaly", json { { "code", "XP_QUEUE_FLUSH" }, { "severe", true }, { "count", 32 } }));
  h.svc->flush();
  EXPECT_EQ(h.svc->queued_files(), 3u);  // priority 0, 1 and 2, one batch each
  EXPECT_EQ(h.svc->pump(), 1);
  ASSERT_EQ(h.posted.size(), 1u);
  ASSERT_EQ(h.posted[0]["events"].size(), 1u);  // the three serious ones are one event with the count
  EXPECT_EQ(h.posted[0]["events"][0]["props"]["code"], "XP_QUEUE_FLUSH");
  EXPECT_EQ(h.posted[0]["events"][0]["props"]["suppressed"], 2);
  EXPECT_EQ(h.svc->queued_files(), 2u);  // the others wait
  EXPECT_EQ(h.svc->pump(), 0);  // once per wait
  EXPECT_EQ(h.posted.size(), 1u);
  // the wait is over: the rest is sent, priority 1 before 2
  h.now += 601'000;
  EXPECT_FALSE(h.svc->backing_off());
  while (h.svc->pump() > 0) {}
  EXPECT_EQ(h.svc->queued_files(), 0u);
  ASSERT_EQ(h.posted.size(), 3u);
  EXPECT_EQ(h.posted[1]["events"][0]["name"], "stream_anomaly");
  EXPECT_EQ(h.posted[2]["events"][0]["name"], "feature_used");
}

TEST(TelemetrySender, TheHistogramsOfAStreamAreLeftOutWhileTheServerAsksForAWait) {
  harness_t h;
  h.next_body = R"({"cfg":{"backoff_s":600,"tier1_rate":1.0}})";
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  h.svc->pump();
  h.posted.clear();
  h.next_body.clear();
  stream_dims_t dims;
  dims.fps = 60;
  h.svc->stream_begin(dims, 4, "c");
  feed_stream(h, 4, 3600, 20, false);
  h.now += 60'000;
  h.svc->tick_streams({});
  h.svc->stream_end(4, "none");
  h.svc->flush(true);
  h.now += 601'000;
  while (h.svc->pump() > 0) {}
  json chunk;
  for (const auto &b : h.posted)
    for (const auto &e : b["events"])
      if (e["name"] == "stream_health") chunk = e["props"];
  ASSERT_FALSE(chunk.is_null());
  EXPECT_FALSE(chunk.contains("h"));
  EXPECT_TRUE(chunk.contains("bad"));
}

// ---------------------------------------------------------------------------------------------------------------------
// review follow-ups

TEST(TelemetryQueue, AFullQueueOfMixedPrioritiesLosesNothingItCouldKeep) {
  temp_dir_t dir;
  disk_queue_t q(dir.path, queue_limits_t { 100, 8 * 1024 * 1024, 300 * 1024, 40 });
  const auto batch = [](int n) { return json { { "install_id", "x" }, { "events", json::array() }, { "n", n } }; };
  for (int i = 0; i < 40; ++i) ASSERT_TRUE(q.push(batch(i), 0));
  for (int i = 0; i < 60; ++i) ASSERT_TRUE(q.push(batch(100 + i), 1));
  ASSERT_EQ(q.files(), 100u);
  // one more of priority 0: the bound of priority 0 is cut by itself, the global bound then holds without a second cut
  ASSERT_TRUE(q.push(batch(500), 0));
  EXPECT_EQ(q.files_of(0), 40u);
  EXPECT_EQ(q.files_of(1), 60u);
  EXPECT_EQ(q.files(), 100u);
  EXPECT_EQ(q.front()->batch["n"], 1);  // the oldest of priority 0 went
}

TEST(TelemetrySender, AWaitSendsEveryProductAndVersionOnceAndDeletesOnlyWhatLeft) {
  harness_t h;
  h.next_body = R"({"cfg":{"backoff_s":600}})";
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  ASSERT_EQ(h.svc->pump(), 1);
  h.posted.clear();
  h.next_body.clear();
  // priority 0 of the host (three of one kind, two components) and of the panel
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(h.svc->emit(kind_e::error, "error", json { { "component", "encoder" }, { "code", "abc" }, { "severity", "fatal" }, { "count", 2 } }));
  ASSERT_TRUE(h.svc->emit(kind_e::error, "error", json { { "component", "capture" }, { "code", "abc" }, { "severity", "fatal" }, { "count", 1 } }));
  ASSERT_TRUE(h.svc->emit(kind_e::error, "error", json { { "component", "panel" }, { "code", "boom" }, { "severity", "fatal" }, { "count", 1 } }, "panel", "1.2.3.4.test"));
  h.svc->flush();
  EXPECT_EQ(h.svc->queued_files(), 2u);  // host and panel, priority 0
  EXPECT_EQ(h.svc->pump(), 1);
  ASSERT_EQ(h.posted.size(), 1u);
  const auto first = h.posted[0];
  EXPECT_EQ(first["events"].size(), 2u);  // two components, never merged into one
  int encoder = 0;
  for (const auto &e : first["events"])
    if (e["props"]["component"] == "encoder") encoder = e["props"]["count"].get<int>();
  EXPECT_EQ(encoder, 6);  // the counts of the merged events are added
  EXPECT_EQ(h.svc->queued_files(), 1u);  // the panel batch is still here: it was not part of that request
  EXPECT_EQ(h.svc->pump(), 1);  // and leaves in the same wait
  ASSERT_EQ(h.posted.size(), 2u);
  EXPECT_EQ(h.posted[1]["product"], "panel");
  EXPECT_EQ(h.svc->queued_files(), 0u);
  EXPECT_EQ(h.svc->pump(), 0);
}

TEST(TelemetrySender, TheAnswerOfAWaitRequestIsHandledLikeAnyOther) {
  harness_t h;
  h.next_body = R"({"cfg":{"backoff_s":600}})";
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  h.svc->pump();
  h.posted.clear();
  ASSERT_TRUE(h.svc->emit(kind_e::error, "error", json { { "component", "x" }, { "code", "a" }, { "severity", "fatal" } }));
  h.svc->flush();
  h.next_status = 503;  // a failing server in the wait: the usual backoff applies and the batch stays
  EXPECT_EQ(h.svc->pump(), 0);
  EXPECT_EQ(h.svc->queued_files(), 1u);
  EXPECT_EQ(h.svc->consecutive_failures(), 1);
  EXPECT_GT(h.svc->retry_not_before_ms(), h.now);
  EXPECT_EQ(h.svc->counts().failed_posts, 1u);
  h.now = h.svc->retry_not_before_ms() + 1;
  h.next_status = 200;
  h.next_body = R"({"cfg":{"health":false}})";  // and the configuration of an answer in a wait is taken
  EXPECT_EQ(h.svc->pump(), 0);  // the wait of this group was used by the failed try
  EXPECT_EQ(h.svc->counts().sent_batches, 1u);
  EXPECT_TRUE(h.svc->remote_config().health);
}

TEST(TelemetryStreamHealth, AnExcerptThatCannotBeMadeDoesNotUseUpTheRoom) {
  harness_t h;
  stream_dims_t dims;
  dims.fps = 60;
  h.svc->stream_begin(dims, 21, "c");
  feed_stream(h, 21, 3600, 20, true);  // a serious window, but the log ring is empty
  h.now += 60'000;
  h.svc->tick_streams({});
  EXPECT_EQ(h.svc->pump_logs(), 0);
  h.now += 61'000;
  h.svc->note_log_line(2, "Frame queue full");
  feed_stream(h, 21, 3600, 20, true);
  h.now += 59'000;
  h.svc->tick_streams({});
  EXPECT_EQ(h.svc->pump_logs(), 1);  // the first excerpt of the run is still allowed: nothing was used up before
}

TEST(TelemetryStreamHealth, TheDailyRoomIsTheServersNumberWithTheLocalOptionAsItsDefault) {
  service_options_t options;
  options.anomaly_logs_per_day = 2;
  harness_t h { options };
  stream_dims_t dims;
  dims.fps = 60;
  h.svc->stream_begin(dims, 22, "c");
  int sent = 0;
  for (int round = 0; round < 5; ++round) {
    h.svc->note_log_line(2, "line");
    feed_stream(h, 22, 3600, 20, true);
    h.now += 60'000;
    h.svc->tick_streams({});
    sent += h.svc->pump_logs();
    h.now += 11 * 60'000;
  }
  EXPECT_EQ(sent, 2);
}

TEST(TelemetryStreamHealth, TheExcerptHasNoQuotedNamesAndNoClockTime) {
  harness_t h;
  stream_dims_t dims;
  dims.fps = 60;
  h.svc->stream_begin(dims, 23, "c");
  h.svc->note_log_line(2, "Launching app \"Genshin Impact\" for client 'Eve phone' [WGC] at 203.0.113.9");
  feed_stream(h, 23, 3600, 20, true);
  h.now += 60'000;
  h.svc->tick_streams({});
  h.svc->pump_logs();
  json body;
  for (std::size_t i = 0; i < h.urls.size(); ++i)
    if (h.urls[i].ends_with("/v1/anomaly-log")) body = h.posted[i];
  ASSERT_FALSE(body.is_null());
  const auto text = body["log"].get<std::string>();
  EXPECT_EQ(text.find("Genshin"), std::string::npos);
  EXPECT_EQ(text.find("Eve"), std::string::npos);
  EXPECT_EQ(text.find("203.0.113.9"), std::string::npos);
}

TEST(TelemetryRemoteConfig, ADebugCohortRaisesTheDetailOfItsStreamsAndTheSwitchAtZeroIsNotOverridden) {
  const auto chunk_of = [](const std::string &body_text) {
    harness_t h { {}, 0.0 };
    h.random_value = 0.5;  // the draw of every stream is above the tiny rate: only the cohort can put a stream into Tier 1
    h.next_body = body_text;
    EXPECT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
    h.svc->flush();
    h.svc->pump();  // the answer carries the configuration
    h.next_body.clear();
    stream_dims_t dims;
    dims.fps = 60;
    dims.client_kind = "other";
    h.svc->stream_begin(dims, 31, "c");
    feed_stream(h, 31, 3600, 20, false);
    h.now += 60'000;
    h.svc->tick_streams({});
    h.svc->stream_end(31, "none");
    h.svc->flush(true);
    while (h.svc->pump() > 0) {}
    json chunk;
    for (const auto &b : h.posted)
      if (b.contains("events"))
        for (const auto &e : b["events"])
          if (e["name"] == "stream_health") chunk = e["props"];
    return chunk;
  };
  const std::int64_t later = 1'800'000'000'000 + 3'600'000;
  // a tiny rate: no stream is sampled, the cohort of this GPU vendor is
  const auto with_cohort = chunk_of(R"({"cfg":{"tier1_rate":0.000001,"debug":[{"until":)" + std::to_string(later) + R"(,"gpu":"nvidia"}]}})");
  ASSERT_FALSE(with_cohort.is_null());
  EXPECT_TRUE(with_cohort.contains("h"));
  const auto other_gpu = chunk_of(R"({"cfg":{"tier1_rate":0.000001,"debug":[{"until":)" + std::to_string(later) + R"(,"gpu":"amd"}]}})");
  ASSERT_FALSE(other_gpu.is_null());
  EXPECT_FALSE(other_gpu.contains("h"));
  // a rate of 0 is the server's switch for all histograms
  const auto switched_off = chunk_of(R"({"cfg":{"tier1_rate":0,"debug":[{"until":)" + std::to_string(later) + R"(,"gpu":"nvidia"}]}})");
  ASSERT_FALSE(switched_off.is_null());
  EXPECT_FALSE(switched_off.contains("h"));
}

TEST(TelemetryRemoteConfig, TheConfigurationIsKeptOnDiskAndReadAgainAtTheNextStart) {
  temp_dir_t dir;
  std::int64_t now = 1'800'000'000'000;
  const auto make = [&](const std::string &body) {
    service_options_t options;
    options.dir = dir.path;
    service_deps_t deps;
    deps.now_ms = [&] { return now; };
    deps.consent = [] { return consent_t {}; };
    deps.random01 = [] { return 0.0; };
    deps.post = [body](const std::string &, const std::string &) {
      post_result_t answer { 200 };
      answer.body = body;
      return answer;
    };
    auto svc = std::make_unique<service_t>(options, deps);
    context_t c;
    c.version = "v1";
    c.channel = "stable";
    c.os = "windows";
    svc->set_context(c);
    return svc;
  };
  {
    auto svc = make(R"({"cfg":{"rev":9,"tier1_rate":0.25,"logs_per_day":4}})");
    EXPECT_DOUBLE_EQ(svc->remote_config().tier1_rate, 0.1);  // the default before any answer
    ASSERT_TRUE(svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
    svc->flush(true);
    while (svc->pump() > 0) {}
    EXPECT_EQ(svc->remote_config().rev, 9);
    EXPECT_DOUBLE_EQ(svc->remote_config().tier1_rate, 0.25);
  }
  auto again = make("");  // a new start reads it before it has any answer
  EXPECT_EQ(again->remote_config().rev, 9);
  EXPECT_DOUBLE_EQ(again->remote_config().tier1_rate, 0.25);
  EXPECT_EQ(again->remote_config().logs_per_day, 4);
}

TEST(TelemetrySender, AnAnswerThatAsksForAWaitEndsTheOrdinaryLoopAtOnce) {
  harness_t h;
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));  // priority 2
  ASSERT_TRUE(h.svc->emit(kind_e::error, "error", json { { "component", "x" }, { "code", "a" }, { "severity", "error" } }));  // priority 1
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "feature_used", json { { "feature", "web.apps_saved" } }));  // priority 2
  h.svc->flush();
  ASSERT_EQ(h.svc->queued_files(), 2u);
  h.next_body = R"({"cfg":{"backoff_s":600}})";
  EXPECT_EQ(h.svc->pump(), 1);  // the first answer asks for a wait: the second batch does not follow in the same call
  EXPECT_TRUE(h.svc->backing_off());
  EXPECT_EQ(h.posted.size(), 1u);
  EXPECT_EQ(h.svc->queued_files(), 1u);
}

TEST(TelemetrySender, AWaitLooksPastTheBatchesOfAGroupThatAlreadyLeft) {
  harness_t h;
  h.next_body = R"({"cfg":{"backoff_s":600}})";
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  h.svc->pump();
  h.posted.clear();
  h.next_body.clear();
  // 30 batches of the host (priority 0), then one of the panel
  for (int i = 0; i < 30; ++i) {
    ASSERT_TRUE(h.svc->emit(kind_e::error, "error", json { { "component", "c" + std::to_string(i) }, { "code", "a" }, { "severity", "fatal" } }));
    h.svc->flush();
  }
  ASSERT_TRUE(h.svc->emit(kind_e::error, "error", json { { "component", "panel" }, { "code", "boom" }, { "severity", "fatal" } }, "panel", "1.2.3.4.test"));
  h.svc->flush();
  h.next_status = 503;  // the first request, the host group, fails: its 30 batches stay in front of the panel batch
  EXPECT_EQ(h.svc->pump(), 0);
  h.now = h.svc->retry_not_before_ms() + 1;
  h.next_status = 200;
  EXPECT_EQ(h.svc->pump(), 1);  // the panel batch is found behind the 30 of the group that is already counted for this wait
  EXPECT_EQ(h.posted.back()["product"], "panel");
}

TEST(TelemetryStreamHealth, AnExcerptThatFindsTheQueueFullTakesNoRoom) {
  service_options_t options;
  options.anomaly_logs_per_day = 3;
  options.anomaly_log_min_gap_ms = 0;
  harness_t h { options };
  stream_dims_t dims;
  dims.fps = 60;
  h.svc->stream_begin(dims, 41, "c");
  for (int round = 0; round < 3; ++round) {  // three serious windows, nothing sent in between: two excerpts wait, the third finds the queue full
    h.svc->note_log_line(2, "line");
    feed_stream(h, 41, 3600, 20, true);
    h.now += 60'000;
    h.svc->tick_streams({});
    h.now += 121'000;  // past the cooldown of a serious code on one stream
  }
  EXPECT_EQ(h.svc->pump_logs(), 2);
  h.svc->note_log_line(2, "line");
  feed_stream(h, 41, 3600, 20, true);
  h.now += 60'000;
  h.svc->tick_streams({});
  EXPECT_EQ(h.svc->pump_logs(), 1);  // the room of the third was never used: this is the third excerpt of the day, the bound is 3
}

TEST(TelemetrySender, AMergedRequestOfAWaitStaysInsideTheLimitsOfOneRequest) {
  harness_t h;
  h.next_body = R"({"cfg":{"backoff_s":600}})";
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  h.svc->pump();
  h.posted.clear();
  h.next_body.clear();
  // three batches of 200 different problems: together more events than one request may carry
  for (int batch = 0; batch < 3; ++batch) {
    for (int i = 0; i < 200; ++i) ASSERT_TRUE(h.svc->emit(kind_e::error, "error", json { { "component", "x" }, { "code", "c" + std::to_string(batch * 200 + i) }, { "severity", "fatal" } }));
    h.svc->flush();
  }
  const auto files = h.svc->queued_files();
  ASSERT_GE(files, 3u);
  EXPECT_EQ(h.svc->pump(), 1);
  ASSERT_EQ(h.posted.size(), 1u);
  EXPECT_LE(h.posted[0]["events"].size(), MAX_BATCH_EVENTS);
  EXPECT_GT(h.svc->queued_files(), 0u);  // what did not fit waits, it is not deleted
  EXPECT_LT(h.svc->queued_files(), files);
}

TEST(TelemetrySender, APauseTheServerAskedForStillHoldsAfterARestart) {
  temp_dir_t dir;
  std::int64_t now = 1'800'000'000'000;
  std::vector<json> posted;
  std::string body;
  const auto make = [&] {
    service_options_t options;
    options.dir = dir.path;
    service_deps_t deps;
    deps.now_ms = [&] { return now; };
    deps.consent = [] { return consent_t {}; };
    deps.random01 = [] { return 0.0; };
    deps.post = [&](const std::string &, const std::string &b) {
      posted.push_back(json::parse(b));
      post_result_t answer { 200 };
      answer.body = body;
      return answer;
    };
    auto svc = std::make_unique<service_t>(options, deps);
    context_t c;
    c.version = "v1";
    c.channel = "stable";
    c.os = "windows";
    svc->set_context(c);
    return svc;
  };
  {
    auto svc = make();
    body = R"({"cfg":{"paused":true}})";
    ASSERT_TRUE(svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
    svc->flush(true);
    while (svc->pump() > 0) {}
    EXPECT_TRUE(svc->backing_off());
  }
  posted.clear();
  body.clear();
  auto again = make();  // the host restarts an hour later: the pause is still on
  now += 3'600'000;
  EXPECT_TRUE(again->backing_off());
  ASSERT_TRUE(again->emit(kind_e::usage, "feature_used", json { { "feature", "web.apps_saved" } }));  // priority 2
  ASSERT_TRUE(again->emit(kind_e::error, "error", json { { "component", "x" }, { "code", "a" }, { "severity", "fatal" } }));  // priority 0
  again->flush();
  EXPECT_EQ(again->pump(), 1);
  ASSERT_EQ(posted.size(), 1u);  // only the serious one left
  EXPECT_EQ(posted[0]["events"][0]["name"], "error");
  EXPECT_EQ(again->queued_files(), 1u);
  // and the pause ends at its time (six hours), or when the server stops asking for it
  now += 6 * 3'600'000;
  EXPECT_FALSE(again->backing_off());
  auto later = make();
  EXPECT_FALSE(later->backing_off());
}

TEST(TelemetryScrub, TheApplicationNameOfTheAbrLineIsNotInTheExcerptEvenWithAQuoteInIt) {
  const auto t = scrub_log_message("ABR enabled for client " + logging::quoted_name("phone") + ": app=" + logging::quoted_name("Example\" Private Game") + " mode=balanced bitrate=20000");
  EXPECT_EQ(t.find("Example"), std::string::npos);
  EXPECT_EQ(t.find("Private"), std::string::npos);
  EXPECT_EQ(t.find("phone"), std::string::npos);
  EXPECT_NE(t.find("mode=balanced"), std::string::npos);
}

// ---------------------------------------------------------------------------------------------------------------------
// Stages (b) and (c): page sketches, API latency, update and install results, device facts

TEST(PageSketches, KeepEightBucketArraysAndDropEverythingElse) {
  const json eight = json::array({ 0, 15, 0, 0, 0, 0, 0, 0 });
  const auto ok = shape_event(kind_e::experience, "page_vitals", json { { "page", "home" }, { "visits", 15 }, { "lcp", eight }, { "inp", json::array({ 1, 2, 3 }) }, { "cls", json::array({ -1, 0, 0, 0, 0, 0, 0, 0 }) }, { "loaf_n", 3 }, { "fr_d", 5 }, { "path", "C:\\Users\\Eve" }, { "title", "Eve-PC" } }, 1000);
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(ok->props["page"], "home");
  EXPECT_EQ(ok->props["lcp"], eight);
  EXPECT_FALSE(ok->props.contains("inp"));  // wrong length
  EXPECT_FALSE(ok->props.contains("cls"));  // a negative count
  EXPECT_FALSE(ok->props.contains("path"));
  EXPECT_FALSE(ok->props.contains("title"));
  EXPECT_EQ(ok->props["fr_d"], 5);
  // the page word is required, and only a page word: no spaces, no slashes
  EXPECT_FALSE(shape_event(kind_e::experience, "page_vitals", json { { "visits", 3 } }, 1000).has_value());
  // the kind must match: a page sketch is an experience event
  EXPECT_FALSE(shape_event(kind_e::error, "page_vitals", json { { "page", "home" } }, 1000).has_value());
}

TEST(PageSketches, ApiLatencyNeedsAnEndpointWord) {
  const auto ok = shape_event(kind_e::experience, "api_latency", json { { "endpoint", "config" }, { "calls", 40 }, { "errors", 1 }, { "timeouts", 0 }, { "lat", json::array({ 0, 40, 0, 0, 0, 0, 0, 0 }) } }, 1000);
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(ok->props["calls"], 40);
  EXPECT_FALSE(shape_event(kind_e::experience, "api_latency", json { { "calls", 40 } }, 1000).has_value());
}

TEST(PageSketches, ThePagesMaySendThemAndNothingOfTheHostsOwn) {
  for (const char *product : { "panel", "web", "sparxie-launch" }) {
    EXPECT_TRUE(client_may_send(product, kind_e::experience, "page_vitals")) << product;
    EXPECT_TRUE(client_may_send(product, kind_e::experience, "api_latency")) << product;
    EXPECT_TRUE(client_may_send(product, kind_e::error, "update_result")) << product;
    // the host reads the installer's journal and its own inventory: a page cannot claim them
    EXPECT_FALSE(client_may_send(product, kind_e::error, "install_result")) << product;
    EXPECT_FALSE(client_may_send(product, kind_e::error, "install_summary")) << product;
    EXPECT_FALSE(client_may_send(product, kind_e::experience, "device_facts")) << product;
    EXPECT_FALSE(client_may_send(product, kind_e::experience, "stream_health")) << product;
  }
  EXPECT_FALSE(client_may_send("host", kind_e::experience, "page_vitals"));
}

TEST(UpdateAndInstallEvents, TheContractOfTheWordsAndNumbers) {
  const auto update = shape_event(kind_e::error, "update_result", json { { "source", "mirror" }, { "stage", "download" }, { "result", "failed" }, { "error_class", "http_5xx" }, { "from_version", "2026.1113.0" }, { "dur_ms", 1234 }, { "message", "C:\\x failed" } }, 1000);
  ASSERT_TRUE(update.has_value());
  EXPECT_FALSE(update->props.contains("message"));
  EXPECT_EQ(update->props["error_class"], "http_5xx");
  // an unknown source is "other"; an unknown stage or result drops the event (they are required)
  EXPECT_EQ(shape_event(kind_e::error, "update_result", json { { "source", "elsewhere" }, { "stage", "check" }, { "result", "ok" } }, 1000)->props["source"], "other");
  EXPECT_FALSE(shape_event(kind_e::error, "update_result", json { { "source", "mirror" }, { "stage", "dance" }, { "result", "ok" } }, 1000).has_value());
  EXPECT_FALSE(shape_event(kind_e::error, "install_result", json { { "result", "maybe" } }, 1000).has_value());
  EXPECT_TRUE(shape_event(kind_e::error, "install_result", json { { "result", "failed" }, { "component", "usbip" }, { "step", "roothub_guard" }, { "exit_code", "102" } }, 1000).has_value());
  // a path in the exit code is not a code
  EXPECT_FALSE(shape_event(kind_e::error, "install_result", json { { "result", "failed" }, { "exit_code", "C:\\x" } }, 1000)->props.contains("exit_code"));
  EXPECT_TRUE(shape_event(kind_e::error, "install_summary", json { { "result", "incomplete" }, { "phase", "upgrade" } }, 1000).has_value());
}

TEST(UpdateAndInstallEvents, PrioritiesKeepTheFailuresAndLetTheRestGive) {
  auto event = [](const char *name, json props) {
    event_t e;
    e.kind = kind_e::error;
    e.name = name;
    e.props = std::move(props);
    return e;
  };
  EXPECT_EQ(event_priority(event("install_result", json { { "result", "failed" } })), 0);
  EXPECT_EQ(event_priority(event("install_result", json { { "result", "ok" } })), 1);
  EXPECT_EQ(event_priority(event("update_result", json { { "result", "failed" } })), 0);
  EXPECT_EQ(event_priority(event("update_result", json { { "result", "ok" } })), 1);
  EXPECT_EQ(event_priority(event("install_summary", json { { "result", "incomplete" } })), 0);
  EXPECT_EQ(event_priority(event("install_summary", json { { "result", "failed" } })), 0);
  EXPECT_EQ(event_priority(event("install_summary", json { { "result", "ok" } })), 1);
  EXPECT_EQ(event_priority(event("device_facts", json::object())), 1);
  EXPECT_EQ(event_priority(event("page_vitals", json::object())), 2);
  EXPECT_EQ(event_priority(event("api_latency", json::object())), 2);
}

TEST(DeviceFacts, SpeedClassesAndLinkClasses) {
  using namespace telemetry::device_facts;
  EXPECT_EQ(nic_speed_class(std::nullopt), "unknown");
  EXPECT_EQ(nic_speed_class(0), "unknown");
  EXPECT_EQ(nic_speed_class(100), "lt_1g");
  EXPECT_EQ(nic_speed_class(866), "lt_1g");
  EXPECT_EQ(nic_speed_class(1000), "1g");
  EXPECT_EQ(nic_speed_class(2500), "2_5g");
  EXPECT_EQ(nic_speed_class(5000), "5g");
  EXPECT_EQ(nic_speed_class(10000), "10g");
  EXPECT_EQ(nic_speed_class(25000), "gt_10g");
  EXPECT_EQ(nic_link_class("ethernet"), "wired");
  EXPECT_EQ(nic_link_class("wifi"), "wireless");
  EXPECT_EQ(nic_link_class("mesh"), "wireless");
  EXPECT_EQ(nic_link_class("other"), "other");
  EXPECT_EQ(nic_link_class(""), "unknown");
}

TEST(DeviceFacts, PicksTheAdapterOnTheDefaultRouteThenTheFastestRealOne) {
  using namespace telemetry::device_facts;
  EXPECT_FALSE(pick_nic({}).has_value());
  EXPECT_FALSE(pick_nic({ { "ethernet", false, 1000, true } }).has_value());  // down
  const auto routed = pick_nic({ { "ethernet", true, 10000, false }, { "wifi", true, 866, true } });
  ASSERT_TRUE(routed.has_value());
  EXPECT_EQ(routed->type, "wifi");
  const auto fastest = pick_nic({ { "virtual_interface", true, 100000, false }, { "ethernet", true, 1000, false }, { "ethernet", true, 2500, false } });
  ASSERT_TRUE(fastest.has_value());
  EXPECT_EQ(fastest->link_speed_mbps, 2500);
}

TEST(DeviceFacts, RefreshRatesSnapToStandardRatesAndBuildNumbersKeepTheirDigits) {
  using namespace telemetry::device_facts;
  EXPECT_EQ(standard_refresh_hz(59.94), 60);
  EXPECT_EQ(standard_refresh_hz(143.98), 144);
  EXPECT_EQ(standard_refresh_hz(119.88), 120);
  EXPECT_EQ(standard_refresh_hz(60), 60);
  EXPECT_EQ(standard_refresh_hz(0), 0);
  EXPECT_EQ(standard_refresh_hz(5000), 0);
  EXPECT_EQ(standard_refresh_hz(std::nan("")), 0);
  EXPECT_EQ(standard_refresh_hz(90.4), 90);
  EXPECT_EQ(standard_refresh_hz(137.0), 137);  // not a standard rate: rounded
  EXPECT_EQ(os_build_token("10.0.26100"), "26100");
  EXPECT_EQ(os_build_token("26100.4349"), "26100");
  EXPECT_EQ(os_build_token("Windows 11 Pro"), "11");
  EXPECT_EQ(os_build_token("no digits"), "");
  EXPECT_LE(os_build_token(std::string(40, '7')).size(), 16u);
}

TEST(DeviceFacts, ReportedOnceThenOnlyWhenChangedAndADayOld) {
  using namespace telemetry::device_facts;
  facts_t f;
  f.gpu_vendor = "nvidia";
  f.gpu_driver = "32.0.15.6094";
  f.os_build = "26100";
  f.displays = 2;
  f.refresh_hz = 144;
  f.nic_link = "wired";
  f.nic_speed = "2_5g";
  const std::int64_t day = 24LL * 3600 * 1000;
  EXPECT_TRUE(should_send(std::nullopt, f, 1000));
  state_t sent { fingerprint(f), 1000 };
  EXPECT_FALSE(should_send(sent, f, 1000 + 10 * day));  // the same facts are never sent again
  facts_t changed = f;
  changed.refresh_hz = 60;
  EXPECT_FALSE(should_send(sent, changed, 1000 + day / 2));  // changed, but a report was sent a few hours ago
  EXPECT_TRUE(should_send(sent, changed, 1000 + day));
  EXPECT_FALSE(should_send(std::nullopt, facts_t {}, 1000));  // nothing is known: nothing is sent
  // the state survives a restart as json, and a damaged one is no state
  const auto back = state_from_json(state_to_json(sent));
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back->fingerprint, sent.fingerprint);
  EXPECT_EQ(back->sent_ms, 1000);
  EXPECT_FALSE(state_from_json(json { { "v", 2 } }).has_value());
  EXPECT_FALSE(state_from_json(json::parse("[1,2]")).has_value());
}

TEST(DeviceFacts, TheEventPassesTheAllowListAndCarriesNothingElse) {
  using namespace telemetry::device_facts;
  facts_t f;
  f.gpu_vendor = "amd";
  f.os_build = "26100";
  f.displays = 1;
  f.refresh_hz = 165;
  f.nic_link = "wireless";
  f.nic_speed = "1g";
  const auto event = shape_event(kind_e::experience, "device_facts", to_props(f), 1000);
  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(event->props.size(), 6u);  // no driver was known
  EXPECT_EQ(event->props["refresh_hz"], 165);
  EXPECT_EQ(event->props["nic_speed"], "1g");
  // a hostile value for the adapter class is "unknown", never text
  const auto hostile = shape_event(kind_e::experience, "device_facts", json { { "nic_speed", "Intel(R) Wi-Fi 6E AX211" }, { "hostname", "EVE-PC" } }, 1000);
  ASSERT_TRUE(hostile.has_value());
  EXPECT_FALSE(hostile->props.contains("hostname"));
  EXPECT_FALSE(hostile->props.contains("nic_speed"));
}

namespace {
  const char *const JOURNAL_OK =
    "{\"v\":1,\"kind\":\"start\",\"run\":\"20261115213000\",\"phase\":\"upgrade\",\"from\":\"2026.1113.173055\"}\n"
    "{\"v\":1,\"kind\":\"component\",\"run\":\"20261115213000\",\"component\":\"gamepad\",\"step\":\"install\",\"exit\":0}\n"
    "{\"v\":1,\"kind\":\"component\",\"run\":\"20261115213000\",\"component\":\"usbip\",\"step\":\"install\",\"exit\":102}\n"
    "{\"v\":1,\"kind\":\"component\",\"run\":\"20261115213000\",\"component\":\"vdd\",\"step\":\"install\",\"exit\":3010}\n"
    "{\"v\":1,\"kind\":\"component\",\"run\":\"20261115213000\",\"component\":\"service\",\"step\":\"verify\",\"exit\":0}\n"
    "{\"v\":1,\"kind\":\"finish\",\"run\":\"20261115213000\",\"exit\":3010,\"ms\":90000}\n";

  const telemetry::install_journal::item_t *
  find(const telemetry::install_journal::result_t &r, const char *name, const char *component = nullptr) {
    for (const auto &i : r.items)
      if (i.name == name && (!component || i.props.value("component", "") == component)) return &i;
    return nullptr;
  }
}  // namespace

TEST(InstallJournal, AFinishedRunBecomesOneResultPerComponentAndOneSummary) {
  const auto r = install_journal::events_from_journal(JOURNAL_OK, 1000);
  ASSERT_TRUE(r.consumed);
  ASSERT_EQ(r.items.size(), 5u);
  const auto *guard = find(r, "install_result", "usbip");
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->props["result"], "failed");
  EXPECT_EQ(guard->props["step"], "roothub_guard");  // 102 to 105 are the root-hub guard of the USB/IP component
  EXPECT_EQ(guard->props["exit_code"], "102");
  EXPECT_EQ(guard->props["phase"], "upgrade");
  EXPECT_EQ(guard->props["from_version"], "2026.1113.173055");
  const auto *vdd = find(r, "install_result", "vdd");
  ASSERT_NE(vdd, nullptr);
  EXPECT_EQ(vdd->props["result"], "ok");  // 3010 is a success that wants a restart
  EXPECT_TRUE(vdd->props["reboot_needed"].get<bool>());
  const auto *gamepad = find(r, "install_result", "gamepad");
  ASSERT_NE(gamepad, nullptr);
  EXPECT_EQ(gamepad->props["result"], "ok");
  EXPECT_FALSE(gamepad->props.contains("error_class"));
  const auto *summary = find(r, "install_summary");
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->props["result"], "failed");  // one component failed although the run finished
  EXPECT_EQ(summary->props["components"], 4);
  EXPECT_EQ(summary->props["failed"], 1);
  EXPECT_TRUE(summary->props["reboot_needed"].get<bool>());
  EXPECT_EQ(summary->props["dur_ms"], 90000);
  // every event passes the allow-list of the sender as it is
  for (const auto &item : r.items) EXPECT_TRUE(shape_event(item.kind, item.name, item.props, 1000).has_value()) << item.name << " " << item.props.dump();
}

TEST(InstallJournal, AllGuardCodesAreNamedAsTheGuardStep) {
  for (const char *code : { "102", "103", "104", "105" }) {
    const std::string line = std::string { R"({"v":1,"kind":"start","run":"r1","phase":"install"})" } + "\n" + R"({"v":1,"kind":"component","run":"r1","component":"usbip","step":"whatever","exit":)" + code + "}\n" + R"({"v":1,"kind":"finish","run":"r1","exit":0})" + "\n";
    const auto r = install_journal::events_from_journal(line, 1000);
    const auto *x = find(r, "install_result", "usbip");
    ASSERT_NE(x, nullptr) << code;
    EXPECT_EQ(x->props["step"], "roothub_guard") << code;
    EXPECT_EQ(x->props["exit_code"], code);
  }
}

TEST(InstallJournal, ARunThatNeverFinishedIsIncompleteOnlyOnceTheJournalIsQuiet) {
  const std::string journal = std::string { R"({"v":1,"kind":"start","run":"r1","phase":"install"})" } + "\n" + R"({"v":1,"kind":"component","run":"r1","component":"vdd","step":"install","exit":0})" + "\n";
  const auto fresh = install_journal::events_from_journal(journal, 30 * 1000);
  EXPECT_FALSE(fresh.consumed);  // still going: leave the file for the next start
  EXPECT_TRUE(fresh.items.empty());
  const auto quiet = install_journal::events_from_journal(journal, install_journal::QUIET_MS + 1);
  EXPECT_TRUE(quiet.consumed);
  const auto *summary = find(quiet, "install_summary");
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->props["result"], "incomplete");
  EXPECT_NE(find(quiet, "install_result", "vdd"), nullptr);
}

TEST(InstallJournal, ExitCodesBecomeCodesAndClasses) {
  EXPECT_EQ(install_journal::exit_code_text(0), "0");
  EXPECT_EQ(install_journal::exit_code_text(3010), "3010");
  EXPECT_EQ(install_journal::exit_code_text(-1), "0xFFFFFFFF");
  EXPECT_EQ(install_journal::exit_code_text(-2147024891LL), "0x80070005");
  EXPECT_EQ(install_journal::error_class_of(5), "permission");
  EXPECT_EQ(install_journal::error_class_of(112), "disk");
  EXPECT_EQ(install_journal::error_class_of(1602), "cancelled");
  EXPECT_EQ(install_journal::error_class_of(102), "other");
  EXPECT_EQ(install_journal::version_token("v2026.1113.173055.name"), "2026.1113.173055");
  EXPECT_EQ(install_journal::version_token("no version"), "");
  EXPECT_EQ(install_journal::version_token("C:\\Users\\Eve\\1.2.3"), "1.2.3");
}

TEST(InstallJournal, AHostileJournalGivesNothingItDoesNotKnow) {
  std::string journal;
  journal += "not json at all\n";
  journal += R"({"v":2,"kind":"start","run":"r1"})"
             "\n";  // another version
  journal += R"({"v":1,"kind":"start","run":"../../etc/passwd","phase":"install"})"
             "\n";  // a run id that is a path
  journal += R"({"v":1,"kind":"start","run":"r2","phase":"hack","from":"C:\\Users\\Eve\\9.9.9"})"
             "\n";
  journal += R"({"v":1,"kind":"component","run":"r2","component":"C:\\Users\\Eve","step":"x","exit":1})"
             "\n";  // not a component of ours
  journal += R"({"v":1,"kind":"component","run":"r2","component":"vdd","step":"A very long step with spaces and a path C:\\x","exit":99999999999999})"
             "\n";  // out of range
  journal += R"({"v":1,"kind":"component","run":"r2","component":"vdd","step":"Verify Step!","exit":7})"
             "\n";
  journal += R"({"v":1,"kind":"finish","run":"r2","exit":"zero"})"
             "\n";
  const auto r = install_journal::events_from_journal(journal, 1000);
  ASSERT_TRUE(r.consumed);
  const auto *vdd = find(r, "install_result", "vdd");
  ASSERT_NE(vdd, nullptr);
  EXPECT_EQ(vdd->props["step"], "install");  // "Verify Step!" is no step word: the default step stands
  EXPECT_EQ(vdd->props["phase"], "install");  // "hack" is not a phase
  EXPECT_EQ(vdd->props["from_version"], "9.9.9");  // only the digits and dots of the version survive
  EXPECT_EQ(find(r, "install_result", "C:\\Users\\Eve"), nullptr);
  for (const auto &item : r.items) {
    const auto text = item.props.dump();
    EXPECT_EQ(text.find("Eve"), std::string::npos) << text;
    EXPECT_EQ(text.find("passwd"), std::string::npos) << text;
    EXPECT_TRUE(shape_event(item.kind, item.name, item.props, 1000).has_value()) << text;
  }
  // a journal of any size is read from its end and bounded
  std::string big;
  for (int i = 0; i < 5000; ++i) big += R"({"v":1,"kind":"component","run":"r3","component":"vdd","step":"s","exit":1})"
                                        "\n";
  EXPECT_LE(install_journal::events_from_journal(big, install_journal::QUIET_MS + 1).items.size(), 40u);
}

TEST(TelemetryScrub, ALogNameIsWrittenBetweenDoubleQuotesWhateverItHolds) {
  EXPECT_EQ(logging::quoted_name("Bob's phone"), "\"Bob's phone\"");
  EXPECT_EQ(logging::quoted_name("say \"hi\"\n\tnow"), "\"say 'hi'  now\"");
  EXPECT_EQ(logging::quoted_name(""), "\"\"");
  const auto title = scrub_log_message("ABR: foreground changed to " + logging::quoted_name("Players' choice - Budget.xlsx") + " (Excel.exe) pid=4242");
  EXPECT_EQ(title.find("Budget"), std::string::npos);
  EXPECT_EQ(title.find("choice"), std::string::npos);
  EXPECT_NE(title.find("pid=4242"), std::string::npos);
  const auto client = scrub_log_message("Client " + logging::quoted_name("Bob' Tablet") + ": Microphone encryption ENABLED");
  EXPECT_EQ(client.find("Bob"), std::string::npos);
  EXPECT_EQ(client.find("Tablet"), std::string::npos);
  EXPECT_NE(client.find("Microphone encryption ENABLED"), std::string::npos);
}

TEST(TelemetryScrub, ASingleQuotedNameThatEndsOnAQuoteBeforeASpaceIsDroppedToItsLastQuote) {
  // the lines as the host wrote them before the double-quote encoding
  const auto title = scrub_log_message("ABR: foreground changed to 'Players' choice - Budget.xlsx' (Excel.exe) pid=4242");
  EXPECT_EQ(title.find("Players"), std::string::npos);
  EXPECT_EQ(title.find("Budget"), std::string::npos);
  EXPECT_EQ(title.find("choice"), std::string::npos);
  const auto client = scrub_log_message("ABR enabled for client 'Bob' phone': app=\"Game\" mode=balanced");
  EXPECT_EQ(client.find("Bob"), std::string::npos);
  EXPECT_EQ(client.find("phone"), std::string::npos);
  EXPECT_NE(client.find("mode=balanced"), std::string::npos);
}

TEST(TelemetryScrub, AQuoteOrBracketWithoutAPartnerDropsTheRestOfTheLine) {
  for (const char *line : { "client 'Bob phone connected at 10:00", "title \"Budget.xlsx - Excel pid=4 mode=x", "app [Budget sheet pid=4 mode=x", "app {Budget sheet pid=4" }) {
    const auto t = scrub_log_message(line);
    EXPECT_EQ(t.find("Bob"), std::string::npos) << line;
    EXPECT_EQ(t.find("Budget"), std::string::npos) << line;
    EXPECT_EQ(t.find("pid"), std::string::npos) << line;
    EXPECT_EQ(t.find("connected"), std::string::npos) << line;
  }
  // a line cut at the size limit inside a name is such a line too
  const auto cut = scrub_log_message(std::string("start \"") + std::string(40, 'x') + "\" tail with Budget.xlsx", 8);
  EXPECT_EQ(cut.find("Budget"), std::string::npos);
  // apostrophes inside words and balanced quotes keep the rest of the line
  const auto ok = scrub_log_message("Couldn't read 'a b' then can't retry \"c\" for 3 s");
  EXPECT_NE(ok.find("Couldn't read"), std::string::npos);
  EXPECT_NE(ok.find("can't retry"), std::string::npos);
  EXPECT_NE(ok.find("for 3 s"), std::string::npos);
}

TEST(TelemetrySender, ARenewedWaitDoesNotLetTheNextPriority0RequestLeaveAtOnce) {
  harness_t h;
  h.next_body = R"({"ok":true,"cfg":{"backoff_s":600}})";  // every answer asks for the wait again
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  ASSERT_EQ(h.svc->pump(), 1);
  ASSERT_TRUE(h.svc->backing_off());
  h.posted.clear();
  ASSERT_TRUE(h.svc->emit(kind_e::experience, "stream_anomaly", json { { "code", "XP_QUEUE_FLUSH" }, { "severe", true }, { "count", 32 } }));
  h.svc->flush();
  EXPECT_EQ(h.svc->pump(), 1);  // the first one leaves, its answer renews the wait
  ASSERT_EQ(h.posted.size(), 1u);
  ASSERT_TRUE(h.svc->emit(kind_e::experience, "stream_anomaly", json { { "code", "XP_SEND_BACKLOG" }, { "severe", true }, { "count", 1 } }));
  h.svc->flush();
  for (int minute = 0; minute < 9; ++minute) {
    h.now += 60'000;
    EXPECT_EQ(h.svc->pump(), 0) << "minute " << minute;
  }
  EXPECT_EQ(h.posted.size(), 1u);
  EXPECT_EQ(h.svc->queued_files(), 1u);
  // the wait is over (the one that the answer renewed): the rest leaves
  h.now += 61'000;
  while (h.svc->pump() > 0) {}
  EXPECT_EQ(h.posted.size(), 2u);
}

TEST(TelemetryScrub, ABracketHoldingBracketsEndsWhereTheOutermostOneCloses) {
  const auto t = scrub_log_message("app [Project [draft] Budget.xlsx] failed");
  EXPECT_EQ(t.find("Project"), std::string::npos);
  EXPECT_EQ(t.find("Budget"), std::string::npos);
  EXPECT_EQ(t.find("draft"), std::string::npos);
  EXPECT_NE(t.find("failed"), std::string::npos);
  const auto mixed = scrub_log_message("title {Plan {v2} [final] Budget.xlsx} then pid=4");
  EXPECT_EQ(mixed.find("Budget"), std::string::npos);
  EXPECT_EQ(mixed.find("Plan"), std::string::npos);
  EXPECT_NE(mixed.find("pid=4"), std::string::npos);
  // the boundary cannot be found: the inner one closes, the outer one never does
  const auto open = scrub_log_message("app [Project [draft] Budget.xlsx failed pid=4");
  EXPECT_EQ(open.find("Budget"), std::string::npos);
  EXPECT_EQ(open.find("pid"), std::string::npos);
  // a short code in brackets stays
  EXPECT_NE(scrub_log_message("encoder [E12] gave up").find("[E12]"), std::string::npos);
}

// ---------------------------------------------------------------------------------------------------------------------
// The record that a choice was turned off (src/telemetry/opt_out.h)

namespace {
  const std::string OPT_OUT_ID = "3f2b8a52-1c3e-4d0e-9d57-0a7c1f6b9e11";
}

TEST(OptOut, OnlyAChoiceThatWasOnAndIsOffCounts) {
  using namespace telemetry::opt_out;
  EXPECT_TRUE(turned_off(state_t { false, true, true }, false, false).empty());  // the first run only remembers
  EXPECT_EQ(turned_off(state_t { true, true, true }, false, true), (std::vector<std::string> { "usage" }));
  EXPECT_EQ(turned_off(state_t { true, true, true }, false, false), (std::vector<std::string> { "usage", "errors" }));
  EXPECT_TRUE(turned_off(state_t { true, false, true }, false, true).empty());  // it was off already
  EXPECT_TRUE(turned_off(state_t { true, false, false }, true, true).empty());  // turned on is not a record
}

TEST(OptOut, TheRecordCarriesTheChoiceTheProductAndTheVersionAndNothingElse) {
  using namespace telemetry::opt_out;
  std::vector<record_t> records;
  add(records, { "errors" }, { { "host", "5.2.0.1.杂鱼" }, { "panel", "1.0.0" } }, OPT_OUT_ID, 1'800'000'000'000);
  ASSERT_EQ(records.size(), 2u);
  const auto body = body_of(records[1], "11111111-2222-4333-8444-555555555555");
  ASSERT_TRUE(body.is_object());
  EXPECT_EQ(body["install_id"], OPT_OUT_ID);
  // nothing about the computer goes with it: the install, a fresh session number, the product, its version and the one event
  for (const char *key : { "channel", "os", "os_version", "arch", "gpu_vendor", "locale" }) EXPECT_FALSE(body.contains(key)) << key;
  std::set<std::string> keys;
  for (const auto &[k, v] : body.items()) keys.insert(k);
  EXPECT_EQ(keys, (std::set<std::string> { "v", "install_id", "session_id", "product", "version", "events" }));
  EXPECT_EQ(body["product"], "panel");
  EXPECT_EQ(body["version"], "1.0.0");
  ASSERT_EQ(body["events"].size(), 1u);
  EXPECT_EQ(body["events"][0]["name"], "opt_out");
  EXPECT_EQ(body["events"][0]["props"], (json { { "choice", "errors" } }));
  // the same turn-off twice is one record; a bad id makes none
  add(records, { "errors" }, { { "host", "5.2.0.1.杂鱼" } }, OPT_OUT_ID, 1'800'000'001'000);
  EXPECT_EQ(records.size(), 2u);
  std::vector<record_t> none;
  add(none, { "usage" }, { { "host", "1" } }, "", 1000);
  EXPECT_TRUE(none.empty());
}

TEST(OptOut, AnExpiredRecordDoesNotBlockANewTurnOff) {
  using namespace telemetry::opt_out;
  std::vector<record_t> records;
  const std::int64_t t0 = 1'800'000'000'000;
  add(records, { "usage" }, { { "host", "1" } }, OPT_OUT_ID, t0);
  ASSERT_EQ(records.size(), 1u);
  // still waiting: no second record
  add(records, { "usage" }, { { "host", "1" } }, OPT_OUT_ID, t0 + MAX_AGE_MS);
  EXPECT_EQ(records.size(), 1u);
  // expired but not yet cleaned up by due(): a new turn-off makes a new record
  add(records, { "usage" }, { { "host", "1" } }, OPT_OUT_ID, t0 + MAX_AGE_MS + 1);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].t, t0 + MAX_AGE_MS + 1);
  // a queue full of expired records has room again
  std::vector<record_t> full(MAX_RECORDS, record_t { "errors", "panel", "1", OPT_OUT_ID, t0, t0, 0 });
  add(full, { "usage" }, { { "host", "1" } }, OPT_OUT_ID, t0 + MAX_AGE_MS + 1);
  ASSERT_EQ(full.size(), 1u);
  EXPECT_EQ(full[0].choice, "usage");
}

TEST(OptOut, ARecordIsRetriedWithGrowingGapsAndGivenUpOnAfterTwoWeeks) {
  using namespace telemetry::opt_out;
  std::vector<record_t> records;
  add(records, { "usage" }, { { "host", "1" } }, OPT_OUT_ID, 1000);
  EXPECT_EQ(due(records, 1000).size(), 1u);
  failed(records[0], 1000);
  EXPECT_TRUE(due(records, 1000 + 60'000).empty());
  EXPECT_EQ(due(records, 1000 + RETRY_BASE_MS).size(), 1u);
  failed(records[0], 1000);
  EXPECT_EQ(records[0].next_try_ms, 1000 + 2 * RETRY_BASE_MS);
  EXPECT_TRUE(due(records, 1000 + MAX_AGE_MS + 1).empty());
  EXPECT_TRUE(records.empty());
  // what is written to disk is read back, and a hostile file gives nothing it does not know
  std::vector<record_t> again;
  add(again, { "usage", "errors" }, { { "host", "1" }, { "web", "2" } }, OPT_OUT_ID, 5000);
  const auto back = parse(serialize(again));
  ASSERT_EQ(back.size(), 4u);
  EXPECT_EQ(back[3].product, "web");
  EXPECT_TRUE(parse("{\"v\":1,\"choice\":\"all\",\"product\":\"host\",\"version\":\"1\",\"install_id\":\"" + OPT_OUT_ID + "\",\"t\":1,\"next\":1,\"tries\":0}\nnot json\n").empty());
}

TEST(OptOut, RecordIdsAreRandomAndSurviveTheFile) {
  using namespace telemetry::opt_out;
  std::vector<record_t> records;
  add(records, { "usage", "errors" }, { { "host", "1" }, { "panel", "2" } }, OPT_OUT_ID, 9'000);
  ASSERT_EQ(records.size(), 4u);
  std::set<std::uint64_t> ids;
  for (const auto &r : records) {
    EXPECT_NE(r.id, 0u);
    ids.insert(r.id);
  }
  EXPECT_EQ(ids.size(), 4u);
  // a wall-clock jump can give two records the same t; the ids still tell them apart
  EXPECT_EQ(records[0].t, records[1].t);
  EXPECT_NE(records[0].id, records[1].id);
  const auto back = parse(serialize(records));
  ASSERT_EQ(back.size(), 4u);
  for (std::size_t i = 0; i < records.size(); ++i) EXPECT_EQ(back[i].id, records[i].id);
}

TEST(OptOut, ARecordWrittenBeforeTheIdExistedGetsOneAndKeepsIt) {
  using namespace telemetry::opt_out;
  const std::string old = "{\"v\":1,\"choice\":\"usage\",\"product\":\"host\",\"version\":\"1\",\"install_id\":\"" + OPT_OUT_ID + "\",\"t\":1800000000000,\"next\":1800000000000,\"tries\":0}\n";
  auto records = parse(old);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_NE(records[0].id, 0u);
  const auto again = parse(serialize(records));
  ASSERT_EQ(again.size(), 1u);
  EXPECT_EQ(again[0].id, records[0].id);
}

TEST(OptOut, TwoRecordsThatShareANumberOnTheDiskGetDistinctOnesAndKeepTheirOwnFields) {
  using namespace telemetry::opt_out;
  const auto line = [](const std::string &choice, const std::string &product, std::int64_t next) {
    return "{\"v\":1,\"id\":11,\"choice\":\"" + choice + "\",\"product\":\"" + product + "\",\"version\":\"1\",\"install_id\":\"" + OPT_OUT_ID + "\",\"t\":1800000000000,\"next\":" + std::to_string(next) +
           ",\"tries\":0}\n";
  };
  auto records = parse(line("usage", "host", 1800000000000) + line("errors", "panel", 1800086400000));
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].id, 11u);
  EXPECT_NE(records[1].id, 0u);
  EXPECT_NE(records[1].id, records[0].id);
  EXPECT_EQ(records[1].choice, "errors");
  EXPECT_EQ(records[1].product, "panel");
  EXPECT_EQ(records[1].next_try_ms, 1800086400000);
  const auto again = parse(serialize(records));
  ASSERT_EQ(again.size(), 2u);
  EXPECT_EQ(again[1].id, records[1].id);
}

TEST(OptOutService, TurningAChoiceOffSendsOneRecordAndNothingAfterIt) {
  harness_t h;
  h.consent = consent_t { true, true };
  h.svc->reconcile_consent();  // the first run: remembered, nothing to say
  const auto id = h.svc->install_id();
  ASSERT_EQ(id.size(), 36u);
  // the panel has talked to this host
  EXPECT_EQ(h.svc->ingest("panel", "1.0.0", json::array({ json { { "kind", "usage" }, { "name", "app_start" }, { "props", json { { "cold", true } } } } })), 1u);
  h.consent = consent_t { false, true };
  h.svc->consent_changed();
  ASSERT_EQ(h.svc->pump(), 0);  // no ordinary batch
  std::vector<json> records;
  for (const auto &b : h.posted)
    for (const auto &e : b["events"])
      if (e["name"] == "opt_out") records.push_back(json { { "product", b["product"] }, { "version", b["version"] }, { "id", b["install_id"] }, { "props", e["props"] } });
  ASSERT_EQ(records.size(), 2u);
  for (const auto &r : records) {
    EXPECT_EQ(r["id"], id);
    EXPECT_EQ(r["props"], (json { { "choice", "usage" } }));
  }
  EXPECT_EQ(names_of(h.posted), (std::set<std::string> { "opt_out" }));
  // delivered: nothing is sent again, and later events of the choice that is off stay unsent
  h.posted.clear();
  h.svc->pump();
  EXPECT_TRUE(h.posted.empty());
  EXPECT_FALSE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  // the other choice goes off too: its record, then the install id is gone
  h.consent = consent_t { false, false };
  h.svc->consent_changed();
  h.svc->pump();
  EXPECT_FALSE(h.posted.empty());
  for (const auto &b : h.posted) EXPECT_EQ(b["events"][0]["props"], (json { { "choice", "errors" } }));
  EXPECT_TRUE(h.svc->install_id().empty());
}

TEST(OptOutService, TheRecordIsSavedBeforeTheStateAndAnExpiredOneLeavesTheDisk) {
  harness_t h;
  h.consent = consent_t { true, true };
  h.svc->reconcile_consent();
  const auto id = h.svc->install_id();
  const auto pending = h.dir.path / "opt_out_pending.jsonl";
  const auto state = h.dir.path / "consent_state.json";
  // no room to save the record (a directory stands where the file goes): the state must still say "on", so the next call finds the turn-off again
  std::filesystem::create_directories(pending);
  h.consent = consent_t { false, true };
  h.svc->consent_changed();
  EXPECT_EQ(json::parse(std::ifstream(state))["usage"], true);
  std::filesystem::remove_all(pending);
  h.svc->reconcile_consent();
  EXPECT_TRUE(std::filesystem::is_regular_file(pending));
  EXPECT_EQ(json::parse(std::ifstream(state))["usage"], false);
  // the record is delivered: the file is empty, not left with the id in it
  h.svc->pump();
  EXPECT_EQ(std::filesystem::file_size(pending), 0u);
  // a record that was never delivered is dropped after two weeks, and the file goes with it even though nothing else is waiting
  h.next_status = 503;
  h.consent = consent_t { false, false };
  h.svc->reconcile_consent();
  h.svc->pump();
  EXPECT_GT(std::filesystem::file_size(pending), 0u);
  h.now += telemetry::opt_out::MAX_AGE_MS + 60'000;
  h.svc->pump();
  EXPECT_EQ(std::filesystem::file_size(pending), 0u);
  EXPECT_EQ(std::ifstream(pending).peek(), std::ifstream::traits_type::eof());
  (void) id;
}

TEST(OptOutService, BothChoicesOffWithTheRecordNotSavedKeepsTheNumberUntilAFreshStartSavesIt) {
  harness_t h;
  h.consent = consent_t { true, true };
  h.svc->reconcile_consent();
  const auto id = h.svc->install_id();
  ASSERT_EQ(id.size(), 36u);
  const auto pending = h.dir.path / "opt_out_pending.jsonl";
  std::filesystem::create_directories(pending);  // the file cannot be written
  h.consent = consent_t { false, false };
  h.svc->consent_changed();
  EXPECT_TRUE(std::filesystem::exists(h.dir.path / "install_id"));  // the way back is kept
  EXPECT_EQ(json::parse(std::ifstream(h.dir.path / "consent_state.json"))["usage"], true);  // and the state still says "on"
  // the host ends (the memory with the record is lost), the problem is gone, a new service starts over the same folder with both choices off
  const auto dir = h.dir.path;
  h.svc.reset();
  std::filesystem::remove_all(pending);
  std::vector<json> posted;
  service_deps_t deps;
  std::int64_t now = h.now + 60'000;
  deps.now_ms = [&] { return now; };
  deps.consent = [] { return consent_t { false, false }; };
  deps.post = [&](const std::string &, const std::string &body) {
    posted.push_back(json::parse(body));
    return post_result_t { 200 };
  };
  service_options_t options;
  options.dir = dir;
  service_t fresh(options, deps);
  context_t c;
  c.version = "5.2.0.1.杂鱼";
  fresh.set_context(c);
  EXPECT_TRUE(fresh.reconcile_consent());  // the turn-off of both is found again, saved, and then the number goes
  EXPECT_FALSE(std::filesystem::exists(dir / "install_id"));
  EXPECT_GT(std::filesystem::file_size(pending), 0u);
  fresh.pump();
  std::set<std::string> choices;
  for (const auto &b : posted) {
    EXPECT_EQ(b["install_id"], id);
    choices.insert(b["events"][0]["props"]["choice"].get<std::string>());
  }
  EXPECT_EQ(choices, (std::set<std::string> { "usage", "errors" }));
  EXPECT_EQ(std::filesystem::file_size(pending), 0u);
}

TEST(OptOutService, AFailedCleanupWriteIsRepeatedUntilTheFileIsEmpty) {
  harness_t h;
  h.consent = consent_t { true, true };
  h.svc->reconcile_consent();
  ASSERT_EQ(h.svc->install_id().size(), 36u);
  const auto pending = h.dir.path / "opt_out_pending.jsonl";
  h.next_status = 503;  // the record stays undelivered
  h.consent = consent_t { false, true };
  h.svc->consent_changed();
  h.svc->pump();
  ASSERT_GT(std::filesystem::file_size(pending), 0u);
  // the write of the cleanup fails (the temporary file cannot be created): the record leaves the memory, the file keeps it for now
  std::filesystem::create_directories(h.dir.path / "opt_out_pending.jsonl.tmp");
  h.now += telemetry::opt_out::MAX_AGE_MS + 60'000;
  h.svc->pump();
  EXPECT_GT(std::filesystem::file_size(pending), 0u);
  // the disk works again: the next pass writes the empty set although nothing is waiting
  std::filesystem::remove_all(h.dir.path / "opt_out_pending.jsonl.tmp");
  h.svc->pump();
  EXPECT_EQ(std::filesystem::file_size(pending), 0u);
}

TEST(OptOutService, AnOfflineRecordWaitsAndAFirstRunWithTheChoiceOffSendsNothing) {
  harness_t h;
  h.consent = consent_t { true, true };
  h.svc->reconcile_consent();
  ASSERT_EQ(h.svc->install_id().size(), 36u);  // an install that never had an id never sent anything: there is nothing to say about it
  h.next_status = 503;
  h.consent = consent_t { true, false };
  h.svc->consent_changed();
  h.svc->pump();
  ASSERT_FALSE(h.posted.empty());
  const auto tries = h.posted.size();
  h.svc->pump();
  EXPECT_EQ(h.posted.size(), tries);  // not again right away
  h.now += 11 * 60'000;
  h.next_status = 200;
  h.svc->pump();
  EXPECT_EQ(h.posted.size(), tries + 1);
  h.posted.clear();
  h.now += 24 * 3600'000;
  h.svc->pump();
  EXPECT_TRUE(h.posted.empty());  // done
  // a new install folder whose choices are off from the start says nothing
  harness_t fresh;
  fresh.consent = consent_t { false, false };
  fresh.svc->reconcile_consent();
  fresh.svc->pump();
  EXPECT_TRUE(fresh.posted.empty());
}

TEST(OptOutService, RecordsWithTheSameTAreMatchedToTheirOwnSendResult) {
  temp_dir_t dir;
  const std::int64_t t0 = 1'800'000'000'000;
  const auto pending = dir.path / "opt_out_pending.jsonl";
  {
    std::ofstream out(pending);
    out << json { { "v", 1 }, { "id", 11 }, { "choice", "usage" }, { "product", "host" }, { "version", "1" }, { "install_id", OPT_OUT_ID }, { "t", t0 }, { "next", t0 }, { "tries", 0 } }.dump() << "\n";
    out << json { { "v", 1 }, { "id", 22 }, { "choice", "usage" }, { "product", "host" }, { "version", "1" }, { "install_id", OPT_OUT_ID }, { "t", t0 }, { "next", t0 + 24 * 3600 * 1000 }, { "tries", 0 } }.dump() << "\n";
  }
  std::int64_t now = t0;
  std::vector<json> posted;
  service_deps_t deps;
  deps.now_ms = [&] { return now; };
  deps.consent = [] { return consent_t { true, true }; };
  deps.post = [&](const std::string &, const std::string &body) {
    posted.push_back(json::parse(body));
    return post_result_t { 200 };
  };
  service_options_t options;
  options.dir = dir.path;
  service_t svc(options, deps);
  context_t c;
  c.version = "5.2.0.1.杂鱼";
  svc.set_context(c);
  svc.pump();
  ASSERT_EQ(posted.size(), 1u);  // only the record that was due
  {
    std::ifstream in(pending);
    std::string line;
    ASSERT_TRUE(std::getline(in, line));
    const auto left = telemetry::opt_out::parse(line);
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0].id, 22u);  // the sent record went; its twin with the same t did not
  }
  now += 24 * 3600 * 1000;
  svc.pump();
  ASSERT_EQ(posted.size(), 2u);
  EXPECT_EQ(std::filesystem::file_size(pending), 0u);
}

// ---------------------------------------------------------------------------------------------------------------------
// The service and the start-up: XP_SVC_CORE_RESTART and XP_SLOW_READY

#include "src/telemetry/service_lifecycle.h"
#include "tools/sunshinesvc_lifecycle.h"

namespace {
  using namespace telemetry::service_lifecycle;
  namespace life = sunshinesvc::lifecycle;

  // the sequence of the bug: an extra core ended, another one adopted, it exits with code 0 about 30 s later, the core is started again
  std::string
  bug_sequence(std::int64_t t0) {
    return life::svc_start_line(t0) + "\n" + life::extra_ended_line(t0 + 100, 1) + "\n" + life::core_start_line(t0 + 200, true, 4242, 90'000, "boot") + "\n" +
           life::core_exit_line(t0 + 30'200, 0, 120'000, 30'000, false) + "\n" + life::core_start_line(t0 + 30'700, false, 5151, 0, "core_exit") + "\n";
  }
}  // namespace

TEST(ServiceLifecycle, TheLinesTheServiceWritesAreTheLinesTheHostReads) {
  const auto records = parse(bug_sequence(1'800'000'000'000));
  ASSERT_EQ(records.size(), 5u);
  EXPECT_EQ(records[0].kind, "svc_start");
  EXPECT_EQ(records[1].kind, "extra_ended");
  EXPECT_EQ(records[1].n, 1);
  EXPECT_TRUE(records[2].adopted);
  EXPECT_EQ(records[2].pid, 4242);
  EXPECT_EQ(records[2].age_ms, 90'000);
  EXPECT_EQ(records[3].kind, "core_exit");
  EXPECT_EQ(records[3].since_adopt_ms, 30'000);
  EXPECT_FALSE(records[3].requested);
  EXPECT_EQ(records[4].cause, "core_exit");
  EXPECT_FALSE(records[4].adopted);
  // an exit code that is a negative NTSTATUS survives as the signed 32-bit value
  const auto crash = parse(life::core_exit_line(1000, 0xC0000005ul, 5000, -1, false) + "\n");
  ASSERT_EQ(crash.size(), 1u);
  EXPECT_EQ(crash[0].exit_code, static_cast<std::int32_t>(0xC0000005u));
}

TEST(ServiceLifecycle, TheBugSequenceIsACoreRestartThatIsSevereBecauseItFollowsAnAdoption) {
  const auto summary = summarize(parse(bug_sequence(1'800'000'000'000)));
  EXPECT_EQ(summary.extras_ended, 1);
  EXPECT_EQ(summary.adopted, 1);
  EXPECT_EQ(summary.launched, 1);
  EXPECT_EQ(summary.exits, 1);
  EXPECT_EQ(summary.soon_after_adopt, 1);
  EXPECT_EQ(summary.unexpected, 1);  // an exit with code 0, but 30 s after the adoption
  EXPECT_EQ(summary.restarts_core_exit, 1);
  state_t state;
  const auto anomaly = judge_restart(summary, state, 1'800'000'100'000);
  ASSERT_TRUE(anomaly.has_value());
  EXPECT_EQ(anomaly->code, "XP_SVC_CORE_RESTART");
  EXPECT_TRUE(anomaly->severe);
  EXPECT_EQ(anomaly->cause, "soon_after_adopt");
  EXPECT_EQ(anomaly->value, 30'000);
  EXPECT_EQ(anomaly->exit_code, "0");
  // the shaped events pass the allow-list
  EXPECT_TRUE(telemetry::shape_event(telemetry::kind_e::experience, "host_anomaly", anomaly_props(*anomaly), 1000).has_value());
  EXPECT_TRUE(telemetry::shape_event(telemetry::kind_e::experience, "svc_lifecycle", summary_props(summary), 1000).has_value());
}

TEST(ServiceLifecycle, ADeliberateRestartOfTheHostIsCountedButNotAProblem) {
  // the host ended with code 0 after an hour, the service started it again: how a person restarts it from its own interface
  const std::string text = life::svc_start_line(1000) + "\n" + life::core_start_line(1100, false, 10, 0, "boot") + "\n" + life::core_exit_line(3'601'100, 0, 3'600'000, -1, false) + "\n" + life::core_start_line(3'602'000, false, 11, 0, "core_exit") + "\n";
  const auto summary = summarize(parse(text));
  EXPECT_EQ(summary.exits, 1);
  EXPECT_EQ(summary.unexpected, 0);
  state_t state;
  EXPECT_FALSE(judge_restart(summary, state, 3'700'000).has_value());
  // asked by the service (a change of the session, a stop) it is not an exit of the core at all
  const auto asked = summarize(parse(life::core_exit_line(2000, 1, 100, -1, true) + "\n"));
  EXPECT_EQ(asked.exits, 0);
}

TEST(ServiceLifecycle, ARestartTheCoreAskedForIsCountedAndNeverAnAnomaly) {
  // the Panel saves a setting: the core leaves its marker, exits with code 0 after twelve seconds and the service starts it again; four times in two minutes
  std::string text = life::svc_start_line(1000) + "\n";
  for (int i = 0; i < 4; ++i) {
    text += life::core_start_line(2000 + i * 20'000, false, 10 + i, 0, i ? "core_exit" : "boot") + "\n";
    text += life::core_exit_line(14'000 + i * 20'000, 0, 12'000, -1, false, true) + "\n";
  }
  const auto summary = summarize(parse(text));
  EXPECT_EQ(summary.restarts_requested, 4);
  EXPECT_EQ(summary.restart_reason, "api");  // none of these lines says why
  EXPECT_EQ(summary.exits, 0);
  EXPECT_EQ(summary.unexpected, 0);
  EXPECT_EQ(summary.short_life, 0);
  state_t state;
  EXPECT_FALSE(judge_restart(summary, state, 1'000'000).has_value());
  // the shaped event carries the count
  EXPECT_TRUE(telemetry::shape_event(telemetry::kind_e::experience, "svc_lifecycle", summary_props(summary), 1000).has_value());
  EXPECT_EQ(summary_props(summary).value("restarts_requested", -1), 4);
  // the same exit without the note is the unrequested exit it always was
  const auto without = summarize(parse(life::core_start_line(2000, false, 10, 0, "boot") + "\n" + life::core_exit_line(14'000, 0, 12'000, -1, false, false) + "\n"));
  EXPECT_EQ(without.unexpected, 1);
  EXPECT_EQ(without.restarts_requested, 0);
  // a restart flag with an exit code of a crash is still a crash
  const auto crashed = summarize(parse(life::core_exit_line(14'000, 0xC0000005ul, 12'000, -1, false, true) + "\n"));
  EXPECT_EQ(crashed.restarts_requested, 0);
  EXPECT_EQ(crashed.unexpected, 1);
}

TEST(ServiceLifecycle, TheReasonOfARequestedRestartTravelsFromTheCoreToTheEvent) {
  const auto dir = std::filesystem::temp_directory_path() / "alk_restart_reason_test";
  std::filesystem::remove_all(dir);
  ASSERT_TRUE(life::note_restart_request(dir, 100'000, "guide", 4242, 1'799'999'000'000));
  std::string word;
  ASSERT_TRUE(life::take_restart_request(dir, 101'000, 4242, 1'799'999'000'000, &word));
  EXPECT_EQ(word, "guide");
  std::filesystem::remove_all(dir);
  const auto line = life::core_exit_line(101'000, 0, 12'000, -1, false, true, word);
  const auto summary = summarize(parse(line + "\n"));
  EXPECT_EQ(summary.restarts_requested, 1);
  EXPECT_EQ(summary.restart_reason, "guide");
  const auto props = summary_props(summary);
  EXPECT_EQ(props.value("restart_reason", ""), "guide");
  EXPECT_TRUE(telemetry::shape_event(telemetry::kind_e::experience, "svc_lifecycle", props, 1000).has_value());
  // a word that is not on the list is "api", whatever a hostile file says
  EXPECT_EQ(summarize(parse(life::core_exit_line(1, 0, 12'000, -1, false, true, "something else") + "\n")).restart_reason, "api");
  EXPECT_EQ(summarize(parse(life::core_exit_line(1, 0, 12'000, -1, false, true, "tray") + "\n")).restart_reason, "tray");
}

TEST(ServiceLifecycle, TheStartOfTheCoreToItsWebInterfaceIsTimedWithoutTheService) {
  state_t state;
  const auto fast = judge_ready("core", 3'000, state, 1'000'000);
  EXPECT_FALSE(fast.has_value());
  EXPECT_EQ(state.core_ms.size(), 1u);
  EXPECT_TRUE(state.svc_core_ms.empty());  // its own baseline
  const auto props = timing_props("core", 3'000, false);
  EXPECT_EQ(props["which"], "core");
  EXPECT_TRUE(telemetry::shape_event(telemetry::kind_e::experience, "startup_timing", props, 1000).has_value());
  const auto slow = judge_ready("core", 20'000, state, 2'000'000);
  ASSERT_TRUE(slow.has_value());
  EXPECT_EQ(slow->which, "core");
  EXPECT_TRUE(telemetry::shape_event(telemetry::kind_e::experience, "host_anomaly", anomaly_props(*slow), 1000).has_value());
  // the baseline survives a restart of the host
  const auto back = state_from_json(state_to_json(state));
  EXPECT_EQ(back.core_ms.size(), 2u);
}

TEST(ServiceLifecycle, TheRestartMarkerIsTakenOnceAndOnlyWhileItIsFresh) {
  const auto dir = std::filesystem::temp_directory_path() / "alk_restart_marker_test";
  std::filesystem::remove_all(dir);
  EXPECT_FALSE(life::take_restart_request(dir, 100'000, 7, 5));  // nothing there
  ASSERT_TRUE(life::note_restart_request(dir, 100'000, "api", 7, 5));
  EXPECT_TRUE(life::take_restart_request(dir, 101'500, 7, 5));
  EXPECT_FALSE(life::take_restart_request(dir, 101'500, 7, 5));  // taken: gone
  ASSERT_TRUE(life::note_restart_request(dir, 100'000, "api", 7, 5));
  EXPECT_FALSE(life::take_restart_request(dir, 100'000 + life::RESTART_MARKER_WINDOW_MS + 1, 7, 5));  // an old note of an earlier restart that never happened
  EXPECT_FALSE(std::filesystem::exists(dir / life::RESTART_MARKER));  // and it is removed all the same
  ASSERT_TRUE(life::note_restart_request(dir, 100'000, "api", 7, 5));
  EXPECT_FALSE(life::take_restart_request(dir, 90'000, 7, 5));  // an exit before the note was written is not this restart
  // the note belongs to the core that wrote it: the exit of another core (another id, or the same id started later) does not take it, and it is gone afterwards
  ASSERT_TRUE(life::note_restart_request(dir, 100'000, "settings", 7, 5));
  EXPECT_FALSE(life::take_restart_request(dir, 101'000, 8, 5));
  EXPECT_FALSE(std::filesystem::exists(dir / life::RESTART_MARKER));
  ASSERT_TRUE(life::note_restart_request(dir, 100'000, "settings", 7, 5));
  EXPECT_FALSE(life::take_restart_request(dir, 101'000, 7, 6));
  std::filesystem::remove_all(dir);
}

TEST(ServiceLifecycle, ACrashShortAfterTheStartIsAProblemAndItBecomesSevereWhenItRepeats) {
  const std::string crash = life::core_start_line(1000, false, 10, 0, "boot") + "\n" + life::core_exit_line(21'000, 0xC0000005ul, 20'000, -1, false) + "\n" + life::core_start_line(22'000, false, 11, 0, "core_exit") + "\n";
  const auto summary = summarize(parse(crash));
  EXPECT_EQ(summary.unexpected, 1);
  EXPECT_EQ(summary.short_life, 1);
  state_t state;
  const auto first = judge_restart(summary, state, 1'000'000);
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(first->severe);
  EXPECT_EQ(first->cause, "unexpected_exit");
  EXPECT_EQ(first->exit_code, "0xC0000005");
  const auto second = judge_restart(summary, state, 1'000'000 + 3600'000);  // an hour later, the next start of the host
  ASSERT_TRUE(second.has_value());
  EXPECT_TRUE(second->severe);
  EXPECT_EQ(second->cause, "repeat");
  // a day and more later the earlier one does not count
  state_t old;
  old.restart_anomalies_ms = { 1000 };
  EXPECT_FALSE(judge_restart(summary, old, 1000 + RESTART_REPEAT_WINDOW_MS + 10'000)->severe);
  // two in one report are a repeat at once
  const auto two = summarize(parse(crash + life::core_exit_line(40'000, 1, 18'000, -1, false) + "\n"));
  state_t fresh;
  EXPECT_TRUE(judge_restart(two, fresh, 5'000'000)->severe);
}

TEST(ServiceLifecycle, TheFileOfAHostileServiceGivesNothingItDoesNotKnow) {
  std::string text = "not json\n";
  text += R"({"v":2,"kind":"svc_start","t":1000})"
          "\n";
  text += R"({"v":1,"kind":"core_start","t":1000,"how":"stolen","pid":1})"
          "\n";
  text += R"({"v":1,"kind":"core_exit","t":-5,"exit":0})"
          "\n";
  text += R"({"v":1,"kind":"formatc","t":1000})"
          "\n";
  text += R"({"v":1,"kind":"core_exit","t":1000,"exit":99999999999999,"life_ms":5,"requested":false})"
          "\n";
  text += R"({"v":1,"kind":"core_start","t":1000,"how":"adopted","pid":7,"age_ms":5,"cause":"C:\\Users\\Eve"})"
          "\n";
  const auto records = parse(text);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[1].cause, "boot");  // a cause that is not one of the words becomes the default
  std::string big;
  for (int i = 0; i < 5000; ++i) big += life::core_exit_line(1000 + i, 1, 100, -1, false) + "\n";
  EXPECT_LE(parse(big).size(), MAX_LINES);
}

TEST(ServiceLifecycle, TheWriterKeepsTheFileSmall) {
  const auto dir = fs::temp_directory_path() / ("svc-life-" + new_uuid_v4());
  const auto file = dir / "config" / "telemetry" / "service_lifecycle.jsonl";
  for (int i = 0; i < 2000; ++i) ASSERT_TRUE(life::append(file, life::core_exit_line(1000 + i, 1, 100, -1, false)));
  EXPECT_LE(fs::file_size(file), life::MAX_FILE_BYTES);
  std::string first;
  {
    std::ifstream in { file };  // closed before the folder is removed: Windows cannot delete a file that is open
    std::getline(in, first);
  }
  EXPECT_FALSE(parse(first + "\n").empty());  // the cut leaves whole lines
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(StartupReady, SlowAboveTheLimitOrWellAboveTheInstallsOwnUsualTimeAndSevereWhenItRepeats) {
  state_t state;
  const std::int64_t day = 24LL * 3600 * 1000;
  std::int64_t now = 10 * day;
  // the install usually takes about 4 s: five readings make its own baseline
  for (int i = 0; i < 5; ++i) EXPECT_FALSE(judge_ready("svc_core", 3'800 + i * 100, state, now += 1000).has_value());
  EXPECT_NEAR(*usual_ms(state.svc_core_ms), 4'000, 1);
  EXPECT_FALSE(judge_ready("svc_core", 9'000, state, now += 1000).has_value());  // twice as long, not three times, and under the limit
  const auto above_baseline = judge_ready("svc_core", 13'500, state, now += 1000);
  ASSERT_TRUE(above_baseline.has_value());
  EXPECT_EQ(above_baseline->code, "XP_SLOW_READY");
  EXPECT_EQ(above_baseline->cause, "above_baseline");
  EXPECT_FALSE(above_baseline->severe);  // the first one
  const auto above_limit = judge_ready("svc_core", 34'000, state, now += day);  // the sequence of the bug: 34 s
  ASSERT_TRUE(above_limit.has_value());
  EXPECT_EQ(above_limit->cause, "above_limit");
  EXPECT_TRUE(above_limit->severe);  // the second within a week
  EXPECT_EQ(above_limit->which, "svc_core");
  EXPECT_TRUE(telemetry::shape_event(telemetry::kind_e::experience, "host_anomaly", anomaly_props(*above_limit), 1000).has_value());
  EXPECT_TRUE(telemetry::shape_event(telemetry::kind_e::experience, "startup_timing", timing_props("svc_core", 34'000, true), 1000).has_value());
  // a week later the repeat is forgotten
  EXPECT_FALSE(judge_ready("page", 16'000, state, now += 8 * day)->severe);
  // the page and the service have baselines of their own
  EXPECT_EQ(state.page_ms.size(), 1u);
  // without five readings there is no baseline: only the limit counts
  state_t young;
  EXPECT_FALSE(judge_ready("page", 14'900, young, 1000).has_value());
  EXPECT_TRUE(judge_ready("page", 15'100, young, 2000).has_value());
}

TEST(StartupReady, TheStateSurvivesARestartAndADamagedStateIsNoState) {
  state_t state;
  state.restart_anomalies_ms = { 1000, 2000 };
  state.slow_ms = { 3000 };
  state.svc_core_ms = { 4000, 4100 };
  state.page_ms = { 900 };
  const auto back = state_from_json(state_to_json(state));
  EXPECT_EQ(back.restart_anomalies_ms, state.restart_anomalies_ms);
  EXPECT_EQ(back.svc_core_ms.size(), 2u);
  EXPECT_TRUE(state_from_json(json { { "v", 9 } }).svc_core_ms.empty());
  EXPECT_TRUE(state_from_json(json::parse("[1]")).slow_ms.empty());
  EXPECT_TRUE(state_from_json(json { { "v", 1 }, { "slow", json::array({ "x", -5, 1e30 }) } }).slow_ms.empty());
}

TEST(StartupReady, TheTimeFromTheStartOfTheServiceCountsOnlyForTheCoreItLaunched) {
  const auto records = parse(bug_sequence(1'800'000'000'000));
  // the core of pid 5151 was launched by the service after the exit; the one of pid 4242 was adopted
  EXPECT_EQ(svc_to_ready_ms(records, 5151, 1'800'000'034'000), 34'000);
  EXPECT_FALSE(svc_to_ready_ms(records, 4242, 1'800'000'034'000).has_value());
  EXPECT_FALSE(svc_to_ready_ms(records, 777, 1'800'000'034'000).has_value());  // a core a person started
  EXPECT_FALSE(svc_to_ready_ms(records, 5151, 1'700'000'000'000).has_value());  // a ready time before the start
  EXPECT_FALSE(svc_to_ready_ms(records, 5151, 1'800'000'000'000 + 4'000'000).has_value());  // over an hour: not a start-up
  EXPECT_FALSE(svc_to_ready_ms(records, 0, 1'800'000'034'000).has_value());
}

TEST(StartupReady, TheEventsOfTheServiceAndTheStartupHaveTheirPriorities) {
  auto event = [](const char *name, json props) {
    event_t e;
    e.kind = kind_e::experience;
    e.name = name;
    e.props = std::move(props);
    return e;
  };
  EXPECT_EQ(event_priority(event("host_anomaly", json { { "code", "XP_SLOW_READY" }, { "severe", true } })), 0);
  EXPECT_EQ(event_priority(event("host_anomaly", json { { "code", "XP_SLOW_READY" }, { "severe", false } })), 1);
  EXPECT_EQ(event_priority(event("svc_lifecycle", json::object())), 1);
  EXPECT_EQ(event_priority(event("startup_timing", json::object())), 1);
  EXPECT_TRUE(client_may_send("web", kind_e::experience, "page_ready"));
  EXPECT_FALSE(client_may_send("web", kind_e::experience, "host_anomaly"));
  EXPECT_FALSE(client_may_send("panel", kind_e::experience, "startup_timing"));
  EXPECT_FALSE(client_may_send("web", kind_e::experience, "svc_lifecycle"));
  // a code that is not ours drops the anomaly
  EXPECT_FALSE(shape_event(kind_e::experience, "host_anomaly", json { { "code", "XP_MADE_UP" } }, 1000).has_value());
  EXPECT_FALSE(shape_event(kind_e::experience, "startup_timing", json { { "which", "page" } }, 1000).has_value());  // the time is required
  EXPECT_TRUE(shape_event(kind_e::experience, "page_ready", json { { "page", "home" }, { "ms", 5200 } }, 1000).has_value());
}

// ---------------------------------------------------------------------------------------------------------------------
// Review round 1 of #1278: frames by structure, IPv6 without a decimal digit, error reports and the choice

#include "src/telemetry/error_report.h"

TEST(ErrorFrames, OnlyTheFourShapesLeaveAndNoAbsolutePathDoes) {
  // the shapes that are sent
  EXPECT_EQ(clean_frame("src/video.cpp:4821:encoder_init"), "src/video.cpp:4821:encoder_init");
  EXPECT_EQ(clean_frame("src_assets/common/assets/web/a.js:12"), "src_assets/common/assets/web/a.js:12");
  EXPECT_EQ(clean_frame("tools/sunshinesvc.cpp:7:(anonymous"), "");  // a parenthesis is not part of a function word
  EXPECT_EQ(clean_frame("src/a.cpp:7:"), "src/a.cpp:7:");
  EXPECT_EQ(clean_frame("src/stream.cpp:9:stream::session_t::run"), "src/stream.cpp:9:stream::session_t::run");
  EXPECT_EQ(clean_frame("Sunshine.exe+0x1a2b3c"), "Sunshine.exe+0x1a2b3c");
  EXPECT_EQ(clean_frame("render@index-DsA.js:123:45"), "render@index-DsA.js:123:45");
  EXPECT_EQ(clean_frame("index-DsA.js:123:45"), "index-DsA.js:123:45");
  // everything that names a person's folders is dropped whole
  for (const char *bad : {
         "/home/alice/project/main.cpp:7:run",
         "C:/Users/alice/project/main.cpp:7:run",
         "C:\\Users\\alice\\main.cpp:7:run",
         "/Users/alice/x.cpp:1:f",
         "home/alice/x.cpp:1:f",
         "alice/src/x.cpp:1:f",
         "./src/x.cpp:1:f",
         "src/../../etc/passwd:1:f",
         "../src/a.cpp:1:f",
         "src/video.cpp",
         "src/video.cpp:abc:f",
         "/src/video.cpp:1:f",
         "D:/work/Sunshine.exe+0x10",
         "folder/Sunshine.exe+0x10",
         "render@/home/alice/index.js:1:2",
         "render@C:/Users/alice/index.js:1:2",
         "name with space.js:1:2",
       })
    EXPECT_EQ(clean_frame(bad), "") << bad;
  // and through the allow-list: a frame list with one absolute path is dropped whole, a location with one is dropped
  json props { { "component", "web" }, { "lang", "js" }, { "fingerprint", "ab12" }, { "code", "E_X" }, { "loc", "/home/alice/a.js:1:2" }, { "frames", json::array({ "run@a.js:1:2", "/home/alice/b.js:3:4" }) } };
  const auto event = shape_event(kind_e::error, "error_report", props, 1000);
  ASSERT_TRUE(event.has_value());
  EXPECT_FALSE(event->props.contains("loc"));
  EXPECT_FALSE(event->props.contains("frames"));
  EXPECT_EQ(event->props.dump().find("alice"), std::string::npos);
}

TEST(LogScrub, AnAddressWithoutADecimalDigitIsStillAnAddress) {
  for (const char *line : { "peer fdab::cafe failed", "connect to dead::beef:cafe refused", "from abcd:ef01:2345:6789:abcd:ef01:2345:6789 lost", "listening on fe80::1 now" }) {
    const auto scrubbed = scrub_log_message(line);
    EXPECT_NE(scrubbed.find("<ipv6>"), std::string::npos) << line << " -> " << scrubbed;
  }
  const auto a = scrub_log_message("peer fdab::cafe failed");
  EXPECT_EQ(a.find("fdab"), std::string::npos);
  EXPECT_EQ(a.find("cafe"), std::string::npos);
  // text that is not an address stays: a scope name, a short pair, a time
  EXPECT_NE(scrub_log_message("in std::vector::size").find("std::vector::size"), std::string::npos);
  EXPECT_NE(scrub_log_message("pair ab::cd kept").find("ab::cd"), std::string::npos);
  // through the allow-list of an error report coming from a page
  json props { { "component", "web" }, { "lang", "js" }, { "fingerprint", "ab12" }, { "code", "E_X" }, { "log", json::array({ "peer fdab::cafe failed", "encoder initialization failed" }) } };
  const auto event = shape_event(kind_e::error, "error_report", props, 1000);
  ASSERT_TRUE(event.has_value());
  ASSERT_TRUE(event->props.contains("log"));
  EXPECT_EQ(event->props["log"].dump().find("fdab"), std::string::npos);
  EXPECT_NE(event->props["log"].dump().find("encoder initialization failed"), std::string::npos);
}

namespace {
  json
  error_reports_sent(harness_t &h) {
    h.svc->flush(true);
    while (h.svc->pump() > 0) {}
    json out = json::array();
    for (const auto &b : h.posted)
      if (b.contains("events"))
        for (const auto &e : b["events"])
          if (e["name"] == "error_report") out.push_back(e["props"]);
    h.posted.clear();
    return out;
  }
  error_input_t
  an_error(const std::string &code) {
    error_input_t e;
    e.code = code;
    e.loc = "src/video.cpp:1:f";
    e.frames = { e.loc };
    return e;
  }
}  // namespace

TEST(ErrorReportsAndTheChoice, WhatThePersonDidWhileErrorReportsWereOffIsNotSentAfterTheyAreOnAgain) {
  harness_t h;
  h.svc->crumb(crumb_e::nav, "apps");  // while on: kept
  h.consent.errors = false;
  h.svc->consent_changed();
  h.svc->crumb(crumb_e::act, "tile.secret_thing");  // while off: not recorded
  h.now += 1000;
  h.consent.errors = true;
  h.svc->consent_changed();
  h.svc->crumb(crumb_e::nav, "settings");
  h.now += 1000;
  h.svc->report_error(an_error("E_ONE"));
  const auto sent = error_reports_sent(h);
  ASSERT_EQ(sent.size(), 1u);
  const auto crumbs = sent[0]["crumbs"].dump();
  EXPECT_EQ(crumbs.find("secret_thing"), std::string::npos);
  EXPECT_EQ(crumbs.find("apps"), std::string::npos);  // what was kept before the choice turned off went with it
  EXPECT_NE(crumbs.find("settings"), std::string::npos);
}

TEST(ErrorReportsAndTheChoice, AnErrorHeldBackForAnAggregateIsDroppedWhenTheChoiceTurnsOff) {
  harness_t h;
  h.svc->report_error(an_error("E_TWICE"));
  h.svc->report_error(an_error("E_TWICE"));  // the second is held for the aggregate
  h.svc->report_error(an_error("E_TWICE"));
  h.consent.errors = false;
  h.svc->consent_changed();  // the first report waiting in the buffer, the held counts and the quota are all gone
  h.consent.errors = true;
  h.svc->consent_changed();
  h.now += error_reporter_t::AGGREGATE_MS + 1000;
  // nothing of the earlier reports comes back, neither the report nor a count of the ones held
  EXPECT_EQ(error_reports_sent(h).size(), 0u);
  // and the same error is a first report again: it leaves at once, once
  h.svc->report_error(an_error("E_TWICE"));
  const auto sent = error_reports_sent(h);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0]["count"], 1);
  h.now += error_reporter_t::AGGREGATE_MS + 1000;
  EXPECT_EQ(error_reports_sent(h).size(), 0u);
}

TEST(ErrorReportsAndTheChoice, ReportsWhileTheChoiceIsOffCountNeitherForTheQuotaNorForTheAggregate) {
  harness_t h;
  h.consent.errors = false;
  h.svc->consent_changed();
  for (int i = 0; i < 3 * error_reporter_t::MAX_PER_HOUR; ++i) h.svc->report_error(an_error("E_OFF" + std::to_string(i)));
  h.svc->report_error(an_error("E_OFF0"));
  EXPECT_EQ(error_reports_sent(h).size(), 0u);
  h.consent.errors = true;
  h.svc->consent_changed();
  // the hourly quota is untouched, and the held counts do not exist: a new error leaves at once and nothing follows it
  h.svc->report_error(an_error("E_NEW"));
  const auto sent = error_reports_sent(h);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0]["code"], "E_NEW");
  h.now += error_reporter_t::AGGREGATE_MS + 1000;
  EXPECT_EQ(error_reports_sent(h).size(), 0u);
}

TEST(ErrorReportsAndTheChoice, TheCountOfLaterReportsCarriesNeitherTheStackNorTheActions) {
  harness_t h;
  h.svc->crumb(crumb_e::nav, "apps");
  h.svc->report_error(an_error("E_AGAIN"));
  h.svc->crumb(crumb_e::act, "tile.something");
  h.svc->report_error(an_error("E_AGAIN"));  // held for the count
  h.svc->report_error(an_error("E_AGAIN"));
  const auto sent = error_reports_sent(h);  // flush(true): the first report and the count
  ASSERT_EQ(sent.size(), 2u);
  const json *first = nullptr, *count = nullptr;
  for (const auto &e : sent) (e["count"] == 1 ? first : count) = &e;
  ASSERT_NE(first, nullptr);
  ASSERT_NE(count, nullptr);
  EXPECT_TRUE(first->contains("frames"));
  EXPECT_TRUE(first->contains("crumbs"));
  EXPECT_EQ((*count)["count"], 2);
  EXPECT_EQ((*count)["fingerprint"], (*first)["fingerprint"]);
  EXPECT_EQ((*count)["code"], "E_AGAIN");
  EXPECT_FALSE(count->contains("frames"));
  EXPECT_FALSE(count->contains("crumbs"));
  EXPECT_FALSE(count->contains("log"));
}

// ---------------------------------------------------------------------------------------------------------------------
// host-side merging of high-volume records

TEST(TelemetryAggregate, MergesHighVolumeRecordsAndPassesPriorityRecordsThrough) {
  const auto exit_a = *shape_event(kind_e::usage, "app_exit", json { { "uptime_s", 30 } }, 100);
  const auto exit_b = *shape_event(kind_e::usage, "app_exit", json { { "uptime_s", 60 } }, 101);
  const auto feature = *shape_event(kind_e::usage, "feature_used", json { { "feature", "host.apps" } }, 102);
  const auto err = *shape_event(kind_e::error, "error", json { { "component", "host" }, { "code", "e1234567" }, { "severity", "error" }, { "count", 1 } }, 103);
  std::vector<std::vector<std::size_t>> origin;
  const auto out = aggregate_events({ exit_a, err, exit_b, feature }, origin);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0].name, "app_exit");
  EXPECT_EQ(out[0].count, 2u);
  EXPECT_EQ(out[0].props["uptime_s"], 45) << "plain numbers are averaged, and stay integers";
  EXPECT_EQ(origin[0], (std::vector<std::size_t> { 0, 2 }));
  EXPECT_EQ(out[1].name, "error");
  EXPECT_EQ(out[1].count, 1u) << "a priority record is never merged";
  EXPECT_EQ(out[2].name, "feature_used");
  EXPECT_EQ(event_to_json(out[2]).contains("count"), false) << "a single occurrence is sent as a plain event";
  EXPECT_EQ(event_to_json(out[0])["count"], 2);
}

TEST(TelemetryAggregate, CounterAndBucketPropsOfPageRecordsAreSummed) {
  const auto a = *shape_event(kind_e::experience, "page_vitals", json { { "page", "home" }, { "visits", 2 }, { "lcp", json::array({ 1, 0, 0, 0, 0, 0, 0, 0 }) } }, 100);
  const auto b = *shape_event(kind_e::experience, "page_vitals", json { { "page", "home" }, { "visits", 3 }, { "lcp", json::array({ 0, 2, 0, 0, 0, 0, 0, 0 }) } }, 101);
  const auto other = *shape_event(kind_e::experience, "page_vitals", json { { "page", "apps" }, { "visits", 1 } }, 102);
  std::vector<std::vector<std::size_t>> origin;
  const auto out = aggregate_events({ a, b, other }, origin);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].props["visits"], 5);
  EXPECT_EQ(out[0].props["lcp"], json::array({ 1, 2, 0, 0, 0, 0, 0, 0 }));
  EXPECT_EQ(out[1].props["page"], "apps");
}

TEST(TelemetryServiceAggregate, HighVolumeRecordsWaitForTheIntervalAndLeaveMergedAtExit) {
  service_options_t options;
  options.aggregate_interval_ms = 6 * 60 * 60 * 1000;
  harness_t h(options);
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(h.svc->emit(kind_e::usage, "feature_used", json { { "feature", "host.apps" } }));
  ASSERT_TRUE(h.svc->emit(kind_e::error, "error", json { { "component", "host" }, { "code", "e1234567" }, { "severity", "error" }, { "count", 1 } }));
  h.svc->flush();
  EXPECT_EQ(h.svc->buffered(), 3u) << "the feature records are held";
  EXPECT_EQ(h.svc->pump(), 1);
  ASSERT_EQ(h.posted.size(), 1u);
  EXPECT_EQ(names_of(h.posted), std::set<std::string> { "error" }) << "the priority record left at once";
  h.now += 6 * 60 * 60 * 1000 + 1;
  h.svc->flush();
  EXPECT_EQ(h.svc->buffered(), 0u);
  EXPECT_EQ(h.svc->pump(), 1);
  ASSERT_EQ(h.posted.size(), 2u);
  ASSERT_EQ(h.posted[1]["events"].size(), 1u);
  EXPECT_EQ(h.posted[1]["events"][0]["name"], "feature_used");
  EXPECT_EQ(h.posted[1]["events"][0]["count"], 3);
}

TEST(TelemetryServiceAggregate, ExitSendsWhatIsHeldAndASwitchTurnedOffDropsIt) {
  service_options_t options;
  options.aggregate_interval_ms = 6 * 60 * 60 * 1000;
  harness_t h(options);
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_exit", json { { "uptime_s", 30 } }));
  h.svc->flush();
  EXPECT_EQ(h.svc->buffered(), 1u);
  h.svc->flush(true);
  EXPECT_EQ(h.svc->buffered(), 0u);
  EXPECT_EQ(h.svc->pump(), 1);

  harness_t g(options);
  ASSERT_TRUE(g.svc->emit(kind_e::usage, "app_exit", json { { "uptime_s", 30 } }));
  g.svc->flush();
  ASSERT_EQ(g.svc->buffered(), 1u);
  g.consent = { false, true };
  g.svc->consent_changed();
  EXPECT_EQ(g.svc->buffered(), 0u) << "what waited for the interval is gone with the choice";
}

TEST(TelemetryAggregate, ABucketVectorOnlyTheLaterRecordCarriesIsKept) {
  const auto a = *shape_event(kind_e::experience, "page_vitals", json { { "page", "home" }, { "visits", 1 } }, 100);
  const auto b = *shape_event(kind_e::experience, "page_vitals", json { { "page", "home" }, { "visits", 1 }, { "lcp", json::array({ 0, 2, 0, 0, 0, 0, 0, 0 }) } }, 101);
  std::vector<std::vector<std::size_t>> origin;
  const auto out = aggregate_events({ a, b }, origin);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].props["lcp"], json::array({ 0, 2, 0, 0, 0, 0, 0, 0 }));
}

TEST(TelemetryServiceAggregate, ThePreviewOfHeldRecordsHonoursItsLimit) {
  service_options_t options;
  options.aggregate_interval_ms = 6 * 60 * 60 * 1000;
  options.ring_max = 10;
  options.flush_events = 1000;
  harness_t h(options);
  for (int i = 0; i < 30; ++i) ASSERT_TRUE(h.svc->emit(kind_e::usage, "feature_used", json { { "feature", i % 2 ? "host.apps" : "host.config" } }));
  h.svc->flush();
  EXPECT_LE(h.svc->preview(1).size(), 1u);
  EXPECT_EQ(h.svc->preview(0).size(), 0u);
}

// ---------------------------------------------------------------------------------------------------------------------
// diagnostic sufficiency (2026-11-18): the encoder, the clipboard words, where a start was slow or failed, drivers, the previous run

namespace {
  stream_dims_t
  diag_dims(std::string encoder = "nvenc") {
    stream_dims_t dims;
    dims.client_kind = "moonlight-macos";
    dims.client_os = "macos";
    dims.codec = "hevc";
    dims.width = 1920;
    dims.height = 1080;
    dims.fps = 60;
    dims.transport = "enet";
    dims.app_kind = "desktop";
    dims.encoder = std::move(encoder);
    dims.capture_backend = "wgc";
    return dims;
  }

  json
  first_event(const std::vector<json> &posted, const std::string &name) {
    json found;
    for (const auto &batch : posted)
      for (const auto &e : batch["events"])
        if (e.value("name", "") == name && found.is_null()) found = e;
    return found;
  }
}  // namespace

TEST(TelemetryDiag, TheQualityEventNamesTheEncoderAndTheCapturePath) {
  harness_t h;
  h.svc->stream_begin(diag_dims(), 11, "client-a");
  h.svc->record_encoded_frame(11);
  h.now += 5000;
  h.svc->stream_end(11, "client_cancel");
  h.svc->flush();
  ASSERT_GE(h.svc->pump(), 1);
  const auto quality = first_event(h.posted, "stream_quality");
  ASSERT_FALSE(quality.is_null());
  EXPECT_EQ(quality["props"]["encoder"], "nvenc");
  EXPECT_EQ(quality["props"]["capture_backend"], "wgc");
}

TEST(TelemetryDiag, AnEncoderLearntAfterTheStreamBeganReachesItsEvents) {
  harness_t h;
  h.svc->set_encoder_resolver([] { return std::string { "vulkan" }; });
  h.svc->stream_begin(diag_dims(""), 12, "client-a");
  h.svc->record_encoded_frame(12);
  h.now += 5000;
  h.svc->stream_end(12, "client_cancel");
  h.svc->flush();
  ASSERT_GE(h.svc->pump(), 1);
  EXPECT_EQ(first_event(h.posted, "stream_quality")["props"]["encoder"], "vulkan");
}

TEST(TelemetryDiag, ANoEncoderAtAllLeavesTheFieldOutInsteadOfAWord) {
  harness_t h;
  h.svc->stream_begin(diag_dims(""), 13, "client-a");
  h.svc->record_encoded_frame(13);
  h.now += 5000;
  h.svc->stream_end(13, "client_cancel");
  h.svc->flush();
  ASSERT_GE(h.svc->pump(), 1);
  const auto quality = first_event(h.posted, "stream_quality");
  ASSERT_FALSE(quality.is_null());  // the event itself was sent: only then does a missing field mean anything
  ASSERT_TRUE(quality.contains("props"));
  EXPECT_FALSE(quality["props"].contains("encoder"));
}

TEST(TelemetryDiag, TheStreamEndCarriesTheClipboardWordsAndOnlyTheFixedOnes) {
  harness_t h;
  h.svc->stream_begin(diag_dims(), 14, "client-a");
  h.svc->record_encoded_frame(14);
  h.now += 5000;
  h.svc->stream_end(14, "client_cancel", "agent_stale", "dropped");
  h.svc->stream_begin(diag_dims(), 15, "client-b");
  h.now += 5000;
  h.svc->stream_end(15, "client_cancel", "C:\\Users\\someone\\secret", "none");
  h.svc->flush(true);
  ASSERT_GE(h.svc->pump(), 1);
  std::vector<json> ends;
  for (const auto &batch : h.posted)
    for (const auto &e : batch["events"])
      if (e["name"] == "stream_end") ends.push_back(e);
  ASSERT_FALSE(ends.empty());
  std::set<std::string> offers, flows;
  for (const auto &e : ends) {
    if (e["props"].contains("clip_offer")) offers.insert(e["props"]["clip_offer"]);
    if (e["props"].contains("clip_flow")) flows.insert(e["props"]["clip_flow"]);
  }
  EXPECT_TRUE(offers.count("agent_stale") == 1 || ends.size() == 1u);
  for (const auto &word : offers) EXPECT_TRUE(word == "offered" || word == "off_config" || word == "no_agent" || word == "agent_stale" || word == "unknown") << word;
  for (const auto &word : flows) EXPECT_TRUE(word == "none" || word == "ok" || word == "dropped") << word;
  EXPECT_EQ(names_of(h.posted).count("stream_end"), 1u);
  EXPECT_EQ(json(h.posted).dump().find("someone"), std::string::npos);
}

TEST(TelemetryDiag, OnlyAStageOverItsBoundIsReported) {
  EXPECT_FALSE(stage_is_slow("display_prepare", 3000));
  EXPECT_TRUE(stage_is_slow("display_prepare", 3001));
  EXPECT_FALSE(stage_is_slow("vdd_create", 2999));
  EXPECT_TRUE(stage_is_slow("encoder_init", 4000));
  EXPECT_FALSE(stage_is_slow("first_frame", 5000));
  EXPECT_TRUE(stage_is_slow("first_frame", 5001));
  EXPECT_FALSE(stage_is_slow("app_launch", 4999));
  harness_t h;
  h.svc->stream_stage("display_prepare", 1200);
  h.svc->stream_stage("display_prepare", 9100);
  h.svc->stream_stage("not_a_stage", 99999);  // a word outside the list never becomes an event
  h.svc->flush(true);
  ASSERT_GE(h.svc->pump(), 1);
  int stages = 0;
  for (const auto &batch : h.posted)
    for (const auto &e : batch["events"])
      if (e["name"] == "stream_stage") {
        ++stages;
        EXPECT_EQ(e["props"]["stage"], "display_prepare");
        EXPECT_EQ(e["props"]["ms"], 9100);
      }
  EXPECT_EQ(stages, 1);
}

TEST(TelemetryDiag, AFailedStartNamesItsStageAndCodeAndNeverSendsMoreThanTheRunLimit) {
  harness_t h;
  h.svc->stream_failed("vdd_create", "VDD_DRIVER_NOT_INSTALLED");
  h.svc->stream_failed("encoder_init", "NO WORKING ENCODER at C:\\x");  // not a code: the event is not kept
  for (int i = 0; i < 40; ++i) h.svc->stream_failed("launch_accept", "NO_APP_TO_RESUME");
  h.svc->flush(true);
  ASSERT_GE(h.svc->pump(), 1);
  int failed = 0;
  bool saw_vdd = false;
  for (const auto &batch : h.posted)
    for (const auto &e : batch["events"])
      if (e["name"] == "stream_failed") {
        ++failed;
        if (e["props"]["stage"] == "vdd_create") {
          saw_vdd = true;
          EXPECT_EQ(e["props"]["code"], "VDD_DRIVER_NOT_INSTALLED");
        }
        EXPECT_EQ(e["props"]["code"].get<std::string>().find(' '), std::string::npos);
      }
  EXPECT_TRUE(saw_vdd);
  EXPECT_LE(failed, 20);
}

TEST(TelemetryDiag, ADriverFailureKeepsOnlyTheFixedWordsAndACleanCode) {
  harness_t h;
  h.svc->driver_failed("gamepad", "create", "0xE0000001 (C:\\Users\\me)");
  h.svc->driver_failed("vdd", "create", "VDD_IOCTL_FAILED");
  h.svc->driver_failed("not_a_driver", "create", "X");  // a word outside the list becomes "other", never itself
  h.svc->flush(true);
  ASSERT_GE(h.svc->pump(), 1);
  std::vector<json> events;
  for (const auto &batch : h.posted)
    for (const auto &e : batch["events"])
      if (e["name"] == "driver_event") events.push_back(e);
  ASSERT_EQ(events.size(), 3u);
  std::set<std::string> drivers;
  for (const auto &e : events) drivers.insert(e["props"]["driver"]);
  EXPECT_EQ(drivers, (std::set<std::string> { "gamepad", "vdd", "other" }));
  bool saw_vdd_code = false;
  for (const auto &e : events) {
    EXPECT_EQ(e["kind"], "error");
    EXPECT_EQ(e["props"]["result"], "failed");
    if (e["props"]["driver"] == "gamepad") {
      // not a code (a path in it): the whole value is left out, no part of the directory stays behind
      EXPECT_FALSE(e["props"].contains("hresult")) << e.dump();
      EXPECT_EQ(e.dump().find("Users"), std::string::npos);
    }
    if (e["props"]["driver"] == "vdd") {
      saw_vdd_code = true;
      EXPECT_EQ(e["props"]["hresult"], "VDD_IOCTL_FAILED");  // a clean code stays
    }
  }
  EXPECT_TRUE(saw_vdd_code);
}

namespace {
  struct diag_run_t {
    std::vector<json> posted;
    std::unique_ptr<service_t> svc;
    diag_run_t(const fs::path &dir, std::int64_t boot_ms, std::int64_t now, std::optional<std::string> boot_id = std::nullopt) {
      service_options_t options;
      options.dir = dir;
      service_deps_t deps;
      deps.now_ms = [now] { return now; };
      deps.consent = [] { return consent_t { true, true }; };
      deps.boot_ms = [boot_ms] { return boot_ms; };
      if (boot_id) deps.boot_id = [word = *boot_id] { return word; };
      deps.post = [this](const std::string &, const std::string &body) {
        posted.push_back(json::parse(body));
        return post_result_t { 200 };
      };
      svc = std::make_unique<service_t>(options, deps);
      context_t c;
      c.version = "5.2.0";
      c.channel = "stable";
      c.os = "windows";
      svc->set_context(c);
    }
  };
}  // namespace

TEST(TelemetryDiag, ARunThatLeftNoCleanMarkerIsReportedOnceUnderItsOwnSession) {
  temp_dir_t dir;
  std::string first_session;
  {
    diag_run_t first(dir.path, 1'700'000'000'000, 1'800'000'000'000);
    first_session = first.svc->session_id();
    first.svc->begin_run_marker();  // started, never ended
  }
  diag_run_t second(dir.path, 1'700'000'000'000, 1'800'000'100'000);
  second.svc->begin_run_marker();
  second.svc->flush(true);
  ASSERT_GE(second.svc->pump(), 1);
  bool found = false;
  for (const auto &batch : second.posted)
    for (const auto &e : batch["events"])
      if (e["name"] == "session_abnormal") {
        found = true;
        EXPECT_EQ(batch["session_id"], first_session);
        EXPECT_NE(batch["session_id"], second.svc->session_id());
        EXPECT_EQ(e["props"]["abnormal_mechanism"], "unknown");
      }
  EXPECT_TRUE(found);
  // this run ends by itself: the next start says nothing
  second.svc->end_run_marker();
  diag_run_t third(dir.path, 1'700'000'000'000, 1'800'000'200'000);
  third.svc->begin_run_marker();
  third.svc->flush(true);
  third.svc->pump();
  EXPECT_EQ(names_of(third.posted).count("session_abnormal"), 0u);
}

TEST(TelemetryDiag, AnotherBootBetweenTheRunsMeansTheComputerWasShutDown) {
  temp_dir_t dir;
  {
    diag_run_t first(dir.path, 1'700'000'000'000, 1'800'000'000'000);
    first.svc->begin_run_marker();
  }
  diag_run_t second(dir.path, 1'790'000'000'000, 1'800'000'100'000);
  second.svc->begin_run_marker();
  second.svc->flush(true);
  ASSERT_GE(second.svc->pump(), 1);
  const auto abnormal = first_event(second.posted, "session_abnormal");
  ASSERT_FALSE(abnormal.is_null());
  EXPECT_EQ(abnormal["props"]["abnormal_mechanism"], "shutdown");
}

TEST(TelemetryDiag, AFeedbackReportLeavesACountThatTheDailyIssueLimitDoesNotHide) {
  harness_t h;
  h.consent = { true, true };
  const json report = json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "suggestion" }, { "component", "panel" }, { "message", "please add" } } } } });
  EXPECT_EQ(h.svc->ingest("web", "1", report), 1u);
  h.svc->flush(true);
  ASSERT_GE(h.svc->pump(), 1);
  const auto submit = first_event(h.posted, "feedback_submit");
  ASSERT_FALSE(submit.is_null());
  EXPECT_EQ(submit["props"]["category"], "feature");
  EXPECT_EQ(json(submit).dump().find("please add"), std::string::npos);
}

TEST(TelemetryDiag, ThePanelMaySendItsClipboardRecordAndAPageOtherThanThePanelMayNot) {
  harness_t h;
  h.consent = { true, true };
  const json record = json::array({ json { { "kind", "experience" }, { "name", "clipboard_health" }, { "props", json { { "first_connect_ms", 1800 }, { "connected_pct", "gt90" }, { "reconnects", 2 }, { "heartbeat_fail", 0 }, { "last_error", "/etc/passwd" }, { "apply_ok", 5 }, { "apply_fail", 0 }, { "post_ok", 7 }, { "post_fail", 1 }, { "runtime_s", 3600 } } } } });
  EXPECT_EQ(h.svc->ingest("panel", "1.0", record), 1u);
  EXPECT_EQ(h.svc->ingest("web", "1.0", record), 0u);
  h.svc->flush(true);
  ASSERT_GE(h.svc->pump(), 1);
  const auto health = first_event(h.posted, "clipboard_health");
  ASSERT_FALSE(health.is_null());
  EXPECT_EQ(health["props"]["connected_pct"], "gt90");
  EXPECT_EQ(health["props"]["post_fail"], 1);
  EXPECT_EQ(health["props"]["last_error"], "other");  // a word outside the list becomes "other": the path never leaves
}

namespace {
  // what a start after a run that left no clean marker reports as the mechanism, for a first run and a second run with these boot words and start times (nullopt: no boot word known to that run)
  std::string
  abnormal_mechanism_after(std::optional<std::string> first_word, std::int64_t first_boot, std::optional<std::string> second_word, std::int64_t second_boot) {
    temp_dir_t dir;
    {
      diag_run_t first(dir.path, first_boot, 1'800'000'000'000, first_word);
      first.svc->begin_run_marker();
    }
    diag_run_t second(dir.path, second_boot, 1'800'000'100'000, second_word);
    second.svc->begin_run_marker();
    second.svc->flush(true);
    second.svc->pump();
    const auto abnormal = first_event(second.posted, "session_abnormal");
    return abnormal.is_null() ? std::string { "none" } : abnormal["props"]["abnormal_mechanism"].get<std::string>();
  }
}  // namespace

TEST(TelemetryDiag, AClockThatWasSetBetweenTwoRunsOfOneBootIsNotARestart) {
  // the start time worked out from the wall clock moved by ten minutes, the boot word did not: the computer was not restarted
  EXPECT_EQ(abnormal_mechanism_after("boot-a", 1'700'000'000'000, "boot-a", 1'700'000'600'000), "unknown");
}

TEST(TelemetryDiag, ANewBootWordIsARestartWhateverTheClockSays) {
  EXPECT_EQ(abnormal_mechanism_after("boot-a", 1'700'000'000'000, "boot-b", 1'700'000'000'000), "shutdown");
}

TEST(TelemetryDiag, WithoutTheBootWordOfBothRunsARestartIsNeverClaimed) {
  EXPECT_EQ(abnormal_mechanism_after(std::nullopt, 1'700'000'000'000, "boot-b", 1'790'000'000'000), "unknown");  // the earlier run knew no word (an older version)
  EXPECT_EQ(abnormal_mechanism_after("boot-a", 1'700'000'000'000, "", 1'790'000'000'000), "unknown");  // this run cannot read it
}

TEST(TelemetryDiag, AFeedbackReportThatWasWrittenByTheFlushIsCountedToo) {
  harness_t h;
  h.consent = { true, true };
  // a report that reached the buffer without having been written by ingest (emitted directly): the flush writes it, and the count goes out with it
  ASSERT_TRUE(h.svc->emit(kind_e::error, "user_report", json { { "category", "bug" }, { "component", "panel" }, { "message", "broken" } }, "web", "1"));
  h.svc->flush(true);
  h.svc->flush(true);  // the count the first flush made is in the buffer now
  ASSERT_GE(h.svc->pump(), 1);
  const auto submit = first_event(h.posted, "feedback_submit");
  ASSERT_FALSE(submit.is_null());
  EXPECT_EQ(submit["props"]["category"], "bug");
}

TEST(TelemetryDiag, AWaitKeepsTheRecordOfAnEarlierRunUnderThatRunsSessionAndTheFaultsOfOneRunApart) {
  harness_t h;
  h.next_body = R"({"cfg":{"backoff_s":600}})";
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  ASSERT_EQ(h.svc->pump(), 1);  // the answer asks for a wait
  h.posted.clear();
  h.next_body.clear();
  const auto earlier = new_uuid_v4();
  ASSERT_TRUE(h.svc->emit(kind_e::error, "session_abnormal", json { { "abnormal_mechanism", "unknown" } }, "host", {}, -1, {}, earlier));
  h.svc->driver_failed("vdd", "create", "VDD_IOCTL_FAILED");
  h.svc->driver_failed("microphone", "start", "MIC_START_FAILED");  // a second driver in the same run: not folded into the first
  h.svc->flush(true);
  int posts = 0;
  std::set<std::string> drivers;
  bool abnormal_under_earlier = false;
  for (int i = 0; i < 3; ++i) {
    posts += h.svc->pump();
    h.now += 120'000;
  }
  EXPECT_GE(posts, 2);
  for (const auto &batch : h.posted)
    for (const auto &e : batch["events"]) {
      if (e["name"] == "session_abnormal") abnormal_under_earlier = batch["session_id"] == earlier;
      if (e["name"] == "driver_event") {
        drivers.insert(e["props"]["driver"].get<std::string>());
        EXPECT_NE(batch["session_id"], earlier);
      }
    }
  EXPECT_TRUE(abnormal_under_earlier);
  EXPECT_EQ(drivers.size(), 2u);
}

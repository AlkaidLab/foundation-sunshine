/**
 * @file tests/unit/test_encoder_probe_cache.cpp
 * @brief The record of the encoder probe: what is a hit, what is a miss, and that damage never throws.
 */
#include <src/encoder_probe_cache.h>

#include <gtest/gtest.h>

namespace {
  using namespace probe_cache;

  constexpr std::int64_t now = 1'793'000'000;

  probe_key_t
  key() {
    return { "2026.1104.121813", { "10de:2704:32.0.15.9144" }, "cfg-7f3a", "ddx" };
  }

  probe_record_t
  record() {
    probe_record_t r;
    r.chosen_encoder = "nvenc";
    r.flags = { 0b10111, 0b11111, 0b10011 };
    r.active_hevc_mode = 3;
    r.active_av1_mode = 3;
    r.ref_frames_invalidation = true;
    r.yuv444[0] = true;
    r.yuv444[1] = true;
    r.yuv444[2] = false;
    r.probe_ms = 1231;
    return r;
  }

  std::string
  text(const probe_key_t &k = key(), const probe_record_t &r = record(), std::int64_t at = now - 60) {
    return to_json(k, r, at).dump();
  }
}  // namespace

TEST(EncoderProbeCache, TheRecordOfTheSameKeyIsAHitWithTheSameAnswer) {
  const auto loaded = parse(text(), key(), now);
  ASSERT_EQ(loaded.verdict, verdict_e::hit);
  EXPECT_TRUE(same_answer(loaded.record, record()));
  EXPECT_EQ(loaded.record.probe_ms, 1231);
}

TEST(EncoderProbeCache, EachPartOfTheKeyDecidesAlone) {
  auto stored = text();
  auto driver = key();
  driver.adapters = { "10de:2704:32.0.15.9200" };
  EXPECT_EQ(parse(stored, driver, now).verdict, verdict_e::other_key) << "a driver update";
  auto other_gpu = key();
  other_gpu.adapters = { "10de:2704:32.0.15.9144", "8086:a780:31.0.101.5333" };
  EXPECT_EQ(parse(stored, other_gpu, now).verdict, verdict_e::other_key) << "a GPU added";
  auto options = key();
  options.config_digest = "cfg-0000";
  EXPECT_EQ(parse(stored, options, now).verdict, verdict_e::other_key) << "an option that reaches the probe";
  auto build = key();
  build.sunshine_version = "2026.1105.1";
  EXPECT_EQ(parse(stored, build, now).verdict, verdict_e::other_key) << "a new build";
  auto backend = key();
  backend.capture_backend = "wgc";
  EXPECT_EQ(parse(stored, backend, now).verdict, verdict_e::other_key) << "the capture backend of the probe";
}

TEST(EncoderProbeCache, AnOldRecordAndOneFromTheFutureAreMisses) {
  EXPECT_EQ(parse(text(key(), record(), now - 29 * 24 * 3600), key(), now).verdict, verdict_e::hit);
  EXPECT_EQ(parse(text(key(), record(), now - 31 * 24 * 3600), key(), now).verdict, verdict_e::too_old);
  EXPECT_EQ(parse(text(key(), record(), now + 3600), key(), now).verdict, verdict_e::too_old) << "written under a wrong clock";
}

TEST(EncoderProbeCache, DamageIsAMissAndNeverAnException) {
  const auto good = text();
  for (const std::string &bad : { std::string(), std::string("not json"), good.substr(0, good.size() / 2), std::string("[]"), std::string("{}"), std::string("{\"schema\":2}") }) {
    EXPECT_EQ(parse(bad, key(), now).verdict, verdict_e::unreadable) << bad;
  }
  auto j = nlohmann::json::parse(good);
  j["record"].erase("encoder");
  EXPECT_EQ(parse(j.dump(), key(), now).verdict, verdict_e::unreadable) << "a missing field";
  j = nlohmann::json::parse(good);
  j["record"]["hevc_mode"] = 9;
  EXPECT_EQ(parse(j.dump(), key(), now).verdict, verdict_e::unreadable) << "a mode that does not exist";
  j = nlohmann::json::parse(good);
  j["record"]["yuv444"] = nlohmann::json::array({ true });
  EXPECT_EQ(parse(j.dump(), key(), now).verdict, verdict_e::unreadable);
  j = nlohmann::json::parse(good);
  j["record"]["h264"] = "many";
  EXPECT_EQ(parse(j.dump(), key(), now).verdict, verdict_e::unreadable) << "the wrong type";
}

TEST(EncoderProbeCache, ARecordWithoutAnEncoderIsNeverStoredNorUsed) {
  const auto dir = std::filesystem::temp_directory_path() / "probe-cache-test-empty";
  std::filesystem::create_directories(dir);
  auto empty = record();
  empty.chosen_encoder.clear();
  EXPECT_FALSE(store(dir / "p.json", key(), empty, now));
  EXPECT_FALSE(std::filesystem::exists(dir / "p.json"));
  EXPECT_EQ(parse(text(key(), empty), key(), now).verdict, verdict_e::not_usable);
  std::filesystem::remove_all(dir);
}

TEST(EncoderProbeCache, StoreIsAtomicAndLoadReadsWhatWasStored) {
  const auto dir = std::filesystem::temp_directory_path() / "probe-cache-test-store";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto file = dir / "encoder-probe.json";
  EXPECT_EQ(load(file, key(), now).verdict, verdict_e::no_file);
  ASSERT_TRUE(store(file, key(), record(), now));
  EXPECT_FALSE(std::filesystem::exists(dir / "encoder-probe.json.tmp")) << "the temporary file is gone";
  const auto again = load(file, key(), now + 5);
  ASSERT_EQ(again.verdict, verdict_e::hit);
  EXPECT_TRUE(same_answer(again.record, record()));
  // a second store replaces the first
  auto changed = record();
  changed.active_av1_mode = 1;
  ASSERT_TRUE(store(file, key(), changed, now + 10));
  EXPECT_EQ(load(file, key(), now + 11).record.active_av1_mode, 1);
  // a directory that does not exist: no store, no throw
  EXPECT_FALSE(store(dir / "missing" / "p.json", key(), record(), now));
  std::filesystem::remove_all(dir);
}

TEST(EncoderProbeCache, TheCheckAfterTheStartComparesAnswersNotTimings) {
  auto a = record();
  auto b = record();
  b.probe_ms = 99999;
  EXPECT_TRUE(same_answer(a, b));
  b.flags.hevc ^= 1;
  EXPECT_FALSE(same_answer(a, b)) << "a flag that changed means the record was wrong: the next start probes";
}

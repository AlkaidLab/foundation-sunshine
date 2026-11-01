/**
 * @file src/encoder_probe_cache.h
 * @brief What the encoder probe found, kept between starts, and when it may be used instead of probing.
 *
 * The probe creates and tests nine encoders in a row (1.2 s on an RTX 4080) and gives the same answer every start until the GPU, the driver or an option changes.
 * The record keeps the result of the last probe that SUCCEEDED, under a key of everything the answer depends on. A start whose key matches may answer from the
 * record and check it later; any difference, any damage and any age over `max_age` is a miss, and the probe runs as before. Nothing here talks to a driver.
 *
 * Not wired into main yet (see docs/startup.md): the check after the start has to run on a private copy of the encoder list, because the live flags are read by the
 * servers while it runs.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace probe_cache {

  inline constexpr int schema = 1;
  inline constexpr std::chrono::hours max_age { 24 * 30 };

  /** Everything the probe's answer depends on. Two keys are the same when every part is. */
  struct probe_key_t {
    std::string sunshine_version;       ///< a new build may change what is tested
    std::vector<std::string> adapters;  ///< "vendor:device:driver" of every GPU the probe may use, sorted
    std::string config_digest;          ///< a digest of the options that reach the probe (encoder, codec modes, per-vendor options, capture backend, HDR)
    std::string capture_backend;        ///< the backend the probe captures with (it may differ from the configured one)

    friend bool
    operator==(const probe_key_t &a, const probe_key_t &b) {
      return a.sunshine_version == b.sunshine_version && a.adapters == b.adapters && a.config_digest == b.config_digest && a.capture_backend == b.capture_backend;
    }
  };

  /** The bit masks of encoder_t::flag_e for the three codecs of the encoder that was chosen. */
  struct flags_t {
    std::uint32_t h264 = 0;
    std::uint32_t hevc = 0;
    std::uint32_t av1 = 0;
    friend bool
    operator==(const flags_t &, const flags_t &) = default;
  };

  struct probe_record_t {
    std::string chosen_encoder;  ///< encoder_t::name, never empty in a stored record
    flags_t flags;
    int active_hevc_mode = 0;  ///< 1 off, 2 main, 3 main10
    int active_av1_mode = 0;
    bool ref_frames_invalidation = false;
    bool yuv444[3] = { false, false, false };
    long long probe_ms = 0;  ///< what the probe took (for the log: what the cache saved)

    /** The answer a start gets from the record is the same as another probe's when this holds (probe_ms is not part of it). */
    friend bool
    same_answer(const probe_record_t &a, const probe_record_t &b) {
      return a.chosen_encoder == b.chosen_encoder && a.flags == b.flags && a.active_hevc_mode == b.active_hevc_mode && a.active_av1_mode == b.active_av1_mode &&
             a.ref_frames_invalidation == b.ref_frames_invalidation && a.yuv444[0] == b.yuv444[0] && a.yuv444[1] == b.yuv444[1] && a.yuv444[2] == b.yuv444[2];
    }
  };

  enum class verdict_e {
    hit,
    no_file,
    unreadable,   ///< damaged, or written by another schema
    other_key,    ///< the GPU, the driver, the build or an option is not the one it was written for
    too_old,
    not_usable,   ///< the record says no encoder
  };

  inline const char *
  describe(verdict_e v) {
    switch (v) {
      case verdict_e::hit: return "hit";
      case verdict_e::no_file: return "no cache file";
      case verdict_e::unreadable: return "cache file unreadable";
      case verdict_e::other_key: return "GPU, driver, build or options changed";
      case verdict_e::too_old: return "cache too old";
      case verdict_e::not_usable: return "cache holds no encoder";
    }
    return "?";
  }

  struct loaded_t {
    verdict_e verdict = verdict_e::no_file;
    probe_record_t record;
  };

  inline nlohmann::json
  to_json(const probe_key_t &key, const probe_record_t &record, std::int64_t written_at_s) {
    return {
      { "schema", schema },
      { "written_at", written_at_s },
      { "key", { { "version", key.sunshine_version }, { "adapters", key.adapters }, { "config", key.config_digest }, { "capture", key.capture_backend } } },
      { "record",
        { { "encoder", record.chosen_encoder },
          { "h264", record.flags.h264 },
          { "hevc", record.flags.hevc },
          { "av1", record.flags.av1 },
          { "hevc_mode", record.active_hevc_mode },
          { "av1_mode", record.active_av1_mode },
          { "ref_frames", record.ref_frames_invalidation },
          { "yuv444", { record.yuv444[0], record.yuv444[1], record.yuv444[2] } },
          { "probe_ms", record.probe_ms } } },
    };
  }

  /** Whatever the text holds, this never throws: anything unexpected is `unreadable`. */
  inline loaded_t
  parse(const std::string &text, const probe_key_t &wanted, std::int64_t now_s) {
    loaded_t out;
    try {
      const auto j = nlohmann::json::parse(text);
      if (!j.is_object() || j.value("schema", 0) != schema) {
        out.verdict = verdict_e::unreadable;
        return out;
      }
      const auto &k = j.at("key");
      probe_key_t stored;
      stored.sunshine_version = k.at("version").get<std::string>();
      stored.adapters = k.at("adapters").get<std::vector<std::string>>();
      stored.config_digest = k.at("config").get<std::string>();
      stored.capture_backend = k.at("capture").get<std::string>();
      const auto &r = j.at("record");
      probe_record_t rec;
      rec.chosen_encoder = r.at("encoder").get<std::string>();
      rec.flags.h264 = r.at("h264").get<std::uint32_t>();
      rec.flags.hevc = r.at("hevc").get<std::uint32_t>();
      rec.flags.av1 = r.at("av1").get<std::uint32_t>();
      rec.active_hevc_mode = r.at("hevc_mode").get<int>();
      rec.active_av1_mode = r.at("av1_mode").get<int>();
      rec.ref_frames_invalidation = r.at("ref_frames").get<bool>();
      const auto &y = r.at("yuv444");
      if (!y.is_array() || y.size() != 3) {
        out.verdict = verdict_e::unreadable;
        return out;
      }
      for (int i = 0; i < 3; ++i) rec.yuv444[i] = y[i].get<bool>();
      rec.probe_ms = r.value("probe_ms", 0LL);
      const auto written = j.at("written_at").get<std::int64_t>();
      if (rec.active_hevc_mode < 0 || rec.active_hevc_mode > 3 || rec.active_av1_mode < 0 || rec.active_av1_mode > 3) {
        out.verdict = verdict_e::unreadable;
        return out;
      }
      if (!(stored == wanted)) {
        out.verdict = verdict_e::other_key;
        return out;
      }
      if (written > now_s || std::chrono::seconds(now_s - written) > max_age) {
        out.verdict = verdict_e::too_old;  // from the future counts as old: the clock was wrong when it was written
        return out;
      }
      if (rec.chosen_encoder.empty()) {
        out.verdict = verdict_e::not_usable;
        return out;
      }
      out.record = std::move(rec);
      out.verdict = verdict_e::hit;
    }
    catch (...) {
      out.verdict = verdict_e::unreadable;
    }
    return out;
  }

  inline loaded_t
  load(const std::filesystem::path &file, const probe_key_t &wanted, std::int64_t now_s) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
      return {};
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      return { verdict_e::unreadable, {} };
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parse(text.str(), wanted, now_s);
  }

  /** Written next to the file and renamed over it, so a crash or a power cut leaves the old record or none, never half of one. A probe that found no encoder is not stored. */
  inline bool
  store(const std::filesystem::path &file, const probe_key_t &key, const probe_record_t &record, std::int64_t now_s) {
    if (record.chosen_encoder.empty()) {
      return false;
    }
    std::error_code ec;
    const auto temporary = std::filesystem::path(file).concat(".tmp");
    {
      std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
      if (!out) {
        return false;
      }
      out << to_json(key, record, now_s).dump(1);
      if (!out) {
        return false;
      }
    }
    std::filesystem::rename(temporary, file, ec);
    if (ec) {
      std::filesystem::remove(temporary, ec);
      return false;
    }
    return true;
  }

}  // namespace probe_cache

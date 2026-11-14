/**
 * @file tests/tools/ds5_matrix/matrix_engine.h
 * @brief Data-driven key runner of the DualSense tuning behaviour matrix.
 *
 * For one key: reset to the defaults, measure the probe, save a non-default value through the same
 * store path the settings page uses, measure again, test the extremes and the clamping, restore the
 * default and measure a third time.  A saved value that changes nothing the probe can see is a FAIL.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "src/ds5/tuning_profile.h"
#include "src/ds5/tuning_runtime.h"
#include "tests/tools/ds5_matrix/matrix_rig.h"

namespace ds5::matrix {
  using sig_t = std::vector<double>;

  struct result_t {
    int key {};
    std::string status;  ///< PASS, FAIL or NOT AUTOMATABLE
    std::string layer;
    std::string evidence;
  };

  struct value_case_t {
    json patch;  ///< one or more keys saved together
    std::string label;
    bool must_differ { true };  ///< the measurement has to move away from the default
  };

  struct case_t {
    int key {};
    std::function<sig_t(rig_t &)> probe;
    json setup = json::object();  ///< saved with the base measurement and with every value
    std::vector<value_case_t> values;
    /** @brief Direction and magnitude check; returns an empty string when the numbers are right. */
    std::function<std::string(const sig_t &base, const std::vector<sig_t> &values)> expect;
    bool range_check { true };
    double tolerance { 1e-6 };
    std::string note;
    /** @brief Extra scenario run after the restore step; returns an empty string when it holds. */
    std::function<std::string(rig_t &, std::string &detail)> extra;
  };

  inline std::string
  fmt(double value) {
    char text[32];
    std::snprintf(text, sizeof text, "%.5g", value);
    return text;
  }

  inline std::string
  fmt(const sig_t &sig, std::size_t limit = 24) {
    std::string out = "[";
    for (std::size_t i = 0; i < sig.size() && i < limit; ++i) out += (i ? " " : "") + fmt(sig[i]);
    if (sig.size() > limit) out += " ..";
    return out + "]";
  }

  inline bool
  approx(const sig_t &a, const sig_t &b, double tolerance) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (std::fabs(a[i] - b[i]) > tolerance) return false;
    }
    return true;
  }

  inline json
  to_json(const tuning::value_t &value) {
    return std::visit([](const auto &entry) -> json { return entry; }, value);
  }

  inline std::string
  key_name(int key) {
    const auto *descriptor = tuning::find_descriptor(static_cast<std::uint32_t>(key));
    return descriptor ? descriptor->key : "?";
  }

  /** @brief Every implemented key at its default, as one POST body (the preset key is a bundle, not a value). */
  inline json
  default_values() {
    json out = json::object();
    for (const auto &descriptor : tuning::descriptors()) {
      if (descriptor.id == 1 || !tuning::runtime::key_implemented(descriptor.id)) continue;
      out[descriptor.key] = to_json(tuning::default_value(descriptor));
    }
    return out;
  }

  inline bool
  post_ok(rig_t &rig, const json &values, std::string *why = nullptr) {
    const auto result = rig.post(values);
    const bool ok = result.ok && result.body.value("status", false);
    if (!ok && why) *why = "POST " + values.dump() + " -> " + std::to_string(result.http_status) + " " + result.body.value("error", std::string {});
    return ok;
  }

  inline bool
  reset(rig_t &rig, const json &setup, std::string *why = nullptr) {
    rig.clear_store();
    return setup.empty() || post_ok(rig, setup, why);
  }

  inline json
  merged(const json &a, const json &b) {
    json out = a;
    for (const auto &[k, v] : b.items()) out[k] = v;
    return out;
  }

  /** @brief The saved value as the GET snapshot reports it for @p key, or null. */
  inline json
  snapshot_value(rig_t &rig, int key) {
    for (const auto &entry : rig.get()["keys"]) {
      if (entry["id"] == key) return entry["value"];
    }
    return nullptr;
  }

  inline bool
  same_json(const json &a, const json &b) {
    if (a.is_number() && b.is_number()) return std::fabs(a.get<double>() - b.get<double>()) < 1e-4;
    return a == b;
  }

  inline result_t
  run_case(rig_t &rig, const case_t &c) {
    result_t result { c.key, "PASS", rig.name(), {} };
    std::vector<std::string> failures;
    std::string detail;
    const auto *descriptor = tuning::find_descriptor(static_cast<std::uint32_t>(c.key));
    const std::string name = descriptor ? descriptor->key : "?";
    std::string why;
    if (!reset(rig, c.setup, &why)) {
      result.status = "FAIL";
      result.evidence = "reset failed: " + why;
      return result;
    }
    const auto base = c.probe(rig);
    detail += "default " + fmt(base);
    std::vector<sig_t> sigs;
    bool primary_ok = false;
    for (std::size_t index = 0; index < c.values.size(); ++index) {
      const auto &value = c.values[index];
      if (!reset(rig, c.setup, &why) || !post_ok(rig, merged(c.setup, value.patch), &why)) {
        failures.push_back("save " + value.label + ": " + why);
        sigs.emplace_back();
        continue;
      }
      if (index == 0 && value.patch.size() == 1) {
        const auto saved = snapshot_value(rig, c.key);
        const auto want = value.patch.begin().value();
        if (!same_json(saved, want)) failures.push_back("GET shows " + saved.dump() + " after saving " + want.dump());
        primary_ok = true;
      }
      const auto sig = c.probe(rig);
      sigs.push_back(sig);
      detail += " | " + value.label + " " + fmt(sig);
      if (value.must_differ && approx(sig, base, c.tolerance)) {
        failures.push_back("no observable change for " + value.label);
      }
    }
    (void) primary_ok;
    const bool sigs_ok = std::none_of(sigs.begin(), sigs.end(), [](const sig_t &sig) { return sig.empty(); });
    if (c.expect && sigs_ok) {
      const auto text = c.expect(base, sigs);
      if (!text.empty()) failures.push_back(text);
    }

    if (c.range_check && descriptor) {
      const bool numeric = descriptor->type == tuning::ValueType::f32 || descriptor->type == tuning::ValueType::u32;
      if (numeric) {
        const double span = descriptor->max_value - descriptor->min_value;
        const bool integer = descriptor->type == tuning::ValueType::u32;
        const auto make = [&](double v) -> json { return integer ? json(static_cast<std::int64_t>(std::llround(v))) : json(v); };
        for (const bool upper : { false, true }) {
          const double edge = upper ? descriptor->max_value : descriptor->min_value;
          const double beyond = upper ? edge + span + 1.0 : edge - span - 1.0;
          if (!reset(rig, c.setup) || !post_ok(rig, merged(c.setup, { { name, make(edge) } }), &why)) {
            failures.push_back(std::string(upper ? "max" : "min") + " rejected: " + why);
            continue;
          }
          const auto at_edge = c.probe(rig);
          const auto over = rig.post(merged(c.setup, { { name, make(beyond) } }));
          bool clamped = false;
          for (const auto &applied : over.body.value("applied", json::array())) {
            if (applied.value("key", "") == name) clamped = applied.value("clamped", false);
          }
          const auto after = c.probe(rig);
          detail += std::string(" | ") + (upper ? "max " : "min ") + fmt(edge) + fmt(at_edge, 12);
          if (!over.ok || !clamped) failures.push_back(std::string(upper ? "above" : "below") + "-range value was not reported as clamped");
          if (!approx(after, at_edge, c.tolerance)) failures.push_back(std::string(upper ? "above" : "below") + "-range value behaves differently from the limit");
        }
      }
      else {
        const json bad = descriptor->type == tuning::ValueType::boolean ? json(7) : json("__not_a_value__");
        if (reset(rig, c.setup)) {
          const auto before = c.probe(rig);
          const auto rejected = rig.post(merged(c.setup, { { name, bad } }));
          const auto after = c.probe(rig);
          if (rejected.ok || rejected.http_status != 400) failures.push_back("invalid value " + bad.dump() + " was accepted (" + std::to_string(rejected.http_status) + ")");
          if (!approx(before, after, c.tolerance)) failures.push_back("a rejected value changed behaviour");
          detail += " | invalid " + bad.dump() + " -> " + std::to_string(rejected.http_status);
        }
      }
    }

    if (!c.values.empty() && descriptor) {
      if (reset(rig, c.setup) && post_ok(rig, merged(c.setup, c.values[0].patch)) &&
          post_ok(rig, merged(c.setup, { { name, to_json(tuning::default_value(*descriptor)) } }))) {
        const auto restored = c.probe(rig);
        detail += " | restored " + fmt(restored, 12);
        if (!approx(restored, base, c.tolerance)) failures.push_back("restoring the default did not restore the behaviour");
      }
      else {
        failures.push_back("restore step failed");
      }
    }
    if (c.extra && reset(rig, c.setup)) {
      const auto text = c.extra(rig, detail);
      if (!text.empty()) failures.push_back(text);
    }
    if (!c.note.empty()) detail += " | " + c.note;
    if (!failures.empty()) {
      result.status = "FAIL";
      std::string text;
      for (const auto &failure : failures) text += failure + "; ";
      detail = text + "|| " + detail;
    }
    result.evidence = detail;
    (void) reset(rig, json::object());
    return result;
  }

  /** @brief Keys that must be unavailable: refused with a reason, never stored, behaviour unchanged. */
  inline result_t
  run_unavailable(rig_t &rig, int key, const json &valid, const std::function<sig_t(rig_t &)> &probe, const std::string &where) {
    result_t result { key, "PASS", rig.name(), {} };
    std::vector<std::string> failures;
    const auto *descriptor = tuning::find_descriptor(static_cast<std::uint32_t>(key));
    const std::string name = descriptor ? descriptor->key : "?";
    reset(rig, json::object());
    const auto base = probe ? probe(rig) : sig_t {};
    const auto alone = rig.post({ { name, valid } });
    std::string reason;
    for (const auto &entry : alone.body.value("unavailable", json::array())) {
      if (entry.value("key", "") == name) reason = entry.value("reason", "");
    }
    if (alone.ok || alone.http_status != 409 || alone.body.value("code", 0) != 5) {
      failures.push_back("POST alone returned " + std::to_string(alone.http_status) + " code " + std::to_string(alone.body.value("code", -1)) + " (want 409 / 5)");
    }
    if (reason.empty()) failures.push_back("no reason in the unavailable list");
    const auto mixed = rig.post({ { name, valid }, { "ds5.rumble.strength", 0.5 } });
    bool listed = false;
    for (const auto &entry : mixed.body.value("unavailable", json::array())) listed = listed || entry.value("key", "") == name;
    if (!mixed.ok || !listed) failures.push_back("mixed POST did not list the key as unavailable");
    bool stored = true;
    std::string status;
    std::string get_reason;
    for (const auto &entry : rig.get()["keys"]) {
      if (entry["id"] == key) {
        stored = entry.value("stored", true);
        status = entry.value("status", "");
        get_reason = entry.value("reason", "");
      }
    }
    if (stored) failures.push_back("the unavailable value was stored");
    if (status != "unavailable") failures.push_back("GET status is '" + status + "'");
    if (get_reason.empty()) failures.push_back("GET carries no reason");
    if (!snapshot_value(rig, key).is_null() && !same_json(snapshot_value(rig, key), to_json(tuning::default_value(*descriptor)))) {
      failures.push_back("GET value moved away from the default");
    }
    std::string behaviour = "n/a";
    if (probe) {
      reset(rig, json::object());
      rig.post({ { name, valid }, { "ds5.rumble.strength", 1.0 } });
      const auto after = probe(rig);
      behaviour = approx(base, after, 1e-6) ? "unchanged" : "CHANGED";
      if (behaviour != "unchanged") failures.push_back("behaviour changed although the key is unavailable");
    }
    result.evidence = "POST alone " + std::to_string(alone.http_status) + " code " + std::to_string(alone.body.value("code", -1)) + "; GET status " + status +
                      " stored=" + (stored ? "true" : "false") + "; behaviour " + behaviour + "; reason: " + reason + (where.empty() ? "" : " | " + where);
    if (!failures.empty()) {
      result.status = "FAIL";
      std::string text;
      for (const auto &failure : failures) text += failure + "; ";
      result.evidence = text + "|| " + result.evidence;
    }
    reset(rig, json::object());
    return result;
  }
}  // namespace ds5::matrix

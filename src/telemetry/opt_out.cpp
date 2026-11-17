/**
 * @file src/telemetry/opt_out.cpp
 */
#include "opt_out.h"

#include <algorithm>
#include <cctype>
#include <random>

namespace telemetry::opt_out {

  using nlohmann::json;

  namespace {
    std::uint64_t
    new_id() {
      static thread_local std::mt19937_64 rng { std::random_device {}() };
      std::uint64_t id = 0;
      while (id == 0) id = rng();
      return id;
    }

    bool
    choice_ok(const std::string &c) {
      return c == "usage" || c == "errors";
    }

    bool
    product_ok(const std::string &p) {
      return p == "host" || p == "panel" || p == "web" || p == "sparxie-launch";
    }

    bool
    version_ok(const std::string &v) {
      if (v.empty() || v.size() > 64) return false;
      return std::all_of(v.begin(), v.end(), [](unsigned char c) { return c >= 0x80 || std::isalnum(c) || c == '.' || c == '_' || c == '+' || c == '-' || c == ' '; });
    }
  }  // namespace

  bool
  is_uuid(std::string_view text) {
    if (text.size() != 36) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
      const char c = text[i];
      if (i == 8 || i == 13 || i == 18 || i == 23) {
        if (c != '-') return false;
      }
      else if (!std::isxdigit(static_cast<unsigned char>(c))) {
        return false;
      }
    }
    return true;
  }

  json
  state_to_json(const state_t &s) {
    return { { "v", 1 }, { "usage", s.usage }, { "errors", s.errors } };
  }

  state_t
  state_from_json(const json &j) {
    state_t s;
    if (!j.is_object() || j.value("v", 0) != 1 || !j.contains("usage") || !j["usage"].is_boolean() || !j.contains("errors") || !j["errors"].is_boolean()) return s;
    s.known = true;
    s.usage = j["usage"].get<bool>();
    s.errors = j["errors"].get<bool>();
    return s;
  }

  std::vector<std::string>
  turned_off(const state_t &before, bool usage_now, bool errors_now) {
    std::vector<std::string> out;
    if (!before.known) return out;
    if (before.usage && !usage_now) out.push_back("usage");
    if (before.errors && !errors_now) out.push_back("errors");
    return out;
  }

  std::vector<record_t>
  parse(std::string_view text) {
    std::vector<record_t> out;
    if (text.size() > MAX_FILE_BYTES) return out;
    std::size_t pos = 0;
    while (pos < text.size() && out.size() < MAX_RECORDS) {
      auto end = text.find('\n', pos);
      if (end == std::string_view::npos) end = text.size();
      const auto line = text.substr(pos, end - pos);
      pos = end + 1;
      if (line.empty()) continue;
      const json j = json::parse(line.begin(), line.end(), nullptr, false);
      if (!j.is_object() || j.value("v", 0) != 1) continue;
      record_t r;
      r.choice = j.value("choice", std::string {});
      r.product = j.value("product", std::string {});
      r.version = j.value("version", std::string {});
      r.install_id = j.value("install_id", std::string {});
      if (!choice_ok(r.choice) || !product_ok(r.product) || !version_ok(r.version) || !is_uuid(r.install_id)) continue;
      if (!j.contains("t") || !j["t"].is_number_integer() || !j.contains("next") || !j["next"].is_number_integer() || !j.contains("tries") || !j["tries"].is_number_integer()) continue;
      r.t = j["t"].get<std::int64_t>();
      r.next_try_ms = j["next"].get<std::int64_t>();
      r.tries = static_cast<int>(std::clamp<std::int64_t>(j["tries"].get<std::int64_t>(), 0, MAX_TRIES));
      if (r.t <= 0) continue;
      // a file written before the id existed has none: make one now and keep it at the next write
      if (j.contains("id") && j["id"].is_number_unsigned()) r.id = j["id"].get<std::uint64_t>();
      else if (j.contains("id") && j["id"].is_number_integer() && j["id"].get<std::int64_t>() > 0) r.id = static_cast<std::uint64_t>(j["id"].get<std::int64_t>());
      // a number that is missing or already taken by an earlier record (a hand-edited or damaged file) gets a new one: the send result
      // is matched to its record by the number, so two records that share it would both be removed after one delivery
      while (r.id == 0 || std::any_of(out.begin(), out.end(), [&](const record_t &other) { return other.id == r.id; })) r.id = new_id();
      out.push_back(std::move(r));
    }
    return out;
  }

  std::string
  serialize(const std::vector<record_t> &records) {
    std::string out;
    for (const auto &r : records) {
      const json j { { "v", 1 }, { "id", r.id }, { "choice", r.choice }, { "product", r.product }, { "version", r.version }, { "install_id", r.install_id }, { "t", r.t }, { "next", r.next_try_ms }, { "tries", r.tries } };
      out += j.dump();
      out += '\n';
    }
    return out;
  }

  namespace {
    // a record that will never be sent again: too old or out of tries
    void
    drop_dead(std::vector<record_t> &records, std::int64_t now_ms) {
      records.erase(std::remove_if(records.begin(), records.end(), [&](const record_t &r) { return r.tries >= MAX_TRIES || now_ms - r.t > MAX_AGE_MS; }), records.end());
    }
  }  // namespace

  void
  add(std::vector<record_t> &records, const std::vector<std::string> &choices, const std::vector<product_version_t> &products, const std::string &install_id, std::int64_t now_ms) {
    if (!is_uuid(install_id)) return;  // no identity, nothing to say it about
    drop_dead(records, now_ms);  // an expired record must not count as waiting or take a place in the queue
    for (const auto &choice : choices) {
      if (!choice_ok(choice)) continue;
      for (const auto &p : products) {
        if (!product_ok(p.product) || !version_ok(p.version)) continue;
        const bool waiting = std::any_of(records.begin(), records.end(), [&](const record_t &r) { return r.choice == choice && r.product == p.product && r.install_id == install_id; });
        if (waiting || records.size() >= MAX_RECORDS) continue;
        std::uint64_t id = new_id();
        while (std::any_of(records.begin(), records.end(), [&](const record_t &r) { return r.id == id; })) id = new_id();
        records.push_back({ choice, p.product, p.version, install_id, now_ms, now_ms, 0, id });
      }
    }
  }

  std::vector<std::size_t>
  due(std::vector<record_t> &records, std::int64_t now_ms) {
    drop_dead(records, now_ms);
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < records.size(); ++i)
      if (records[i].next_try_ms <= now_ms) out.push_back(i);
    return out;
  }

  void
  failed(record_t &r, std::int64_t now_ms) {
    ++r.tries;
    const auto step = RETRY_BASE_MS << std::min(r.tries - 1, 7);
    r.next_try_ms = now_ms + std::min<std::int64_t>(step, 24LL * 3600 * 1000);
  }

  json
  body_of(const record_t &record, std::string_view session_id) {
    event_t e;
    e.t = record.t;
    e.kind = kind_e::usage;
    e.name = "opt_out";
    e.props = json { { "choice", record.choice } };
    e.product = record.product;
    e.version = record.version;
    const auto shaped = shape_event(e.kind, e.name, e.props, record.t);
    if (!shaped) return nullptr;
    e.t = shaped->t;
    e.props = shaped->props;
    context_t context;  // empty on purpose: only the fields below are kept
    context.install_id = record.install_id;
    context.session_id = std::string { session_id };
    auto envelopes = build_envelopes(context, record.product, { e });
    if (envelopes.empty()) return nullptr;
    auto body = std::move(envelopes.front());
    for (const char *key : { "channel", "os", "os_version", "arch", "gpu_vendor", "locale" }) body.erase(key);
    return body;
  }

}  // namespace telemetry::opt_out

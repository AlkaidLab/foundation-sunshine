/**
 * @file src/telemetry/service.cpp
 * @brief The telemetry sender: buffer, queue, retry, consent, identity and stream trackers.
 */
#include "service.h"
#include "error_code.h"
#include "opt_out.h"

#include "src/crash/crash_context.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <tuple>

using json = nlohmann::json;
using namespace std::literals;
namespace fs = std::filesystem;

namespace telemetry {

  namespace {
    /** The build identity as an allow-list token: lower-case ASCII letters and digits with `.`, `_` and `-`, at most 40 characters
     *  (`v2026.1114.173055.杂鱼` becomes `v2026.1114.173055`). An error event carries it so a report names the build that raised it. */
    std::string
    build_token(std::string_view version) {
      std::string out;
      for (const unsigned char c : version) {
        if (out.size() >= 40) break;
        if (c < 0x80 && std::isalnum(c))
          out.push_back(static_cast<char>(std::tolower(c)));
        else if (!out.empty() && (c == '.' || c == '-') && out.back() != '.' && out.back() != '-')
          out.push_back(static_cast<char>(c));
        else if (!out.empty() && out.back() != '_' && out.back() != '.' && out.back() != '-')
          out.push_back('_');
      }
      while (!out.empty() && !std::isalnum(static_cast<unsigned char>(out.back()))) out.pop_back();
      return out;
    }

    bool
    truthy(const std::string &v) {
      return v == "on" || v == "true" || v == "1" || v == "enabled" || v == "yes";
    }
    bool
    falsy(const std::string &v) {
      return v == "off" || v == "false" || v == "0" || v == "disabled" || v == "no";
    }

    std::mt19937_64 &
    rng() {
      static std::mt19937_64 engine { std::random_device {}() ^ (static_cast<std::uint64_t>(std::random_device {}()) << 32) };
      return engine;
    }

    std::mutex &
    rng_mutex() {
      static std::mutex m;
      return m;
    }

    // a version as a page hands it over: the whole string or nothing (a name or a path with its separators stripped would still be a name)
    std::string
    sanitize_version(std::string_view version) {
      return safe_version(version);
    }
  }  // namespace

  std::string
  new_uuid_v4() {
    std::array<std::uint8_t, 16> b {};
    {
      std::lock_guard lock { rng_mutex() };
      for (std::size_t i = 0; i < 16; i += 8) {
        auto v = rng()();
        for (std::size_t j = 0; j < 8; ++j) b[i + j] = static_cast<std::uint8_t>(v >> (8 * j));
      }
    }
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0f) | 0x40);
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3f) | 0x80);
    char out[40];
    std::snprintf(out, sizeof(out), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return out;
  }

  consent_t
  consent_from_config(const std::string &usage_value, const std::string &errors_value, bool usage_key_present, bool errors_key_present) {
    consent_t c;
    c.usage = !(usage_key_present && falsy(usage_value));
    if (errors_key_present && (truthy(errors_value) || falsy(errors_value))) c.errors = truthy(errors_value);
    // a person who answered "off" to the old single question said no to everything that left the computer: that stays no for error reports until they turn it on
    else c.errors = !(usage_key_present && usage_value == "off");
    return c;
  }

  // ---------------------------------------------------------------------------------------------------------------------

  service_t::service_t(service_options_t options, service_deps_t deps):
      options_(std::move(options)), deps_(std::move(deps)), queue_(options_.dir / "queue", options_.queue), session_id_(new_uuid_v4()) {
    config_.logs_per_day = options_.anomaly_logs_per_day;  // until the server says otherwise
    try {
      std::ifstream in(options_.dir / "remote_config.json");
      const auto stored = in ? json::parse(in, nullptr, false) : json();
      if (!stored.is_discarded() && stored.is_object()) config_ = parse_remote_config(stored, 0, options_.anomaly_logs_per_day);
    }
    catch (...) {
    }
    if (!deps_.now_ms) deps_.now_ms = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); };
    try {
      std::ifstream wait(options_.dir / "remote_wait");
      std::int64_t deadline = 0;
      if (wait >> deadline && deadline > deps_.now_ms()) {
        backoff_until_ms_.store(deadline, std::memory_order_relaxed);  // a pause or a wait the server asked for is still on
        wait_period_ms_ = deadline - deps_.now_ms();
      }
    }
    catch (...) {
    }
    if (!deps_.consent) deps_.consent = [] { return consent_t {}; };
    if (!deps_.random01) {
      deps_.random01 = [] {
        std::lock_guard lock { rng_mutex() };
        return std::uniform_real_distribution<double>(0.0, 1.0)(rng());
      };
    }
    last_flush_ms_ = deps_.now_ms();
    last_aggregate_flush_ms_ = last_flush_ms_;
    try {
      std::ifstream seen(options_.dir / "products_seen.json");
      const auto stored = seen ? json::parse(seen, nullptr, false) : json();
      if (!stored.is_discarded() && stored.is_object()) {
        for (const auto &[product, version] : stored.items())
          if (known_product(product) && version.is_string() && products_seen_.size() < 4) products_seen_[product] = sanitize_version(version.get<std::string>());
      }
      std::ifstream pending(options_.dir / "opt_out_pending.jsonl");
      if (pending) opt_outs_ = opt_out::parse(std::string { std::istreambuf_iterator<char> { pending }, std::istreambuf_iterator<char> {} });
    }
    catch (...) {
    }
  }

  consent_t
  service_t::consent_now() const {
    try {
      return deps_.consent();
    }
    catch (...) {
      return consent_t { false, false };
    }
  }

  // ---- identity ----

  std::string
  service_t::install_id() {
    const auto c = consent_now();
    std::lock_guard lock { mutex_ };
    if (!c.any()) return {};
    if (!install_id_.empty()) return install_id_;
    const auto path = options_.dir / "install_id";
    std::error_code ec;
    {
      std::ifstream in(path);
      std::string text;
      if (in && std::getline(in, text)) {
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
        // a UUID in text form: 36 characters
        if (text.size() == 36) {
          install_id_ = text;
          return install_id_;
        }
      }
    }
    fs::create_directories(options_.dir, ec);
    install_id_ = new_uuid_v4();
    auto temp = path;
    temp += ".tmp";
    {
      std::ofstream out(temp, std::ios::trunc);
      out << install_id_ << '\n';
    }
    fs::rename(temp, path, ec);
    return install_id_;
  }

  std::string
  service_t::rotate_install_id() {
    {
      std::lock_guard lock { mutex_ };
      install_id_.clear();
      std::error_code ec;
      fs::remove(options_.dir / "install_id", ec);
    }
    return install_id();
  }

  std::optional<json>
  service_t::identity() {
    const auto c = consent_now();
    if (!c.any()) return std::nullopt;
    const auto id = install_id();
    if (id.empty()) return std::nullopt;
    json j {
      { "v", WIRE_VERSION },
      { "install_id", id },
      { "session_id", session_id_ },
      { "endpoint", options_.endpoint },
      { "usage", c.usage },
      { "errors", c.errors },
    };
    return j;
  }

  void
  service_t::set_context(context_t context) {
    std::lock_guard lock { mutex_ };
    context_ = std::move(context);
  }

  context_t
  service_t::context() const {
    std::lock_guard lock { mutex_ };
    return context_;
  }

  // ---- events ----

  namespace {
    /** The shape of a session id (8-4-4-4-12 hex digits), so a record about an earlier run can only carry an id this host wrote. */
    bool
    new_uuid_valid(std::string_view id) {
      if (id.size() != 36) return false;
      for (std::size_t i = 0; i < id.size(); ++i) {
        const auto c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
          if (c != '-') return false;
        }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
          return false;
        }
      }
      return true;
    }

    /** Standard base64 (RFC 4648 section 4), padded; an empty string when the text is not that. */
    std::string
    base64_decode(std::string_view text) {
      static const auto value = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
      };
      if (text.empty() || text.size() % 4 != 0) return {};
      std::string out;
      out.reserve(text.size() / 4 * 3);
      for (std::size_t i = 0; i < text.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
          const auto c = static_cast<unsigned char>(text[i + k]);
          if (c == '=' && i + 4 == text.size() && k >= 2) {
            v[k] = 0;
            ++pad;
          }
          else if (pad == 0 && value(c) >= 0) v[k] = value(c);
          else return {};
        }
        const auto n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
        out.push_back(static_cast<char>((n >> 16) & 0xff));
        if (pad < 2) out.push_back(static_cast<char>((n >> 8) & 0xff));
        if (pad < 1) out.push_back(static_cast<char>(n & 0xff));
      }
      return out;
    }

    /** PNG, JPEG or WebP by the first bytes (the page's word for the type is not used). */
    bool
    is_screenshot_image(const std::string &b) {
      if (b.size() >= 8 && b.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) return true;
      if (b.size() >= 3 && static_cast<unsigned char>(b[0]) == 0xff && static_cast<unsigned char>(b[1]) == 0xd8 && static_cast<unsigned char>(b[2]) == 0xff) return true;
      return b.size() >= 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0;
    }

    /**
     * The screenshot of a feedback report: checked again (the page's own check is not trusted), uploaded like the run log, and its reference put into `props` (shot_ref) or the word for why not (shot_state).
     * -> "uploaded" or that word. The Worker checks the picture once more and takes the metadata out of what it keeps.
     */
    std::string
    attach_screenshot(const service_deps_t &deps, const std::string &endpoint, const std::string &id, const std::string &version, const std::string &encoded, json &props) {
      const auto left_out = [&](std::string state) {
        props["shot_state"] = state;
        return state;
      };
      if (encoded.size() > MAX_USER_SHOT_BYTES / 3 * 4 + 8) return left_out("too_large");
      const auto image = base64_decode(encoded);
      if (image.size() < 16 || !is_screenshot_image(image)) return left_out("rejected");
      if (image.size() > MAX_USER_SHOT_BYTES) return left_out("too_large");
      if (!deps.session_shot || version.empty()) return left_out("unavailable");
      try {
        if (const auto ref = deps.session_shot(endpoint, id, version, image)) {
          props["shot_ref"] = *ref;
          return "uploaded";
        }
        return left_out(deps.session_shot_state ? deps.session_shot_state() : "unavailable");
      }
      catch (...) {
        return left_out("unavailable");
      }
    }
  }  // namespace

  bool
  service_t::emit(kind_e kind, std::string_view name, const json &props, std::string_view product, std::string_view version, int priority, std::string_view report_id, std::string_view session_id) {
    try {
      if (!known_product(product)) return false;
      // a report the person pressed 发送 for is their consent for that one send: it does not wait for the error-report switch
      if (!consent_now().allows(kind) && !is_user_report(kind, name)) {
        std::lock_guard lock { mutex_ };
        ++counts_.refused;
        return false;
      }
      auto shaped = shape_event(kind, name, props, deps_.now_ms());
      if (!shaped) return false;
      shaped->product = std::string { product };
      shaped->version = sanitize_version(version);
      shaped->priority = priority;
      if (!session_id.empty() && new_uuid_valid(session_id)) shaped->session_id = std::string { session_id };
      if (is_user_report(kind, name)) shaped->report_id = std::string { report_id };
      bool wake = false;
      {
        std::lock_guard lock { mutex_ };
        ring_.push_back(std::move(*shaped));
        ++counts_.emitted;
        while (ring_.size() > options_.ring_max) {
          ring_.pop_front();
          ++counts_.dropped;
        }
        wake = ring_.size() >= options_.flush_events;
      }
      if (wake && wake_) wake_();
      return true;
    }
    catch (...) {
      return false;
    }
  }

  namespace {
    // the category of a feedback report in the Worker's words (feedback_submit); the page's own words map to four
    json
    feedback_submit_props(const json &report_props) {
      const auto category = report_props.is_object() && report_props.contains("category") && report_props["category"].is_string() ? report_props["category"].get<std::string>() : std::string {};
      return json { { "category", category == "bug" ? "bug" : category == "suggestion" ? "feature" : category == "performance" || category == "compat" ? "improvement" : "other" } };
    }
  }  // namespace

  bool
  service_t::persist_report(const event_t &event) {
    try {
      // the identity was fixed when the report was accepted: nothing is looked up again (the switches may have changed since)
      auto ctx = context();
      const bool alone = event.report_install.empty();
      ctx.install_id = alone ? event.report_id : event.report_install;
      if (ctx.install_id.empty()) return false;
      ctx.session_id = alone ? new_uuid_v4() : session_id_;
      for (auto envelope : build_envelopes(ctx, event.product, { event })) {
        if (alone) envelope["report_only"] = true;
        std::lock_guard lock { queue_mutex_ };
        if (!queue_.push(envelope, 0)) return false;
      }
      return true;
    }
    catch (const std::exception &) {
      return false;
    }
  }

  std::size_t
  service_t::ingest(std::string_view product, std::string_view version, const json &events, ingest_report_t *report) {
    if (!events.is_array() || !known_product(product)) return 0;
    note_product(product, version);
    std::size_t kept = 0, seen = 0;
    for (const auto &e : events) {
      if (++seen > MAX_BATCH_EVENTS) break;
      if (!e.is_object() || !e.contains("kind") || !e["kind"].is_string() || !e.contains("name") || !e["name"].is_string()) continue;
      const auto kind = parse_kind(e["kind"].get<std::string>());
      if (!kind) continue;
      const auto name = e["name"].get<std::string>();
      if (!client_may_send(product, *kind, name)) continue;
      // a page names a feature with one of its own ids ("panel.pair_open", "web.app_added"): an id of another shape is refused, so a name cannot ride in as a feature
      if (name == "feature_used" && e.contains("props") && e["props"].is_object() && e["props"].contains("feature") && e["props"]["feature"].is_string()) {
        const auto id = e["props"]["feature"].get<std::string>();
        const std::string prefix = std::string { product } + ".";
        if (id.rfind(prefix, 0) != 0) continue;
      }
      auto props = e.contains("props") ? e["props"] : json::object();
      // both switches off: there is no install id (it is neither recovered nor created for this). The report gets a one-time random id of its own, used for its log and its batch, and the Worker takes it as a report without an install.
      // the identity of the report is fixed here, once: the install id when there is one, else a one-time id. The log is uploaded under it and the batch is written with it, whatever the switches say a moment later.
      std::string report_id, report_install;
      if (name == "user_report") {
        report_install = install_id();
        if (report_install.empty()) report_id = new_uuid_v4();
      }
      // a feedback report goes with the anonymised log of this session: uploaded first, its reference travels in the report. A page cannot name a reference itself.
      if (name == "user_report" && props.is_object()) {
        props.erase("log_ref");
        props.erase("shot_ref");  // like the log's: the page cannot name a reference, only the host gets one by uploading
        props.erase("shot_state");
        // the screenshot rides in the report as base64 (the page made it fit: at most 700 KB); it is taken out here, whatever happens to it, so the picture is never queued, shaped or sent as an event prop
        const auto shot_text = props.contains("shot") && props["shot"].is_string() ? props["shot"].get<std::string>() : std::string {};
        props.erase("shot");
        if (!shot_text.empty()) {
          const auto state = attach_screenshot(deps_, endpoint(), report_id.empty() ? report_install : report_id, safe_version(version), shot_text, props);
          if (report) report->shot = state;
        }
        // always: the run log is part of a feedback report, and pressing 发送 is the consent for it (whatever the error-report switch says); only with the version the report will carry
        if (deps_.session_log) {
          try {
            const auto safe = safe_version(version);
            if (!safe.empty()) {
              if (const auto ref = deps_.session_log(endpoint(), report_id.empty() ? report_install : report_id, safe)) props["log_ref"] = *ref;
              else if (deps_.session_log_state) props["log_state"] = deps_.session_log_state();  // the report goes without its log: the word says why
            }
          }
          catch (...) {
          }
        }
      }
      if (name == "user_report") {
        // a feedback report is on disk before the page is told it was accepted (the page shows success from this answer): it is written to the queue here, on this thread, and not left to the buffer that the sender
        // thread swaps out. When the write fails the event stays in the buffer for the next flush and the page is told it was not kept.
        auto shaped = shape_event(*kind, name, props, deps_.now_ms());
        if (!shaped) continue;
        shaped->product = std::string { product };
        shaped->version = sanitize_version(version);
        shaped->report_id = report_id;
        shaped->report_install = report_install;
        if (persist_report(*shaped)) {
          {
            std::lock_guard lock { mutex_ };
            ++counts_.emitted;
            ++kept;
          }
          // the count of feedback reports that survives the daily limit of issues of the Worker (a report is only an issue until the limit): one small event, the category in the Worker's words
          emit(kind_e::error, "feedback_submit", feedback_submit_props(props));
        }
        else {
          std::lock_guard lock { mutex_ };
          ++counts_.write_failed;
          ring_.push_back(std::move(*shaped));
          while (ring_.size() > options_.ring_max) {
            ring_.pop_front();
            ++counts_.dropped;
          }
        }
        continue;
      }
      if (emit(*kind, name, props, product, version, -1, report_id)) ++kept;
    }
    return kept;
  }

  // ---- streams ----

  void
  service_t::stream_begin(const stream_dims_t &dims, std::uint32_t session_id, std::string_view client_key) {
    try {
      const auto now = deps_.now_ms();
      auto with_driver = dims;
      {
        std::lock_guard lock { mutex_ };
        with_driver.gpu_driver = gpu_driver_;
      }
      auto stats = std::make_shared<stream_stats_t>(session_id, std::move(with_driver), now);
      stats->set_detail(stream_detail(session_id, dims));
      {
        std::unique_lock lock { streams_mutex_ };
        const auto key = std::string { client_key };
        const auto found = last_end_by_client_.find(key);
        if (!key.empty() && found != last_end_by_client_.end() && now - found->second <= options_.reconnect_window_ms) stats->set_reconnects(1);
        client_of_[session_id] = key;
        streams_[session_id] = std::move(stats);
      }
      crash::context::stream_started(dims.codec, dims.width, dims.height, dims.fps, dims.hdr, dims.transport, dims.client_kind);  // for the crash report: what the host was doing
      emit(kind_e::usage, "stream_start", dims_to_props(dims));
    }
    catch (...) {
    }
  }

  remote_config_t
  parse_remote_config(const json &cfg, std::int64_t now_ms, int default_logs_per_day) {
    remote_config_t out;
    if (!cfg.is_object()) return out;
    const auto num = [&](const char *k, double d) { return cfg.contains(k) && cfg[k].is_number() ? cfg[k].get<double>() : d; };
    out.rev = static_cast<std::int64_t>(num("rev", 0));
    out.tier1_rate = std::clamp(num("tier1_rate", 0.1), 0.0, 1.0);
    out.logs_per_day = static_cast<int>(std::clamp(num("logs_per_day", default_logs_per_day), 0.0, 10.0));
    out.health = !cfg.contains("health") || !cfg["health"].is_boolean() || cfg["health"].get<bool>();
    out.backoff_s = static_cast<std::int64_t>(std::clamp(num("backoff_s", 0), 0.0, 21600.0));
    out.paused = cfg.contains("paused") && cfg["paused"].is_boolean() && cfg["paused"].get<bool>();
    if (cfg.contains("debug") && cfg["debug"].is_array()) {
      for (const auto &c : cfg["debug"]) {
        if (!c.is_object() || !c.contains("until") || !c["until"].is_number() || c["until"].get<double>() <= static_cast<double>(now_ms)) continue;
        debug_cohort_t d;
        d.until = static_cast<std::int64_t>(c["until"].get<double>());
        const auto text = [&](const char *k) { return c.contains(k) && c[k].is_string() ? c[k].get<std::string>().substr(0, 64) : std::string {}; };
        d.version = text("version");
        d.gpu = text("gpu");
        d.client = text("client");
        d.logs_per_day = c.contains("logs_per_day") && c["logs_per_day"].is_number() ? static_cast<int>(std::clamp(c["logs_per_day"].get<double>(), 0.0, 10.0)) : 0;
        out.debug.push_back(std::move(d));
        if (out.debug.size() >= 8) break;
      }
    }
    return out;
  }

  remote_config_t
  service_t::remote_config() const {
    std::lock_guard lock { mutex_ };
    return config_;
  }

  void
  service_t::apply_remote_config(const std::string &body) {
    try {
      const auto doc = json::parse(body, nullptr, false);
      if (doc.is_discarded() || !doc.is_object() || !doc.contains("cfg")) return;
      auto cfg = parse_remote_config(doc["cfg"], deps_.now_ms(), options_.anomaly_logs_per_day);
      {
        std::lock_guard lock { mutex_ };
        config_ = cfg;
      }
      // a wait the server asked for does not stop priority 0 (see pump); a failed request still backs everything off.
      // The latest answer is the server's word: a wait it no longer asks for ends. The deadline is kept on disk, so a restart does not skip a pause.
      const auto asked = cfg.paused ? std::max<std::int64_t>(cfg.backoff_s, 6 * 3600) : cfg.backoff_s;
      const std::int64_t deadline = asked > 0 ? deps_.now_ms() + asked * 1000 : 0;
      backoff_until_ms_.store(deadline, std::memory_order_relaxed);
      wait_period_ms_ = asked * 1000;  // the answer to a priority 0 request renews the wait, it must not make the next one leave at once
      std::ofstream(options_.dir / "remote_wait", std::ios::trunc) << deadline << '\n';
      std::error_code ec;
      fs::create_directories(options_.dir, ec);
      std::ofstream(options_.dir / "remote_config.json", std::ios::trunc) << doc["cfg"].dump();
    }
    catch (...) {
    }
  }

  bool
  service_t::stream_detail(std::uint32_t session_id, const stream_dims_t &dims) {
    remote_config_t cfg;
    std::string gpu;
    std::string version;
    {
      std::lock_guard lock { mutex_ };
      cfg = config_;
      gpu = context_.gpu_vendor;
      version = context_.version;
    }
    const auto now = deps_.now_ms();
    // a Tier 1 rate of 0 is the server's switch for all histograms (it is what the quota guard sends at 90 % of a free limit): a debug cohort raises the detail of its streams, it does not override that switch
    if (cfg.tier1_rate <= 0) return false;
    for (const auto &c : cfg.debug) {
      if (c.until <= now) continue;
      if ((!c.version.empty() && c.version != version) || (!c.gpu.empty() && c.gpu != gpu) || (!c.client.empty() && c.client != dims.client_kind)) continue;
      return true;  // Tier 3: a debug cohort asked for the detail of this stream
    }
    if (cfg.tier1_rate >= 1.0) return true;
    return deps_.random01() < cfg.tier1_rate;
  }

  void
  service_t::record_frame_done(std::uint32_t session_id, std::int64_t steady_ns) noexcept {
    try {
      std::shared_lock lock { streams_mutex_ };
      const auto found = streams_.find(session_id);
      if (found != streams_.end()) found->second->health().frame_done(steady_ns);
    }
    catch (...) {
    }
  }

  void
  service_t::crumb(crumb_e kind, std::string_view id) {
    try {
      std::lock_guard lock { errors_mutex_ };
      // what the person did is only kept for an error report, so it is not kept while error reports are off
      if (!consent_now().errors) return;
      crumbs_.add(kind, id, deps_.now_ms());
    }
    catch (...) {
    }
  }

  void
  service_t::report_error(const error_input_t &error) {
    try {
      const auto now = deps_.now_ms();
      std::optional<json> props;
      {
        std::lock_guard lock { errors_mutex_ };
        // before anything is counted: the hourly quota and the held counts of the reporter belong to reports that may be sent
        if (!consent_now().errors) return;
        props = reporter_.report(error, crumbs_.snapshot(now), now);
      }
      if (props) emit(kind_e::error, "error_report", *props);
    }
    catch (...) {
    }
  }

  void
  service_t::record_sample(std::uint32_t session_id, hooks::metric_e metric, double ms) noexcept {
    try {
      std::shared_lock lock { streams_mutex_ };
      const auto found = streams_.find(session_id);
      if (found != streams_.end()) found->second->health().record(metric, ms);
    }
    catch (...) {
    }
  }

  void
  service_t::record_count(std::uint32_t session_id, hooks::counter_e counter, std::uint64_t n) noexcept {
    try {
      std::shared_lock lock { streams_mutex_ };
      const auto found = streams_.find(session_id);
      if (found != streams_.end()) found->second->health().count(counter, n);
    }
    catch (...) {
    }
  }

  void
  service_t::set_gpu_driver(std::string driver) {
    // digits and dots only: a driver version is that, and nothing else may leave through this dimension
    std::string clean;
    for (const char c : driver)
      if ((c >= '0' && c <= '9') || c == '.') clean.push_back(c);
    if (clean.size() > 24) clean.resize(24);
    std::lock_guard lock { mutex_ };
    gpu_driver_ = std::move(clean);
  }

  void
  service_t::emit_health_chunk(stream_stats_t &stats, std::int64_t now) {
    if (stats.health().chunk_empty()) return;
    if (!remote_config().health) {
      stats.health().take_chunk(now, false);  // switched off remotely: the chunk is dropped, not sent
      return;
    }
    const bool detail = stats.detail() && !backing_off() && remote_config().tier1_rate > 0;
    auto props = stats.health().take_chunk(now, detail);
    if (props.value("windows", 0) == 0) return;  // only idle windows: nothing to say about a stream that sent no frames
    emit(kind_e::experience, "stream_health", props);
  }

  void
  service_t::on_window(stream_stats_t &stats, const health::window_t &window, std::int64_t now) {
    for (const auto &a : window.anomalies) {
      json props {
        { "code", a.code },
        { "severe", a.severe },
        { "value", a.value },
        { "threshold", a.threshold },
        { "count", a.count },
        { "suppressed", a.suppressed },
        { "window_s", static_cast<std::int64_t>(std::llround(window.seconds)) },
      };
      const auto &d = stats.health().dims();
      props["codec"] = d.codec;
      props["width"] = d.width;
      props["height"] = d.height;
      props["fps"] = d.fps;
      props["hdr"] = d.hdr;
      props["transport"] = d.transport;
      props["client_kind"] = d.client_kind;
      props["client_os"] = d.client_os;
      if (window.probe.target_kbps > 0) props["bitrate_band"] = health::bitrate_band(window.probe.target_kbps);
      if (const auto enc = stats.health().encoder(); !enc.empty()) props["encoder"] = enc;
      if (!d.capture_backend.empty()) props["capture_backend"] = d.capture_backend;
      if (!d.gpu_driver.empty()) props["gpu_driver"] = d.gpu_driver;
      // a severe anomaly sends an anonymized excerpt of the host log around it, at most a few times per install and day
      bool log = false;
      if (a.severe && consent_now().errors) {
        std::lock_guard logs { logs_mutex_ };
        if (anomaly_log_allowed(now)) {
          // two excerpts wait at most: when they do, the ones waiting are kept and the new one takes no room
          auto text = pending_logs_.size() < 2 ? log_excerpt(now) : std::string {};
          if (!text.empty()) {
            anomaly_log_commit(now);
            pending_logs_.push_back({ a.code, now, session_id_, std::move(text), 0 });
            log = true;
          }
        }
      }
      props["log"] = log;
      emit(kind_e::experience, "stream_anomaly", props);
    }
    // the install's own baseline: one small event when its statistics got clearly worse than they used to be
    if (!window.idle && remote_config().health) {
      std::vector<baseline_event_t> found;
      {
        std::lock_guard logs { logs_mutex_ };
        if (!baseline_) baseline_ = std::make_unique<self_baseline_t>(options_.dir / "stream_baseline.json", context().version);
        found = baseline_->observe(window);
      }
      for (const auto &b : found) {
        const auto &d = stats.health().dims();
        json props {
          { "metric", b.metric },
          { "baseline", b.baseline },
          { "current", b.current },
          { "delta", b.delta },
          { "windows", b.windows },
          { "baseline_windows", b.baseline_windows },
          { "version_changed", b.version_changed },
          { "codec", d.codec },
          { "width", d.width },
          { "height", d.height },
          { "fps", d.fps },
          { "hdr", d.hdr },
          { "transport", d.transport },
          { "client_kind", d.client_kind },
          { "client_os", d.client_os },
        };
        if (window.probe.target_kbps > 0) props["bitrate_band"] = health::bitrate_band(window.probe.target_kbps);
        if (const auto enc = stats.health().encoder(); !enc.empty()) props["encoder"] = enc;
        if (!d.capture_backend.empty()) props["capture_backend"] = d.capture_backend;
        if (!d.gpu_driver.empty()) props["gpu_driver"] = d.gpu_driver;
        emit(kind_e::experience, "stream_regress", props);
      }
    }
  }

  bool
  service_t::anomaly_log_allowed(std::int64_t now) {
    const auto day = now / 86'400'000;
    if (anomaly_log_day_ != day) {
      anomaly_log_day_ = day;
      anomaly_log_count_ = 0;
      // the count of today survives a restart: it is kept in the telemetry directory
      std::error_code ec;
      std::ifstream in(options_.dir / "anomaly_log_quota");
      std::int64_t saved_day = -1;
      int saved_count = 0;
      if (in >> saved_day >> saved_count && saved_day == day) anomaly_log_count_ = saved_count;
    }
    int limit;
    {
      std::lock_guard lock { mutex_ };
      limit = config_.logs_per_day;  // the server's number (the local option is its default)
      for (const auto &c : config_.debug)
        if (c.until > now && c.logs_per_day > limit && (c.version.empty() || c.version == context_.version) && (c.gpu.empty() || c.gpu == context_.gpu_vendor)) limit = c.logs_per_day;
    }
    if (anomaly_log_count_ >= limit) return false;
    // the first serious problem of a run is never held back by the gap: a run that goes wrong early must leave its log
    if (!anomaly_log_first_ && last_anomaly_log_ms_ >= 0 && now - last_anomaly_log_ms_ < options_.anomaly_log_min_gap_ms) return false;
    return true;
  }

  void
  service_t::anomaly_log_commit(std::int64_t now) {
    anomaly_log_first_ = false;
    ++anomaly_log_count_;
    last_anomaly_log_ms_ = now;
    std::error_code ec;
    fs::create_directories(options_.dir, ec);
    std::ofstream(options_.dir / "anomaly_log_quota", std::ios::trunc) << now / 86'400'000 << ' ' << anomaly_log_count_ << '\n';
  }

  void
  service_t::note_log_line(int severity, const char *message, std::size_t length) noexcept {
    try {
      const auto now = deps_.now_ms();
      std::lock_guard lock { ring_mutex_ };
      log_ring_.push_back({ now, severity, std::string { message ? message : "", std::min<std::size_t>(length, 400) } });
      while (log_ring_.size() > 2500 || (!log_ring_.empty() && now - log_ring_.front().ms > options_.anomaly_log_span_ms + 30'000)) log_ring_.pop_front();
    }
    catch (...) {
    }
  }

  std::string
  service_t::log_excerpt(std::int64_t now) {
    std::vector<ring_line_t> lines;
    {
      std::lock_guard lock { ring_mutex_ };
      for (const auto &l : log_ring_)
        if (now - l.ms <= options_.anomaly_log_span_ms) lines.push_back(l);
    }
    // the same scrubber as the rest of the telemetry; the clock is relative to the anomaly, so no wall-clock time or date leaves with it
    static const char level[] = { 'T', 'D', 'I', 'W', 'E', 'F' };
    const auto redact = [this](std::string text) {
      for (const auto &word : options_.redact_words) {
        if (word.size() < 2) continue;
        std::string lower_text = text, lower_word = word;
        std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(), [](unsigned char c) { return std::tolower(c); });
        std::transform(lower_word.begin(), lower_word.end(), lower_word.begin(), [](unsigned char c) { return std::tolower(c); });
        for (std::size_t at = 0; (at = lower_text.find(lower_word, at)) != std::string::npos;) {
          text.replace(at, word.size(), "<name>");
          lower_text.replace(at, word.size(), "<name>");
          at += 6;
        }
      }
      return text;
    };
    std::vector<std::string> out;
    out.reserve(lines.size());
    std::size_t total = 0;
    for (const auto &l : lines) {
      char head[32];
      std::snprintf(head, sizeof head, "[-%6.1fs] %c ", static_cast<double>(now - l.ms) / 1000.0, level[std::clamp(l.severity, 0, 5)]);
      auto line = std::string { head } + redact(scrub_log_message(l.text, 400)) + "\n";
      total += line.size();
      out.push_back(std::move(line));
    }
    // keep the newest lines when the excerpt is longer than the limit
    std::size_t first = 0;
    while (total > options_.anomaly_log_max_bytes && first < out.size()) total -= out[first++].size();
    std::string text;
    text.reserve(total);
    for (std::size_t i = first; i < out.size(); ++i) text += out[i];
    return text;
  }

  int
  service_t::pump_logs() {
    int sent = 0;
    try {
      std::size_t n;
      {
        std::lock_guard logs { logs_mutex_ };
        n = pending_logs_.size();
      }
      for (std::size_t i = 0; i < n; ++i) {
        pending_log_t item;
        {
          std::lock_guard logs { logs_mutex_ };
          if (pending_logs_.empty()) break;
          if (!consent_now().errors) {
            pending_logs_.clear();
            break;
          }
          item = std::move(pending_logs_.front());
          pending_logs_.pop_front();
        }
        const auto ctx = context();
        const auto id = install_id();
        if (id.empty()) continue;
        json body {
          { "v", 1 },
          { "install_id", id },
          { "session_id", item.session },
          { "product", "host" },
          { "version", safe_version(ctx.version) },
          { "channel", ctx.channel },
          { "os", ctx.os },
          { "code", item.code },
          { "t", item.t },
          { "log", item.text },
        };
        const auto result = deps_.post(options_.endpoint + "/v1/anomaly-log", body.dump());
        const bool retry = result.status == 0 || result.status == 408 || result.status == 425 || result.status == 429 || result.status >= 500;
        if (result.status >= 200 && result.status < 300) {
          ++sent;
        }
        else if (retry && ++item.tries < 2) {
          std::lock_guard logs { logs_mutex_ };
          pending_logs_.push_back(std::move(item));
        }
      }
    }
    catch (...) {
    }
    return sent;
  }

  void
  service_t::quality_event(const stream_stats_t &stats, std::int64_t now) {
    emit(kind_e::experience, "stream_quality", stats.quality_props(now));
  }

  void
  service_t::stream_end(std::uint32_t session_id, std::string_view stop_reason, std::string_view clip_offer, std::string_view clip_flow) {
    try {
      std::shared_ptr<stream_stats_t> stats;
      const auto now = deps_.now_ms();
      {
        std::unique_lock lock { streams_mutex_ };
        const auto found = streams_.find(session_id);
        if (found == streams_.end()) return;
        stats = std::move(found->second);
        streams_.erase(found);
        if (streams_.empty()) crash::context::stream_ended();
        const auto client = client_of_.find(session_id);
        if (client != client_of_.end()) {
          if (!client->second.empty()) {
            last_end_by_client_[client->second] = now;
            if (last_end_by_client_.size() > 64) last_end_by_client_.erase(last_end_by_client_.begin());
          }
          client_of_.erase(client);
        }
      }
      if (stats->encoder().empty() && encoder_resolver_) stats->set_encoder(encoder_resolver_());
      if (stats->take_first_frame_report() && stats->first_frame_ms()) {
        emit(kind_e::experience, "startup", json { { "first_frame_ms", *stats->first_frame_ms() } });
        stream_stage("first_frame", *stats->first_frame_ms());
      }
      if (const auto w = stats->health().finish(now, health::probe_t {})) on_window(*stats, *w, now);
      emit_health_chunk(*stats, now);
      auto props = dims_to_props(stats->dims());
      props["duration_s"] = std::max<std::int64_t>(0, (now - stats->start_ms()) / 1000);
      props["result"] = result_of_stop_reason(stop_reason);
      if (!clip_offer.empty()) props["clip_offer"] = std::string { clip_offer };
      if (!clip_flow.empty()) props["clip_flow"] = std::string { clip_flow };
      emit(kind_e::usage, "stream_end", props);
      quality_event(*stats, now);
    }
    catch (...) {
    }
  }

  void
  service_t::record_encoded_frame(std::uint32_t session_id) noexcept {
    try {
      std::shared_lock lock { streams_mutex_ };
      const auto found = streams_.find(session_id);
      if (found != streams_.end()) found->second->on_encoded_frame(deps_.now_ms());
    }
    catch (...) {
    }
  }

  void
  service_t::record_encode_ms(std::uint32_t session_id, double ms) noexcept {
    try {
      std::shared_lock lock { streams_mutex_ };
      const auto found = streams_.find(session_id);
      if (found != streams_.end()) {
        found->second->on_encode_ms(ms);
        found->second->health().record(hooks::metric_e::encode_ms, ms);
      }
    }
    catch (...) {
    }
  }

  void
  service_t::record_capture_ms(std::uint32_t session_id, double ms) noexcept {
    try {
      std::shared_lock lock { streams_mutex_ };
      const auto found = streams_.find(session_id);
      if (found != streams_.end()) {
        found->second->on_capture_ms(ms);
        found->second->health().record(hooks::metric_e::capture_ms, ms);
      }
    }
    catch (...) {
    }
  }

  bool
  service_t::streams_active() const {
    std::shared_lock lock { streams_mutex_ };
    return !streams_.empty();
  }

  void
  service_t::tick_streams(const std::vector<session_probe_t> &probes) {
    try {
      const auto now = deps_.now_ms();
      std::vector<std::shared_ptr<stream_stats_t>> active;
      {
        std::shared_lock lock { streams_mutex_ };
        for (const auto &[id, stats] : streams_) active.push_back(stats);
      }
      for (const auto &stats : active) {
        session_probe_t probe;
        for (const auto &p : probes)
          if (p.session_id == stats->session_id()) probe = p;
        stats->tick(now, probe);
        {
          health::probe_t hp;
          hp.rtt_ms = probe.rtt_ms;
          hp.loss_pct = probe.loss_pct;
          hp.measured_kbps = probe.measured_kbps;
          hp.target_kbps = probe.target_kbps;
          hp.fec_pct = probe.fec_pct;
          if (const auto w = stats->health().tick(now, hp)) on_window(*stats, *w, now);
          if (stats->health().chunk_due(now)) emit_health_chunk(*stats, now);
        }
        if (stats->encoder().empty() && encoder_resolver_) stats->set_encoder(encoder_resolver_());
        if (stats->take_first_frame_report()) {
          if (const auto first = stats->first_frame_ms()) {
            emit(kind_e::experience, "startup", json { { "first_frame_ms", *first } });
            stream_stage("first_frame", *first);
          }
        }
      }
    }
    catch (...) {
    }
  }

  // ---- where a stream start was slow or failed, and driver failures ----

  void
  service_t::stream_stage(std::string_view stage, std::int64_t ms) {
    try {
      if (!stage_is_slow(stage, ms)) return;
      if (stage_events_.fetch_add(1, std::memory_order_relaxed) >= MAX_STAGE_EVENTS_PER_RUN) return;
      emit(kind_e::experience, "stream_stage", json { { "stage", std::string { stage } }, { "ms", ms } });
    }
    catch (...) {
    }
  }

  void
  service_t::stream_failed(std::string_view stage, std::string_view code) {
    try {
      if (stage_events_.fetch_add(1, std::memory_order_relaxed) >= MAX_STAGE_EVENTS_PER_RUN) return;
      emit(kind_e::experience, "stream_failed", json { { "stage", std::string { stage } }, { "code", std::string { code } } });
    }
    catch (...) {
    }
  }

  void
  service_t::driver_failed(std::string_view driver, std::string_view op, std::string_view code) {
    try {
      if (driver_events_.fetch_add(1, std::memory_order_relaxed) >= MAX_DRIVER_EVENTS_PER_RUN) return;
      json props { { "driver", std::string { driver } }, { "op", std::string { op } }, { "result", "failed" } };
      // the code is a result code (letters, digits and _ . : + -), never text: a value with anything else in it is not a code and is left out whole (cutting the other characters out would keep the rest of a path or a name)
      const bool is_code = !code.empty() && code.size() <= 64 && std::all_of(code.begin(), code.end(), [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '.' || c == ':' || c == '+' || c == '-';
      });
      if (is_code) props["hresult"] = std::string { code };
      emit(kind_e::error, "driver_event", props);
    }
    catch (...) {
    }
  }

  // ---- the run marker: was the previous run of the host ended by itself ----

  void
  service_t::begin_run_marker() {
    try {
      const auto path = options_.dir / "run_marker.json";
      std::error_code ec;
      std::ifstream in(path);
      const auto previous = in ? json::parse(in, nullptr, false) : json();
      in.close();
      const auto boot = deps_.boot_ms ? deps_.boot_ms() : 0;
      if (previous.is_object() && previous.value("status", "") == "running" && previous.contains("session_id") && previous["session_id"].is_string()) {
        // the previous run left no sign that it ended by itself. Another boot since then: the computer was shut down or restarted, which is not the program's doing.
        bool rebooted = false;
        if (deps_.boot_id) {
          // the boot word is what tells the boots apart; a start time worked out from the wall clock moves with every clock that is set, so it is never the proof: without both words the answer is "unknown"
          const auto now_word = deps_.boot_id();
          const auto before_word = previous.value("boot_id", std::string {});
          rebooted = !now_word.empty() && !before_word.empty() && now_word != before_word;
        }
        else {
          const auto before = previous.value("boot_ms", std::int64_t { 0 });
          rebooted = boot > 0 && before > 0 && std::llabs(boot - before) > 120'000;
        }
        emit(kind_e::error, "session_abnormal", json { { "abnormal_mechanism", rebooted ? "shutdown" : "unknown" } }, "host", {}, -1, {}, previous["session_id"].get<std::string>());
      }
      write_run_marker("running", boot);
    }
    catch (...) {
    }
  }

  void
  service_t::end_run_marker() {
    write_run_marker("exited", deps_.boot_ms ? deps_.boot_ms() : 0);
  }

  void
  service_t::write_run_marker(std::string_view status, std::int64_t boot) {
    try {
      std::error_code ec;
      fs::create_directories(options_.dir, ec);
      const auto path = options_.dir / "run_marker.json";
      const auto tmp = options_.dir / "run_marker.json.tmp";
      {
        std::ofstream out(tmp, std::ios::trunc);
        std::string boot_word;
        if (deps_.boot_id) boot_word = deps_.boot_id();
        out << json { { "session_id", session_id_ }, { "status", std::string { status } }, { "boot_ms", boot }, { "boot_id", boot_word }, { "t", deps_.now_ms() } }.dump();
      }
      fs::rename(tmp, path, ec);
    }
    catch (...) {
    }
  }

  // ---- the log ----

  void
  service_t::note_log(int severity, const char *message, std::size_t length, std::string_view component, std::string_view site) noexcept {
    if (severity < 3) return;
    try {
      const auto hour = deps_.now_ms() / 3'600'000;
      std::lock_guard lock { log_mutex_ };
      auto &window = log_windows_[std::string { component }];
      if (window.hour != hour && window.hour >= 0 && (window.fatal | window.error | window.warning) == 0) window.hour = hour;
      if (window.hour < 0) window.hour = hour;
      if (severity >= 5) ++window.fatal;
      else if (severity == 4) ++window.error;
      else ++window.warning;
      if (severity < 4 || !message) return;
      const auto raw = std::string { message, std::min<std::size_t>(length, 400) };
      auto key = std::string { component } + '\n' + raw.substr(0, 60);
      auto found = raw_errors_.find(key);
      if (found == raw_errors_.end()) {
        if (raw_errors_.size() >= MAX_RAW_ERRORS) {
          // the table is full: further distinct messages of an hour are only counted in the tally above
          return;
        }
        found = raw_errors_.emplace(key, raw_error_t { std::string { component }, raw, severity >= 5 ? 5 : 4, 0, std::string { site.substr(0, 200) } }).first;
      }
      ++found->second.count;
      if (severity >= 5) found->second.severity = 5;
    }
    catch (...) {
    }
  }

  void
  service_t::collect_logs(std::int64_t now, bool close_hour) {
    std::map<std::string, log_window_t> windows;
    std::map<std::string, raw_error_t> raws;
    const auto hour = now / 3'600'000;
    {
      std::lock_guard lock { log_mutex_ };
      for (auto it = log_windows_.begin(); it != log_windows_.end();) {
        auto &w = it->second;
        const bool over = w.hour >= 0 && w.hour != hour;
        if ((over || close_hour) && (w.fatal | w.error | w.warning)) {
          windows[it->first] = w;
          w = log_window_t { hour, 0, 0, 0 };
        }
        ++it;
      }
      raws.swap(raw_errors_);
    }
    for (const auto &[component, w] : windows) {
      emit(kind_e::error, "log_summary", json { { "component", catalogue_id(component, 32).empty() ? "host" : catalogue_id(component, 32) }, { "fatal", w.fatal }, { "error", w.error }, { "warning", w.warning } });
    }
    const auto build = build_token(context().version);
    for (const auto &[key, raw] : raws) {
      // The line itself is not sent. Its code is a hash of the fixed words it starts with (src/telemetry/error_code.h): the same error is one code and what a person chose is not in it.
      const auto code = error_code_of(scrub_log_message(raw.raw, MAX_MESSAGE_CHARS));
      emit(kind_e::error, "error", json { { "component", catalogue_id(raw.component, 32).empty() ? "host" : catalogue_id(raw.component, 32) }, { "code", code }, { "severity", raw.severity >= 5 ? "fatal" : "error" }, { "count", raw.count }, { "build", build } });
      // Every error and fatal log line has a site, so every code can be told where it comes from: the first line of a code in the hour becomes an error report with that site (the group of the
      // Worker then has a place in the source, which the count alone never had). No text of the line goes with it. The reporter keeps its own hourly and per-install limits.
      if (!raw.site.empty()) {
        error_input_t input;
        input.component = catalogue_id(raw.component, 32).empty() ? "host" : catalogue_id(raw.component, 32);
        input.code = code;
        input.severity = raw.severity >= 5 ? "fatal" : "error";
        input.loc = clean_frame(raw.site);
        if (!input.loc.empty()) input.frames.push_back(input.loc);
        report_error(input);
      }
    }
  }

  // ---- sender ----

  bool
  service_t::flush_wanted() const {
    std::lock_guard lock { mutex_ };
    return ring_.size() >= options_.flush_events;
  }

  void
  service_t::flush(bool final) {
    try {
      const auto now = deps_.now_ms();
      collect_logs(now, final);
      // errors held back by the rate limit leave as one aggregate per fingerprint
      std::vector<json> held;
      {
        std::lock_guard errors_lock { errors_mutex_ };
        if (consent_now().errors) held = reporter_.flush(final ? now + error_reporter_t::AGGREGATE_MS : now);
      }
      for (const auto &props : held) emit(kind_e::error, "error_report", props, "host", {}, 1);  // the counts of known errors: priority 1
      std::deque<event_t> taken;
      consent_marks_t marks_at_take;
      {
        // the take and the put-back below are the two sections a consent change must not slip between (consent_changed holds the same mutex for its clear): the writes in between run without it
        std::lock_guard flush_lock { flush_mutex_ };
        std::lock_guard lock { mutex_ };
        taken.swap(ring_);
        if (options_.aggregate_interval_ms > 0) {
          // the high-volume families wait here and leave merged once per interval and at exit; everything else goes on as before
          for (auto it = taken.begin(); it != taken.end();) {
            if (high_volume_event(it->name) && it->report_id.empty() && it->report_install.empty()) {
              aggregate_ring_.push_back(std::move(*it));
              it = taken.erase(it);
            }
            else
              ++it;
          }
          while (aggregate_ring_.size() > options_.ring_max * 4) {
            aggregate_ring_.pop_front();
            ++counts_.dropped;
          }
          if (final || now - last_aggregate_flush_ms_ >= options_.aggregate_interval_ms) {
            for (auto &e : aggregate_ring_) taken.push_back(std::move(e));
            aggregate_ring_.clear();
            last_aggregate_flush_ms_ = now;
          }
        }
        marks_at_take = consent_marks_;
        last_flush_ms_ = now;
      }
      if (taken.empty()) return;
      const auto c = consent_now();
      // a batch holds events of one product and one priority, so the queue can drop, hold and send by priority
      // (a record about an earlier run, session_abnormal, has the session id of that run and goes out in an envelope of its own)
      std::map<std::tuple<std::string, int, std::string>, std::vector<event_t>> by_product;
      std::vector<event_t> alone;  // reports with an id of their own (both switches off): one batch each, with no install id and no session of the host
      for (auto &e : taken) {
        if (!c.allows(e.kind) && !is_user_report(e.kind, e.name)) continue;
        // a feedback report that reached the buffer without an identity (emitted directly, kept through a change of the switches): it gets one now, from the install id when there is one and a one-time id when there is not,
        // and goes out on its own either way: it is never skipped for want of an install id
        if (is_user_report(e.kind, e.name) && e.report_id.empty() && e.report_install.empty()) {
          e.report_install = install_id();
          if (e.report_install.empty()) e.report_id = new_uuid_v4();
        }
        if (!e.report_id.empty() || !e.report_install.empty()) {
          alone.push_back(std::move(e));
          continue;
        }
        const auto priority = event_priority(e);
        by_product[{ e.product, priority, e.session_id }].push_back(std::move(e));
      }
      std::vector<event_t> failed;  // what could not be written: it goes back to the buffer, never lost to a failed write
      for (const auto &e : alone) {
        if (!persist_report(e)) {
          failed.push_back(e);
          continue;
        }
        // a report whose first write failed in ingest() is written here, and is counted here (ingest counted only the writes that went through): no lock is held
        if (is_user_report(e.kind, e.name)) emit(kind_e::error, "feedback_submit", feedback_submit_props(e.props));
      }
      if (!by_product.empty()) {
        auto ctx = context();
        ctx.install_id = install_id();
        ctx.session_id = session_id_;
        for (auto &[key, events] : by_product) {
          auto group_ctx = ctx;
          if (!std::get<2>(key).empty()) group_ctx.session_id = std::get<2>(key);
          // no install id (both switches went off since): nothing to write for these, they are the automatic events of a choice that was turned off
          if (ctx.install_id.empty()) continue;
          std::vector<std::vector<std::size_t>> origin;
          std::vector<event_t> merged;
          if (options_.aggregate_interval_ms > 0) merged = aggregate_events(events, origin);
          const auto &outgoing = options_.aggregate_interval_ms > 0 ? merged : events;
          std::vector<bool> written(events.size(), false);  // per event: only what a pushed envelope carried is on disk, a group is no longer written all or nothing
          bool complete = false;
          try {
            std::vector<std::vector<std::size_t>> members;
            const auto envelopes = build_envelopes(group_ctx, std::get<0>(key), outgoing, members);
            complete = true;
            for (std::size_t at = 0; at < envelopes.size(); ++at) {
              if (deps_.before_queue_write) deps_.before_queue_write();
              bool pushed = false;
              try {
                // check and write are one step against consent_changed(): its clear and its mark move under the same mutex, so an envelope is on disk before the change clears the queue or the
                // mark has moved and the events of the turned-off choice are left out; never written behind the clear (a choice turned on again later would send them)
                std::lock_guard flush_lock { flush_mutex_ };
                auto envelope = envelopes[at];
                json kept = json::array();
                for (std::size_t i = 0; i < members[at].size(); ++i) {
                  if (consent_marks_.moved_since(outgoing[members[at][i]].kind, marks_at_take)) continue;
                  kept.push_back(std::move(envelope["events"][i]));
                }
                if (kept.empty()) {
                  pushed = true;  // everything of this envelope belonged to a choice that was turned off: nothing to write
                }
                else {
                  envelope["events"] = std::move(kept);
                  std::lock_guard lock { queue_mutex_ };
                  pushed = queue_.push(envelope, std::get<1>(key));
                }
              }
              catch (...) {
              }
              if (!pushed) {
                complete = false;  // the envelopes after this one are not attempted: the events of this envelope and of those go back
                break;
              }
              for (const auto member : members[at]) {
                if (origin.empty()) written[member] = true;
                else
                  for (const auto from : origin[member]) written[from] = true;
              }
            }
          }
          catch (const std::exception &) {
            complete = false;  // the envelopes could not even be built: nothing of this group reached the queue
          }
          if (complete) continue;
          for (std::size_t at = 0; at < events.size(); ++at)
            if (!written[at]) failed.push_back(events[at]);
        }
      }
      if (!failed.empty()) {
        std::lock_guard flush_lock { flush_mutex_ };
        std::lock_guard lock { mutex_ };
        std::size_t back = 0;
        for (auto it = failed.rbegin(); it != failed.rend(); ++it) {
          // the consent moved while the writes went on: the automatic events of the old consent are not put back, only a report the person sent is (it never waited for a switch)
          if (consent_marks_.moved_since(it->kind, marks_at_take) && !is_user_report(it->kind, it->name)) continue;
          ring_.push_front(*it);
          ++back;
        }
        counts_.write_failed += back;
        // what a failed write puts back may be the whole held interval: it fits the same room it was taken from
        const auto room = options_.aggregate_interval_ms > 0 ? options_.ring_max * 4 : options_.ring_max;
        while (ring_.size() > room) {
          ring_.pop_front();
          ++counts_.dropped;
        }
      }
    }
    catch (...) {
    }
  }

  bool
  service_t::handle_answer(const post_result_t &answer, std::size_t count, const std::vector<fs::path> &paths) {
    const int status = answer.status;
    const auto now = deps_.now_ms();
    if (status >= 200 && status < 300) {
      {
        std::lock_guard lock { queue_mutex_ };
        for (const auto &p : paths) queue_.remove(p);
      }
      failures_ = 0;
      not_before_ms_ = 0;
      if (!answer.body.empty()) apply_remote_config(answer.body);
      std::lock_guard lock { mutex_ };
      ++counts_.sent_batches;
      counts_.sent_events += count;
      return true;
    }
    if (status_retryable(status)) {
      ++failures_;
      // a 429 or 503 that says how long to wait is believed (up to an hour); otherwise the backoff decides
      const auto asked = std::min<std::int64_t>(answer.retry_after_s, options_.max_backoff_ms / 1000) * 1000;
      not_before_ms_ = now + std::max<std::int64_t>(asked, backoff_seconds(failures_, deps_.random01()) * 1000);
      std::lock_guard lock { mutex_ };
      ++counts_.failed_posts;
      return false;
    }
    // refused for good (a 4xx that a retry will not change): the batches are dropped
    {
      std::lock_guard lock { queue_mutex_ };
      for (const auto &p : paths) queue_.remove(p);
    }
    std::lock_guard lock { mutex_ };
    ++counts_.rejected_batches;
    return true;
  }

  namespace {
    // Priority 0 events held back by a wait the server asked for leave once as counts: the same error or problem becomes one event with its count.
    json
    collapse_priority0(std::vector<disk_queue_t::entry_t> &entries) {
      json merged = entries.front().batch;
      json events = json::array();
      std::map<std::string, std::size_t> seen;
      bool health = false;
      for (auto &entry : entries) {
        if (entry.batch.value("product", "") != merged.value("product", "")) continue;
        for (auto &e : entry.batch["events"]) {
          const auto name = e.value("name", "");
          const auto &p = e.contains("props") ? e["props"] : json::object();
          if (name == "stream_health") {
            if (health) continue;  // one summary per wait: the others are counted out
            health = true;
            events.push_back(e);
            continue;
          }
          std::string key = name + "|" + p.value("component", std::string {}) + "|" + p.value("code", std::string {}) + "|" + p.value("fingerprint", std::string {}) + "|" + p.value("metric", std::string {});
          // a driver fault is told apart by driver, operation, result and code: a failed virtual display must not hide a failed microphone start
          if (name == "driver_event") key += "|" + p.value("driver", std::string {}) + "|" + p.value("op", std::string {}) + "|" + p.value("result", std::string {}) + "|" + p.value("hresult", std::string {});
          const auto found = seen.find(key);
          if (found == seen.end()) {
            seen[key] = events.size();
            events.push_back(e);
          }
          else if (auto &kept = events[found->second]; kept.contains("props")) {
            auto &kp = kept["props"];
            if (name == "error_report" || name == "error")
              kp["count"] = kp.value("count", 1) + p.value("count", 1);
            else if (name == "stream_anomaly")
              kp["suppressed"] = kp.value("suppressed", 0) + 1 + p.value("suppressed", 0);
          }
        }
      }
      merged["events"] = std::move(events);
      return merged;
    }
  }  // namespace

  int
  service_t::pump() {
    int sent = 0;
    try {
      if (deps_.now_ms() < not_before_ms_) return 0;
      send_opt_outs();
      const auto c = consent_now();
      // while the server asks for a wait only priority 0 may leave: one request per product and version in the wait, repeated events merged into one with their count; the rest stays in the queue under its caps
      if (backing_off()) {
        std::vector<disk_queue_t::entry_t> entries;
        {
          std::lock_guard lock { queue_mutex_ };
          entries = queue_.entries(0, 64);  // priority 0 is bounded by max_p0_files (40): all of it is looked at
        }
        for (auto &e : entries) filter_by_consent(e.batch, c.usage, c.errors);
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const auto &e) { return e.batch["events"].empty(); }), entries.end());
        // a feedback report is never merged with anything and never counted away: one request for each, as it was written (with its own install or one-time id), and only the file that was answered is removed. It does not wait for
        // the group's turn either: the person pressed 发送 for it, and the rate cap per minute still holds.
        const auto is_report = [](const disk_queue_t::entry_t &e) {
          for (const auto &ev : e.batch["events"])
            if (ev.value("name", "") == "user_report") return true;
          return false;
        };
        if (const auto report = std::find_if(entries.begin(), entries.end(), is_report); report != entries.end()) {
          const auto at = deps_.now_ms();
          while (!post_times_.empty() && at - post_times_.front() >= 60'000) post_times_.pop_front();
          if (static_cast<int>(post_times_.size()) >= options_.max_posts_per_minute) return 0;
          post_times_.push_back(at);
          post_result_t answer;
          try {
            if (deps_.post) answer = deps_.post(events_url(), report->batch.dump());
          }
          catch (...) {
            answer = {};
          }
          if (handle_answer(answer, report->batch["events"].size(), { report->path }) && answer.status >= 200 && answer.status < 300) ++sent;
          return sent;
        }
        // the session id belongs to the group: a record about an earlier run (session_abnormal) goes out under that run's id, never merged into the envelope of this run
        const auto group_of = [](const disk_queue_t::entry_t &e) {
          return e.batch.value("product", std::string {}) + "\n" + e.batch.value("version", std::string {}) + "\n" + e.batch.value("session_id", std::string {});
        };
        const auto now = deps_.now_ms();
        const auto first = std::find_if(entries.begin(), entries.end(), [&](const auto &e) {
          const auto sent = p0_group_sent_at_.find(group_of(e));
          return sent == p0_group_sent_at_.end() || now - sent->second >= std::max<std::int64_t>(wait_period_ms_, 60'000);
        });
        if (first == entries.end()) return 0;
        // the same limit per minute as outside a wait
        while (!post_times_.empty() && now - post_times_.front() >= 60'000) post_times_.pop_front();
        if (static_cast<int>(post_times_.size()) >= options_.max_posts_per_minute) return 0;
        post_times_.push_back(now);
        const auto group = group_of(*first);
        // the merged request must fit the limits of one request: the files of the group are taken in order until the events or the bytes would pass them, the rest waits
        std::vector<disk_queue_t::entry_t> included;
        std::size_t events_total = 0, bytes_total = 0;
        for (auto &e : entries) {
          if (group_of(e) != group) continue;
          const auto events_here = e.batch["events"].size();
          const auto bytes_here = e.batch.dump().size();
          if (!included.empty() && (events_total + events_here > MAX_BATCH_EVENTS || bytes_total + bytes_here > MAX_BATCH_BYTES * 3 / 4)) break;
          events_total += events_here;
          bytes_total += bytes_here;
          included.push_back(std::move(e));
        }
        std::size_t count = 0;
        std::vector<fs::path> paths;
        for (const auto &e : included) paths.push_back(e.path);
        auto merged = collapse_priority0(included);
        count = merged["events"].size();
        post_result_t answer;
        try {
          if (deps_.post) answer = deps_.post(events_url(), merged.dump());
        }
        catch (...) {
          answer = {};
        }
        p0_group_sent_at_[group] = now;  // one request per group in a wait period, whatever the answer: a failure backs off through handle_answer
        if (handle_answer(answer, count, paths)) ++sent;
        return sent;
      }
      for (int i = 0; i < options_.max_batches_per_pump; ++i) {
        std::optional<disk_queue_t::entry_t> entry;
        {
          std::lock_guard lock { queue_mutex_ };
          entry = queue_.front();
        }
        if (!entry) break;
        const auto count = filter_by_consent(entry->batch, c.usage, c.errors);
        if (count == 0) {
          std::lock_guard lock { queue_mutex_ };
          queue_.remove(entry->path);
          continue;
        }
        // pace: the Worker takes 20 requests a minute per install, a backlog after a long offline time must not run into that
        {
          const auto at = deps_.now_ms();
          while (!post_times_.empty() && at - post_times_.front() >= 60'000) post_times_.pop_front();
          if (static_cast<int>(post_times_.size()) >= options_.max_posts_per_minute) break;
          post_times_.push_back(at);
        }
        post_result_t answer;
        try {
          if (deps_.post) answer = deps_.post(events_url(), entry->batch.dump());
        }
        catch (...) {
          answer = {};
        }
        if (!handle_answer(answer, count, { entry->path })) break;
        if (answer.status >= 200 && answer.status < 300) ++sent;
        if (backing_off()) break;  // the answer asked for a wait: the rest goes through the priority 0 path from now on
      }
    }
    catch (...) {
    }
    return sent;
  }

  void
  service_t::consent_changed() {
    try {
      // before the install id goes: the record of a turned-off choice carries it; when it could not be saved the id stays on the computer (the next start finds the turn-off again and saves it)
      const bool recorded = reconcile_consent();
      const auto c = consent_now();
      {
        // the generation moves with the clear: a flush that took its events out before this puts back only a report the person sent, never the automatic events of the consent that just ended
        std::lock_guard flush_lock { flush_mutex_ };
        // a choice found off is marked: the events a running flush holds for it are not written or put back, also when it is turned on again before the flush ends
        if (!c.usage) ++consent_marks_.usage;
        if (!c.errors) ++consent_marks_.errors;
        std::lock_guard lock { mutex_ };
        ring_.erase(std::remove_if(ring_.begin(), ring_.end(), [&](const event_t &e) { return !c.allows(e.kind) && !is_user_report(e.kind, e.name); }), ring_.end());
        aggregate_ring_.erase(std::remove_if(aggregate_ring_.begin(), aggregate_ring_.end(), [&](const event_t &e) { return !c.allows(e.kind); }), aggregate_ring_.end());
      }
      if (!c.errors) {
        {
          std::lock_guard lock { log_mutex_ };
          raw_errors_.clear();
          log_windows_.clear();
        }
        // the actions of the person and the errors held back for an aggregate: gone with the choice, so turning it on again does not send them
        std::lock_guard lock { errors_mutex_ };
        crumbs_.clear();
        reporter_.clear();
      }
      std::lock_guard queue_lock { queue_mutex_ };
      if (!c.any()) {
        // only the automatic events go; a feedback report the person sent and that is still waiting is theirs and stays (its batch keeps the id it was built with)
        queue_.refilter(false, false);
        if (recorded) {
          std::lock_guard lock { mutex_ };
          install_id_.clear();
          std::error_code ec;
          fs::remove(options_.dir / "install_id", ec);
        }
      }
      else {
        queue_.refilter(c.usage, c.errors);
      }
    }
    catch (...) {
    }
  }


  // ---- the record that a choice was turned off (opt_out.h) ----

  namespace {
    /** Writes `text` to `path` through a temporary file and a rename, so a crash leaves the old content or the new, never half of it. */
    bool
    write_file_atomic(const fs::path &path, const std::string &text) {
      try {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        auto temp = path;
        temp += ".tmp";
        {
          std::ofstream out(temp, std::ios::binary | std::ios::trunc);
          out << text;
          out.flush();
          if (!out) return false;
        }
        fs::rename(temp, path, ec);
        return !ec;
      }
      catch (...) {
        return false;
      }
    }
  }  // namespace

  std::string
  service_t::peek_install_id() {
    {
      std::lock_guard lock { mutex_ };
      if (!install_id_.empty()) return install_id_;
    }
    std::ifstream in(options_.dir / "install_id");
    std::string text;
    if (in && std::getline(in, text)) {
      while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
      if (opt_out::is_uuid(text)) return text;
    }
    return {};
  }

  void
  service_t::note_product(std::string_view product, std::string_view version) {
    try {
      const auto safe = sanitize_version(version);
      if (product == "host" || safe.empty()) return;
      std::lock_guard lock { opt_out_mutex_ };
      auto &slot = products_seen_[std::string { product }];
      if (slot == safe) return;
      slot = safe;
      json j = json::object();
      for (const auto &[p, v] : products_seen_) j[p] = v;
      std::error_code ec;
      fs::create_directories(options_.dir, ec);
      write_file_atomic(options_.dir / "products_seen.json", j.dump());
    }
    catch (...) {
    }
  }

  bool
  service_t::reconcile_consent() {
    try {
      const auto c = consent_now();
      opt_out::state_t before;
      {
        std::ifstream in(options_.dir / "consent_state.json");
        const auto stored = in ? json::parse(in, nullptr, false) : json();
        if (!stored.is_discarded()) before = opt_out::state_from_json(stored);
      }
      const auto off = opt_out::turned_off(before, c.usage, c.errors);
      const bool changed = !before.known || before.usage != c.usage || before.errors != c.errors;
      if (!off.empty()) {
        // The record is saved before the new state is: a state that says "already off" without a saved record would lose the record for good (the next start would find nothing turned off).
        const auto id = peek_install_id();
        std::string host_version;
        {
          std::lock_guard lock { mutex_ };
          host_version = sanitize_version(context_.version);
        }
        if (host_version.empty()) return false;  // the context is not set yet: telemetry.cpp calls this again once it is, and the state still says "on"
        std::lock_guard lock { opt_out_mutex_ };
        std::vector<opt_out::product_version_t> products { { "host", host_version } };
        for (const auto &[p, v] : products_seen_) products.push_back({ p, v });
        opt_out::add(opt_outs_, off, products, id, deps_.now_ms());
        if (!write_file_atomic(options_.dir / "opt_out_pending.jsonl", opt_out::serialize(opt_outs_))) return false;  // not saved: the state is not committed, the next call tries again
      }
      if (changed && !write_file_atomic(options_.dir / "consent_state.json", opt_out::state_to_json({ true, c.usage, c.errors }).dump())) return false;
      // both choices are off and every turn-off is saved (the record carries the number from now on): the number leaves the install; a retry of an earlier failure ends here too
      if (!c.any()) {
        std::lock_guard lock { mutex_ };
        install_id_.clear();
        std::error_code ec;
        fs::remove(options_.dir / "install_id", ec);
      }
      return true;
    }
    catch (...) {
      return false;
    }
  }

  void
  service_t::send_opt_outs() {
    try {
      std::vector<opt_out::record_t> work;
      {
        std::lock_guard lock { opt_out_mutex_ };
        if (opt_outs_.empty() && !opt_outs_unsaved_) return;
        const auto before = opt_outs_.size();
        const auto due = opt_out::due(opt_outs_, deps_.now_ms());
        for (const auto i : due) work.push_back(opt_outs_[i]);
        // records that are too old or were tried too often are dropped for good: the file (and the anonymous id in it) goes with them, even when nothing else is waiting. When the file cannot be
        // written the pass is remembered as unsaved and repeated at the next one: the old id must not stay on the disk because one write failed.
        if (opt_outs_.size() != before || opt_outs_unsaved_) opt_outs_unsaved_ = !write_file_atomic(options_.dir / "opt_out_pending.jsonl", opt_out::serialize(opt_outs_));
      }
      if (work.empty()) return;
      std::vector<opt_out::record_t> done_or_dropped, failed_now;
      for (auto &r : work) {
        // the same pacing as every other request
        const auto at = deps_.now_ms();
        while (!post_times_.empty() && at - post_times_.front() >= 60'000) post_times_.pop_front();
        if (static_cast<int>(post_times_.size()) >= options_.max_posts_per_minute) break;
        post_times_.push_back(at);
        const auto body = opt_out::body_of(r, new_uuid_v4());
        if (body.is_null()) {
          done_or_dropped.push_back(r);
          continue;
        }
        post_result_t answer;
        try {
          if (deps_.post) answer = deps_.post(events_url(), body.dump());
        }
        catch (...) {
          answer = {};
        }
        // delivered, or refused for good (a 4xx other than 408/429): the record is done; anything else is tried again later
        if ((answer.status >= 200 && answer.status < 300) || (answer.status >= 400 && answer.status < 500 && answer.status != 408 && answer.status != 429)) done_or_dropped.push_back(r);
        else {
          opt_out::failed(r, deps_.now_ms());
          failed_now.push_back(r);
        }
      }
      std::lock_guard lock { opt_out_mutex_ };
      // the record's own random id, not t: a wall-clock jump can give a later record the same t
      const auto same = [](const opt_out::record_t &a, const opt_out::record_t &b) { return a.id == b.id; };
      opt_outs_.erase(std::remove_if(opt_outs_.begin(), opt_outs_.end(), [&](const auto &r) { return std::any_of(done_or_dropped.begin(), done_or_dropped.end(), [&](const auto &d) { return same(r, d); }); }), opt_outs_.end());
      for (auto &r : opt_outs_)
        for (const auto &f : failed_now)
          if (same(r, f)) {
            r.tries = f.tries;
            r.next_try_ms = f.next_try_ms;
          }
      opt_outs_unsaved_ = !write_file_atomic(options_.dir / "opt_out_pending.jsonl", opt_out::serialize(opt_outs_));
    }
    catch (...) {
    }
  }

  json
  service_t::preview(std::size_t max) const {
    std::lock_guard lock { mutex_ };
    std::vector<event_t> shown { ring_.begin(), ring_.end() };
    // what waits for the aggregate interval is shown the way it will leave: merged
    if (!aggregate_ring_.empty()) {
      std::vector<std::vector<std::size_t>> origin;
      for (auto &m : aggregate_events(std::vector<event_t> { aggregate_ring_.begin(), aggregate_ring_.end() }, origin)) shown.push_back(std::move(m));
    }
    json out = json::array();
    const auto start = shown.size() > max ? shown.size() - max : 0;
    for (std::size_t i = start; i < shown.size(); ++i) {
      auto e = event_to_json(shown[i]);
      e["product"] = shown[i].product;
      out.push_back(std::move(e));
    }
    return out;
  }

  service_counts_t
  service_t::counts() const {
    std::lock_guard lock { mutex_ };
    return counts_;
  }

  std::size_t
  service_t::buffered() const {
    std::lock_guard lock { mutex_ };
    return ring_.size() + aggregate_ring_.size();
  }

}  // namespace telemetry

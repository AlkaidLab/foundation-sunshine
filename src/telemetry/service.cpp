/**
 * @file src/telemetry/service.cpp
 * @brief The telemetry sender: buffer, queue, retry, consent, identity and stream trackers.
 */
#include "service.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>

using json = nlohmann::json;
using namespace std::literals;
namespace fs = std::filesystem;

namespace telemetry {

  namespace {
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

    std::uint32_t
    fnv1a(const std::string &text) {
      std::uint32_t h = 2166136261u;
      for (unsigned char c : text) {
        h ^= c;
        h *= 16777619u;
      }
      return h;
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
    if (!deps_.now_ms) deps_.now_ms = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); };
    if (!deps_.consent) deps_.consent = [] { return consent_t {}; };
    if (!deps_.random01) {
      deps_.random01 = [] {
        std::lock_guard lock { rng_mutex() };
        return std::uniform_real_distribution<double>(0.0, 1.0)(rng());
      };
    }
    last_flush_ms_ = deps_.now_ms();
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

  bool
  service_t::emit(kind_e kind, std::string_view name, const json &props, std::string_view product, std::string_view version) {
    try {
      if (!known_product(product)) return false;
      if (!consent_now().allows(kind)) {
        std::lock_guard lock { mutex_ };
        ++counts_.refused;
        return false;
      }
      auto shaped = shape_event(kind, name, props, deps_.now_ms());
      if (!shaped) return false;
      shaped->product = std::string { product };
      shaped->version = sanitize_version(version);
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

  std::size_t
  service_t::ingest(std::string_view product, std::string_view version, const json &events) {
    if (!events.is_array() || !known_product(product)) return 0;
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
      if (emit(*kind, name, e.contains("props") ? e["props"] : json::object(), product, version)) ++kept;
    }
    return kept;
  }

  // ---- streams ----

  void
  service_t::stream_begin(const stream_dims_t &dims, std::uint32_t session_id, std::string_view client_key) {
    try {
      const auto now = deps_.now_ms();
      auto stats = std::make_shared<stream_stats_t>(session_id, dims, now);
      {
        std::unique_lock lock { streams_mutex_ };
        const auto key = std::string { client_key };
        const auto found = last_end_by_client_.find(key);
        if (!key.empty() && found != last_end_by_client_.end() && now - found->second <= options_.reconnect_window_ms) stats->set_reconnects(1);
        client_of_[session_id] = key;
        streams_[session_id] = std::move(stats);
      }
      emit(kind_e::usage, "stream_start", dims_to_props(dims));
    }
    catch (...) {
    }
  }

  void
  service_t::quality_event(const stream_stats_t &stats, std::int64_t now) {
    emit(kind_e::experience, "stream_quality", stats.quality_props(now));
  }

  void
  service_t::stream_end(std::uint32_t session_id, std::string_view stop_reason) {
    try {
      std::shared_ptr<stream_stats_t> stats;
      const auto now = deps_.now_ms();
      {
        std::unique_lock lock { streams_mutex_ };
        const auto found = streams_.find(session_id);
        if (found == streams_.end()) return;
        stats = std::move(found->second);
        streams_.erase(found);
        const auto client = client_of_.find(session_id);
        if (client != client_of_.end()) {
          if (!client->second.empty()) {
            last_end_by_client_[client->second] = now;
            if (last_end_by_client_.size() > 64) last_end_by_client_.erase(last_end_by_client_.begin());
          }
          client_of_.erase(client);
        }
      }
      if (stats->take_first_frame_report() && stats->first_frame_ms()) emit(kind_e::experience, "startup", json { { "first_frame_ms", *stats->first_frame_ms() } });
      auto props = dims_to_props(stats->dims());
      props["duration_s"] = std::max<std::int64_t>(0, (now - stats->start_ms()) / 1000);
      props["result"] = result_of_stop_reason(stop_reason);
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
      if (found != streams_.end()) found->second->on_encode_ms(ms);
    }
    catch (...) {
    }
  }

  void
  service_t::record_capture_ms(std::uint32_t session_id, double ms) noexcept {
    try {
      std::shared_lock lock { streams_mutex_ };
      const auto found = streams_.find(session_id);
      if (found != streams_.end()) found->second->on_capture_ms(ms);
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
        if (stats->take_first_frame_report()) {
          if (const auto first = stats->first_frame_ms()) emit(kind_e::experience, "startup", json { { "first_frame_ms", *first } });
        }
      }
    }
    catch (...) {
    }
  }

  // ---- the log ----

  void
  service_t::note_log(int severity, const char *message, std::size_t length, std::string_view component) noexcept {
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
        found = raw_errors_.emplace(key, raw_error_t { std::string { component }, raw, severity >= 5 ? 5 : 4, 0 }).first;
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
    for (const auto &[key, raw] : raws) {
      // The line itself is not sent. Its code is a hash of the words it starts with, up to the first character that starts a value (a quote, a bracket, ':' '=' or a digit): the fixed text of the message, so the same error is one code and what a person chose is not in it.
      std::string head;
      for (const char c : scrub_log_message(raw.raw, MAX_MESSAGE_CHARS)) {
        if (std::isdigit(static_cast<unsigned char>(c)) || c == ':' || c == '=' || c == '"' || c == '\'' || c == '[' || c == '(' || c == '{' || c == '<' || c == '/' || c == '\\') break;
        head.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }
      while (!head.empty() && head.back() == ' ') head.pop_back();
      char code[16];
      std::snprintf(code, sizeof(code), "e%08x", fnv1a(head));
      emit(kind_e::error, "error", json { { "component", catalogue_id(raw.component, 32).empty() ? "host" : catalogue_id(raw.component, 32) }, { "code", code }, { "severity", raw.severity >= 5 ? "fatal" : "error" }, { "count", raw.count } });
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
      std::deque<event_t> taken;
      {
        std::lock_guard lock { mutex_ };
        taken.swap(ring_);
        last_flush_ms_ = now;
      }
      if (taken.empty()) return;
      const auto c = consent_now();
      std::map<std::string, std::vector<event_t>> by_product;
      for (auto &e : taken) {
        if (!c.allows(e.kind)) continue;
        by_product[e.product].push_back(std::move(e));
      }
      if (by_product.empty()) return;
      auto ctx = context();
      ctx.install_id = install_id();
      if (ctx.install_id.empty()) return;
      ctx.session_id = session_id_;
      for (const auto &[product, events] : by_product) {
        for (const auto &envelope : build_envelopes(ctx, product, events)) {
          std::lock_guard lock { queue_mutex_ };
          queue_.push(envelope);
        }
      }
    }
    catch (...) {
    }
  }

  int
  service_t::pump() {
    int sent = 0;
    try {
      if (deps_.now_ms() < not_before_ms_) return 0;
      const auto c = consent_now();
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
        const int status = answer.status;
        const auto now = deps_.now_ms();
        if (status >= 200 && status < 300) {
          {
            std::lock_guard lock { queue_mutex_ };
            queue_.remove(entry->path);
          }
          failures_ = 0;
          not_before_ms_ = 0;
          ++sent;
          std::lock_guard lock { mutex_ };
          ++counts_.sent_batches;
          counts_.sent_events += count;
        }
        else if (status_retryable(status)) {
          ++failures_;
          // a 429 or 503 that says how long to wait is believed (up to an hour); otherwise the backoff decides
          const auto asked = std::min<std::int64_t>(answer.retry_after_s, options_.max_backoff_ms / 1000) * 1000;
          not_before_ms_ = now + std::max<std::int64_t>(asked, backoff_seconds(failures_, deps_.random01()) * 1000);
          std::lock_guard lock { mutex_ };
          ++counts_.failed_posts;
          break;
        }
        else {
          // refused for good (a 4xx that a retry will not change): the batch is dropped
          {
            std::lock_guard lock { queue_mutex_ };
            queue_.remove(entry->path);
          }
          std::lock_guard lock { mutex_ };
          ++counts_.rejected_batches;
        }
      }
    }
    catch (...) {
    }
    return sent;
  }

  void
  service_t::consent_changed() {
    try {
      const auto c = consent_now();
      {
        std::lock_guard lock { mutex_ };
        ring_.erase(std::remove_if(ring_.begin(), ring_.end(), [&](const event_t &e) { return !c.allows(e.kind); }), ring_.end());
      }
      if (!c.errors) {
        std::lock_guard lock { log_mutex_ };
        raw_errors_.clear();
        log_windows_.clear();
      }
      std::lock_guard queue_lock { queue_mutex_ };
      if (!c.any()) {
        queue_.clear();
        std::lock_guard lock { mutex_ };
        install_id_.clear();
        std::error_code ec;
        fs::remove(options_.dir / "install_id", ec);
      }
      else {
        queue_.refilter(c.usage, c.errors);
      }
    }
    catch (...) {
    }
  }

  json
  service_t::preview(std::size_t max) const {
    std::lock_guard lock { mutex_ };
    json out = json::array();
    const auto start = ring_.size() > max ? ring_.size() - max : 0;
    for (std::size_t i = start; i < ring_.size(); ++i) {
      auto e = event_to_json(ring_[i]);
      e["product"] = ring_[i].product;
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
    return ring_.size();
  }

}  // namespace telemetry

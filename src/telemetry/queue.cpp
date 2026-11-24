/**
 * @file src/telemetry/queue.cpp
 * @brief The on-disk queue of telemetry batches.
 */
#include "queue.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace telemetry {

  disk_queue_t::disk_queue_t(fs::path dir, queue_limits_t limits):
      dir_(std::move(dir)), limits_(limits) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    // an interrupted write leaves a temporary file: it was never complete
    for (const auto &entry : fs::directory_iterator(dir_, ec)) {
      if (entry.path().extension() == ".tmp") fs::remove(entry.path(), ec);
    }
    // files that an earlier version wrote carry no class letter: a priority 0 batch with a feedback report or an error in it is given its class now, so it leaves at once like the new ones and is kept as long as they are
    std::vector<fs::path> legacy;
    for (const auto &entry : fs::directory_iterator(dir_, ec))
      if (entry.is_regular_file(ec) && entry.path().extension() == ".json" && priority_of_name(entry.path()) == 0 && delivery_of_name(entry.path()) == delivery_e::routine) legacy.push_back(entry.path());
    for (const auto &file : legacy) {
      const auto size = fs::file_size(file, ec);
      if (ec || size == 0 || size > limits_.max_file_bytes) continue;
      std::ifstream in(file, std::ios::binary);
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      const auto batch = json::parse(text, nullptr, false);
      if (batch.is_discarded() || !batch.is_object() || !batch.contains("events") || !batch["events"].is_array()) continue;
      bool report = false, error = false;
      for (const auto &e : batch["events"]) {
        if (!e.is_object()) continue;
        const auto name = e.contains("name") && e["name"].is_string() ? e["name"].get<std::string>() : std::string {};
        if (name == "user_report") report = true;
        else if (e.contains("kind") && e["kind"].is_string() && e["kind"].get<std::string>() == "error") error = true;
      }
      if (!report && !error) continue;
      auto renamed = file;
      renamed.replace_filename(file.stem().string() + (report ? "f" : "e") + ".json");
      fs::rename(file, renamed, ec);
    }
  }

  std::vector<fs::path>
  disk_queue_t::list() const {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir_, ec)) {
      if (entry.is_regular_file(ec) && entry.path().extension() == ".json") files.push_back(entry.path());
    }
    // errors and feedback first (feedback ahead of errors), then by priority; the oldest first within each
    const auto rank = [](const fs::path &f) {
      const auto d = delivery_of_name(f);
      return d == delivery_e::feedback ? 0 : d == delivery_e::error ? 1 : 2;
    };
    std::sort(files.begin(), files.end(), [&](const fs::path &a, const fs::path &b) {
      const auto ra = rank(a), rb = rank(b);
      if (ra != rb) return ra < rb;
      const auto pa = priority_of_name(a), pb = priority_of_name(b);
      return pa != pb ? pa < pb : a.filename() < b.filename();
    });
    return files;
  }

  delivery_e
  disk_queue_t::delivery_of_name(const fs::path &path) {
    const auto stem = path.stem().string();
    const auto at = stem.rfind("-p");
    // "<time>-<counter>-p0e": the class letter follows the priority digit (f: feedback, e: error)
    if (at == std::string::npos || stem.size() != at + 4) return delivery_e::routine;
    return stem[at + 3] == 'f' ? delivery_e::feedback : stem[at + 3] == 'e' ? delivery_e::error : delivery_e::routine;
  }

  int
  disk_queue_t::priority_of_name(const fs::path &path) {
    const auto stem = path.stem().string();
    const auto at = stem.rfind("-p");
    if (at == std::string::npos || at + 2 >= stem.size()) return 2;  // an older file without a priority
    const char c = stem[at + 2];
    return c >= '0' && c <= '2' ? c - '0' : 2;
  }

  std::size_t
  disk_queue_t::files_of(int priority) const {
    std::size_t n = 0;
    for (const auto &f : list())
      if (priority_of_name(f) == priority) ++n;
    return n;
  }

  std::size_t
  disk_queue_t::files_of(delivery_e delivery) const {
    std::size_t n = 0;
    for (const auto &f : list())
      if (delivery_of_name(f) == delivery) ++n;
    return n;
  }

  std::size_t
  disk_queue_t::files() const {
    return list().size();
  }

  std::size_t
  disk_queue_t::bytes() const {
    std::size_t total = 0;
    std::error_code ec;
    for (const auto &file : list()) {
      const auto size = fs::file_size(file, ec);
      if (!ec) total += static_cast<std::size_t>(size);
    }
    return total;
  }

  std::size_t
  disk_queue_t::enforce_limits() {
    std::error_code ec;
    auto files = list();  // feedback, errors, then priority 0 to 2: the ones to give up are found by the order below
    std::vector<std::size_t> sizes;
    std::size_t total = 0;
    for (const auto &f : files) {
      const auto size = fs::file_size(f, ec);
      sizes.push_back(ec ? 0 : static_cast<std::size_t>(size));
      total += sizes.back();
    }
    std::size_t removed = 0;
    std::vector<bool> gone(files.size(), false);
    std::size_t alive = files.size();
    const auto drop = [&](std::size_t i) {
      fs::remove(files[i], ec);
      total -= sizes[i];
      gone[i] = true;
      --alive;
      ++removed;
    };
    // the classes of a file: 0 feedback, 1 error, 2 routine priority 0, 3 priority 1, 4 priority 2
    const auto tier = [&](std::size_t i) {
      const auto d = delivery_of_name(files[i]);
      if (d == delivery_e::feedback) return 0;
      if (d == delivery_e::error) return 1;
      return 2 + priority_of_name(files[i]);
    };
    const auto oldest_first = [&](int wanted_tier) {
      std::vector<std::size_t> order;
      for (std::size_t i = 0; i < files.size(); ++i)
        if (!gone[i] && tier(i) == wanted_tier) order.push_back(i);
      std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return files[a].filename() < files[b].filename(); });
      return order;
    };
    // each of the three that must arrive has a bound of its own and is cut by it alone (the oldest first)
    const std::size_t own_bounds[3] = { limits_.max_feedback_files, limits_.max_error_files, limits_.max_p0_files };
    for (int t = 0; t < 3; ++t) {
      auto order = oldest_first(t);
      for (std::size_t k = 0; order.size() - k > own_bounds[t]; ++k) drop(order[k]);
    }
    // then until both global caps hold: priority 2 goes first, then 1, then routine priority 0, then errors, and feedback last; the oldest first within each
    for (int t : { 4, 3, 2, 1, 0 }) {
      if (alive <= limits_.max_files && total <= limits_.max_bytes) break;
      for (const auto i : oldest_first(t)) {
        if (alive <= limits_.max_files && total <= limits_.max_bytes) break;
        drop(i);
      }
    }
    dropped_ += removed;
    return removed;
  }

  bool
  disk_queue_t::push(const json &batch, int priority, delivery_e delivery) {
    const auto text = batch.dump();
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    char name[64];
    // the class letter of an error or a feedback report follows the priority digit; a routine batch has none (the form of every file written before)
    const char *letter = delivery == delivery_e::feedback ? "f" : delivery == delivery_e::error ? "e" : "";
    std::snprintf(name, sizeof(name), "%013lld-%06llu-p%d%s.json", static_cast<long long>(now), static_cast<unsigned long long>(counter_++ % 1000000), std::clamp(priority, 0, 2), letter);
    const auto target = dir_ / name;
    auto temp = target;
    temp += ".tmp";
    {
      std::ofstream out(temp, std::ios::binary | std::ios::trunc);
      if (!out) return false;
      out << text;
      out.flush();
      if (!out) {
        out.close();
        std::error_code ec;
        fs::remove(temp, ec);
        return false;
      }
    }
    std::error_code ec;
    fs::rename(temp, target, ec);
    if (ec) {
      fs::remove(temp, ec);
      return false;
    }
    enforce_limits();
    // an error or a feedback report that the caps took out at once (a queue smaller than one batch) was not kept: the caller still has it
    if (delivery != delivery_e::routine && !fs::exists(target, ec)) return false;
    return true;
  }

  std::optional<disk_queue_t::entry_t>
  disk_queue_t::front(int max_priority, lane_e lane) {
    std::error_code ec;
    for (const auto &file : list()) {
      const bool urgent = delivery_of_name(file) != delivery_e::routine;
      if ((lane == lane_e::urgent && !urgent) || (lane == lane_e::routine && urgent)) continue;
      if (priority_of_name(file) > max_priority) continue;
      const auto size = fs::file_size(file, ec);
      if (ec || size == 0 || size > limits_.max_file_bytes) {
        fs::remove(file, ec);
        continue;
      }
      std::ifstream in(file, std::ios::binary);
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();  // Windows does not delete an open file: a corrupt batch removed below would stay and be read again
      auto batch = json::parse(text, nullptr, false);
      if (batch.is_discarded() || !batch.is_object() || !batch.contains("events") || !batch["events"].is_array() || !batch.contains("install_id")) {
        fs::remove(file, ec);
        continue;
      }
      return entry_t { file, std::move(batch) };
    }
    return std::nullopt;
  }

  std::vector<disk_queue_t::entry_t>
  disk_queue_t::entries(int max_priority, std::size_t limit) {
    std::vector<entry_t> out;
    std::error_code ec;
    for (const auto &file : list()) {
      if (out.size() >= limit) break;
      if (priority_of_name(file) > max_priority) continue;
      const auto size = fs::file_size(file, ec);
      if (ec || size == 0 || size > limits_.max_file_bytes) {
        fs::remove(file, ec);
        continue;
      }
      std::ifstream in(file, std::ios::binary);
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      auto batch = json::parse(text, nullptr, false);
      if (batch.is_discarded() || !batch.is_object() || !batch.contains("events") || !batch["events"].is_array() || !batch.contains("install_id")) {
        fs::remove(file, ec);
        continue;
      }
      out.push_back({ file, std::move(batch) });
    }
    return out;
  }

  void
  disk_queue_t::remove(const fs::path &path) {
    std::error_code ec;
    fs::remove(path, ec);
  }

  bool
  disk_queue_t::rewrite(const fs::path &path, const json &batch) {
    auto temp = path;
    temp += ".tmp";
    {
      std::ofstream out(temp, std::ios::binary | std::ios::trunc);
      if (!out) return false;
      out << batch.dump();
      out.flush();
      if (!out) return false;
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    return !ec;
  }

  void
  disk_queue_t::clear() {
    std::error_code ec;
    for (const auto &file : list()) fs::remove(file, ec);
  }

  void
  disk_queue_t::refilter(bool usage, bool errors) {
    std::error_code ec;
    for (const auto &file : list()) {
      const auto size = fs::file_size(file, ec);
      if (ec || size == 0 || size > limits_.max_file_bytes) {
        fs::remove(file, ec);
        continue;
      }
      std::ifstream in(file, std::ios::binary);
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      auto batch = json::parse(text, nullptr, false);
      if (batch.is_discarded() || !batch.is_object()) {
        fs::remove(file, ec);
        continue;
      }
      const auto before = batch.contains("events") && batch["events"].is_array() ? batch["events"].size() : 0;
      const auto left = filter_by_consent(batch, usage, errors);
      if (left == 0) fs::remove(file, ec);
      else if (left != before) rewrite(file, batch);
    }
  }

  std::size_t
  filter_by_consent(json &envelope, bool usage, bool errors) {
    if (!envelope.is_object() || !envelope.contains("events") || !envelope["events"].is_array()) return 0;
    json kept = json::array();
    for (auto &e : envelope["events"]) {
      const auto kind = e.contains("kind") && e["kind"].is_string() ? e["kind"].get<std::string>() : std::string {};
      // a feedback report the person sent stays: pressing 发送 was the consent for it, whatever the error-report switch says now
      const bool report = kind == "error" && e.contains("name") && e["name"].is_string() && e["name"].get<std::string>() == "user_report";
      const bool allowed = report || ((kind == "error") ? errors : (kind == "usage" || kind == "experience") ? usage : false);
      if (allowed) kept.push_back(std::move(e));
    }
    envelope["events"] = std::move(kept);
    return envelope["events"].size();
  }

  std::int64_t
  backoff_seconds(int failures, double jitter, int base_s, int cap_s) {
    if (failures < 1) return 0;
    const double base = static_cast<double>(base_s) * std::pow(2.0, std::min(failures - 1, 7));
    const double capped = std::min(base, static_cast<double>(cap_s));
    return static_cast<std::int64_t>(std::llround(capped * (1.0 + 0.2 * std::clamp(jitter, 0.0, 0.999))));
  }

  std::int64_t
  retry_after_ms(long asked_s, double jitter, std::int64_t cap_s) {
    if (asked_s <= 0) return 0;
    const auto seconds = std::min<std::int64_t>(asked_s, cap_s);
    return static_cast<std::int64_t>(std::llround(static_cast<double>(seconds) * 1000.0 * (1.0 + 0.2 * std::clamp(jitter, 0.0, 0.999))));
  }

  bool
  status_retryable(int status) {
    if (status == 0 || status == 408 || status == 425 || status == 429 || status >= 500) return true;
    // an answer that a proxy, a firewall or a service without the route yet can give for a reason that is not about the batch: it is kept, not dropped
    return status == 401 || status == 403 || status == 404 || status == 405 || status == 410 || status == 421;
  }

}  // namespace telemetry

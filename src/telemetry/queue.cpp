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
  }

  std::vector<fs::path>
  disk_queue_t::list() const {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir_, ec)) {
      if (entry.is_regular_file(ec) && entry.path().extension() == ".json") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
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
    auto files = list();
    std::size_t total = 0;
    std::vector<std::size_t> sizes;
    std::error_code ec;
    for (const auto &file : files) {
      const auto size = fs::file_size(file, ec);
      sizes.push_back(ec ? 0 : static_cast<std::size_t>(size));
      total += sizes.back();
    }
    std::size_t removed = 0, first = 0;
    while (first < files.size() && (files.size() - first > limits_.max_files || total > limits_.max_bytes)) {
      fs::remove(files[first], ec);
      total -= sizes[first];
      ++first;
      ++removed;
    }
    dropped_ += removed;
    return removed;
  }

  bool
  disk_queue_t::push(const json &batch) {
    const auto text = batch.dump();
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    char name[64];
    std::snprintf(name, sizeof(name), "%013lld-%06llu.json", static_cast<long long>(now), static_cast<unsigned long long>(counter_++ % 1000000));
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
    return true;
  }

  std::optional<disk_queue_t::entry_t>
  disk_queue_t::front() {
    std::error_code ec;
    for (const auto &file : list()) {
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
      const bool allowed = (kind == "error") ? errors : (kind == "usage" || kind == "experience") ? usage : false;
      if (allowed) kept.push_back(std::move(e));
    }
    envelope["events"] = std::move(kept);
    return envelope["events"].size();
  }

  std::int64_t
  backoff_seconds(int failures, double jitter) {
    if (failures < 1) return 0;
    const double base = 30.0 * std::pow(2.0, std::min(failures - 1, 7));
    const double capped = std::min(base, 3600.0);
    return static_cast<std::int64_t>(std::llround(capped * (1.0 + 0.2 * std::clamp(jitter, 0.0, 0.999))));
  }

  bool
  status_retryable(int status) {
    return status == 0 || status == 408 || status == 425 || status == 429 || status >= 500;
  }

}  // namespace telemetry

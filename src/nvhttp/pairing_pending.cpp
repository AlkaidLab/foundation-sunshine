/**
 * @file src/nvhttp/pairing_pending.cpp
 * @brief Pending-PIN registry for the pairing session.
 */
#include "pairing_pending.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include "src/globals.h"
#include "src/logging.h"
#include "src/tray/system_tray.h"
#include "src/tray/tray_state.h"
#include "src/uuid.h"

using namespace std::literals;

namespace nvhttp::pairing_pending {
  namespace {
    namespace pt = boost::property_tree;

    constexpr std::size_t max_pending_requests = 8;
    constexpr std::size_t max_pending_requests_per_address = 4;
    constexpr int max_creations_per_window = 12;
    constexpr int creation_window_seconds = 60;
    constexpr auto default_pending_timeout = 5min;

    // Sources: pair-rfc6585-status-20261103 section 4;
    // pair-libstdcxx-concurrency-20261103;
    // pair-msvc-condition-variable-20261103.
    struct creation_limiter_t {
      std::unordered_map<std::string, std::pair<int, std::chrono::steady_clock::time_point>> attempts;

      bool
      allow(const std::string &address, std::chrono::steady_clock::time_point now) {
        auto &entry = attempts[address];
        if (now - entry.second > std::chrono::seconds(creation_window_seconds)) {
          entry = { 0, now };
        }
        if (entry.first >= max_creations_per_window) {
          return false;
        }
        entry.first++;
        return true;
      }

      bool
      would_allow(const std::string &address, std::chrono::steady_clock::time_point now) const {
        const auto entry = attempts.find(address);
        if (entry == attempts.end()) {
          return true;
        }
        if (now - entry->second.second > std::chrono::seconds(creation_window_seconds)) {
          return true;
        }
        return entry->second.first < max_creations_per_window;
      }

      void
      cleanup(std::chrono::steady_clock::time_point now) {
        for (auto entry = attempts.begin(); entry != attempts.end();) {
          if (now - entry->second.second > std::chrono::seconds(creation_window_seconds * 2)) {
            entry = attempts.erase(entry);
          }
          else {
            ++entry;
          }
        }
      }
    };

    struct entry_t {
      std::string unique_id;
      std::string request_id;
      std::string device_name;
      std::string source_address;
      std::chrono::system_clock::time_point started_at;
      std::chrono::system_clock::time_point expires_at;
      std::chrono::steady_clock::time_point expires_steady;
      response_t response;
    };

    std::mutex mutex;
    std::condition_variable changed;
    std::unordered_map<std::string, entry_t> entries;
    std::uint64_t generation = 0;
    std::chrono::milliseconds pending_timeout { default_pending_timeout };
    creation_limiter_t creation_limiter;

    void
    notify_changed_locked() {
      ++generation;
      changed.notify_all();
    }

    std::size_t
    encode_utf8(std::uint32_t code_point, std::array<char, 4> &out) {
      if (code_point < 0x80) {
        out[0] = static_cast<char>(code_point);
        return 1;
      }
      if (code_point < 0x800) {
        out[0] = static_cast<char>(0xC0 | (code_point >> 6));
        out[1] = static_cast<char>(0x80 | (code_point & 0x3F));
        return 2;
      }
      if (code_point < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (code_point >> 12));
        out[1] = static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (code_point & 0x3F));
        return 3;
      }
      out[0] = static_cast<char>(0xF0 | (code_point >> 18));
      out[1] = static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
      out[2] = static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
      out[3] = static_cast<char>(0x80 | (code_point & 0x3F));
      return 4;
    }

    // Returns the bytes consumed, or zero for a malformed sequence.
    std::size_t
    decode_utf8(std::string_view input, std::size_t offset, std::uint32_t &code_point) {
      const auto first = static_cast<unsigned char>(input[offset]);
      if (first < 0x80) {
        code_point = first;
        return 1;
      }

      std::size_t length = 0;
      std::uint32_t minimum = 0;
      if ((first & 0xE0) == 0xC0) {
        length = 2;
        code_point = first & 0x1F;
        minimum = 0x80;
      }
      else if ((first & 0xF0) == 0xE0) {
        length = 3;
        code_point = first & 0x0F;
        minimum = 0x800;
      }
      else if ((first & 0xF8) == 0xF0) {
        length = 4;
        code_point = first & 0x07;
        minimum = 0x10000;
      }
      else {
        return 0;
      }

      if (offset + length > input.size()) {
        return 0;
      }
      for (std::size_t index = 1; index < length; ++index) {
        const auto next = static_cast<unsigned char>(input[offset + index]);
        if ((next & 0xC0) != 0x80) {
          return 0;
        }
        code_point = (code_point << 6) | (next & 0x3F);
      }

      if (code_point < minimum || code_point > 0x10FFFF ||
          (code_point >= 0xD800 && code_point <= 0xDFFF)) {
        return 0;
      }
      return length;
    }

    void
    write_response(response_t &response, const std::string &xml) {
      if (response.has_left() && response.left()) {
        response.left()->write(xml);
      }
      else if (response.has_right() && response.right()) {
        response.right()->write(xml);
      }
    }

    void
    refresh_indicator_locked() {
      const auto *newest = static_cast<const entry_t *>(nullptr);
      for (const auto &[id, entry] : entries) {
        if (!newest || entry.started_at > newest->started_at) {
          newest = &entry;
        }
      }

      if (!newest) {
        tray_state::clear_pairing_required();
        return;
      }

      const auto name = newest->device_name.empty() ? "Unknown device"s : newest->device_name;
      tray_state::set_pairing_required(name);
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_require_pin(name);
#endif
    }

    void
    erase_locked(std::unordered_map<std::string, entry_t>::iterator entry) {
      entries.erase(entry);
      refresh_indicator_locked();
      notify_changed_locked();
    }

    void
    expire(std::string unique_id, std::string request_id) {
      std::lock_guard lock { mutex };
      const auto entry = entries.find(request_id);
      if (entry == entries.end() || entry->second.unique_id != unique_id) {
        return;
      }

      send_xml(entry->second.response, 408, "Pairing PIN entry timed out");
      BOOST_LOG(info) << "Pairing request expired while waiting for PIN entry: id="
                      << mask_for_log(request_id);
      erase_locked(entry);
    }

    void
    purge_expired_locked(std::chrono::steady_clock::time_point now) {
      std::vector<std::string> expired;
      for (const auto &[id, entry] : entries) {
        if (entry.expires_steady <= now) {
          expired.push_back(id);
        }
      }

      for (const auto &id : expired) {
        const auto entry = entries.find(id);
        if (entry == entries.end()) {
          continue;
        }
        send_xml(entry->second.response, 408, "Pairing PIN entry timed out");
        BOOST_LOG(info) << "Pairing request expired while waiting for PIN entry: id="
                        << mask_for_log(id);
        erase_locked(entry);
      }
    }

    std::string
    make_request_id_locked() {
      std::string id;
      do {
        id = uuid_util::uuid_t::generate().string();
      } while (entries.find(id) != entries.end());
      return id;
    }
  }  // namespace

  std::string
  sanitize_device_name(std::string_view name) {
    constexpr std::size_t max_bytes = 64;
    std::string result;
    result.reserve(std::min(name.size(), max_bytes));
    bool pending_space = false;
    std::size_t offset = 0;
    while (offset < name.size()) {
      std::uint32_t code_point = 0;
      const auto consumed = decode_utf8(name, offset, code_point);
      if (consumed == 0) {
        ++offset;
        continue;
      }
      offset += consumed;

      const bool whitespace = code_point == ' ' || code_point == '\t' || code_point == '\r' || code_point == '\n';
      const bool control = code_point < 0x20 || (code_point >= 0x7F && code_point <= 0x9F);
      const bool format = code_point == 0x00AD || code_point == 0x061C ||
                          code_point == 0x200B || code_point == 0x200E || code_point == 0x200F ||
                          (code_point >= 0x202A && code_point <= 0x202E) ||
                          (code_point >= 0x2060 && code_point <= 0x206F) ||
                          code_point == 0xFEFF;
      if (whitespace || control || format) {
        pending_space = !result.empty();
        continue;
      }

      std::array<char, 4> encoded {};
      const auto encoded_length = encode_utf8(code_point, encoded);
      const auto required = result.size() + (pending_space ? 1 : 0) + encoded_length;
      if (required > max_bytes) {
        break;
      }
      if (pending_space) {
        result.push_back(' ');
        pending_space = false;
      }
      result.append(encoded.data(), encoded_length);
    }

    if (result.empty()) {
      return "Unknown device"s;
    }
    return result;
  }

  std::string
  mask_for_log(std::string_view value) {
    constexpr std::size_t visible = 8;
    if (value.empty()) {
      return "<none>"s;
    }
    std::string masked { value.substr(0, std::min(value.size(), visible)) };
    if (value.size() > visible) {
      masked += "...";
    }
    return masked;
  }

  void
  send_xml(response_t &response, int status_code, const std::string &status_message) {
    pt::ptree tree;
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", status_code);
    tree.put("root.<xmlattr>.status_message", status_message);
    std::ostringstream data;
    pt::write_xml(data, tree);
    write_response(response, data.str());
  }

  void
  write_xml(response_t &response, const std::string &xml) {
    write_response(response, xml);
  }

  add_result_e
  add(const std::string &unique_id, std::string_view device_name,
    const std::string &source_address, response_t &response) {
    std::lock_guard lock { mutex };
    const auto now_steady = std::chrono::steady_clock::now();
    purge_expired_locked(now_steady);
    creation_limiter.cleanup(now_steady);

    std::size_t per_address = 0;
    for (const auto &[id, entry] : entries) {
      if (entry.source_address == source_address) {
        ++per_address;
      }
    }
    if (entries.size() >= max_pending_requests ||
        per_address >= max_pending_requests_per_address ||
        !creation_limiter.allow(source_address, now_steady)) {
      return add_result_e::rate_limited;
    }

    const auto now = std::chrono::system_clock::now();
    const auto timeout = pending_timeout;
    entry_t entry;
    entry.unique_id = unique_id;
    entry.request_id = make_request_id_locked();
    entry.device_name = sanitize_device_name(device_name);
    entry.source_address = source_address;
    entry.started_at = now;
    entry.expires_at = now + timeout;
    entry.expires_steady = now_steady + timeout;
    entry.response = std::move(response);

    const auto request_id = entry.request_id;
    entries.emplace(request_id, std::move(entry));
    refresh_indicator_locked();
    notify_changed_locked();

    BOOST_LOG(info) << "Pairing request waiting for PIN: id=" << mask_for_log(request_id)
                    << " device_bytes=" << device_name.size()
                    << " from=" << source_address;

    task_pool.pushDelayed([unique_id, request_id]() {
      expire(unique_id, request_id);
    },
      timeout);
    return add_result_e::added;
  }

  bool
  can_add(const std::string &source_address) {
    std::lock_guard lock { mutex };
    const auto now = std::chrono::steady_clock::now();
    purge_expired_locked(now);

    std::size_t per_address = 0;
    for (const auto &[id, entry] : entries) {
      if (entry.source_address == source_address) {
        ++per_address;
      }
    }
    return entries.size() < max_pending_requests &&
           per_address < max_pending_requests_per_address &&
           creation_limiter.would_allow(source_address, now);
  }

  bool
  has_pending(const std::string &unique_id) {
    std::lock_guard lock { mutex };
    const auto now = std::chrono::steady_clock::now();
    purge_expired_locked(now);
    for (const auto &[id, entry] : entries) {
      if (entry.unique_id == unique_id) {
        return true;
      }
    }
    return false;
  }

  void
  remove(const std::string &unique_id) {
    std::lock_guard lock { mutex };
    for (auto entry = entries.begin(); entry != entries.end(); ++entry) {
      if (entry->second.unique_id != unique_id) {
        continue;
      }
      send_xml(entry->second.response, 400, "Pairing request cancelled");
      erase_locked(entry);
      return;
    }
  }

  std::optional<taken_t>
  take(const std::string &request_id) {
    std::lock_guard lock { mutex };
    purge_expired_locked(std::chrono::steady_clock::now());

    auto selected = entries.end();
    for (auto entry = entries.begin(); entry != entries.end(); ++entry) {
      if (!request_id.empty()) {
        if (entry->first == request_id) {
          selected = entry;
          break;
        }
        continue;
      }
      if (selected == entries.end() || entry->second.started_at < selected->second.started_at) {
        selected = entry;
      }
    }

    if (selected == entries.end()) {
      return std::nullopt;
    }

    taken_t result;
    result.unique_id = std::move(selected->second.unique_id);
    result.request_id = selected->first;
    result.device_name = std::move(selected->second.device_name);
    result.response = std::move(selected->second.response);
    erase_locked(selected);
    return result;
  }

  bool
  reject(const std::string &request_id) {
    std::lock_guard lock { mutex };
    purge_expired_locked(std::chrono::steady_clock::now());
    const auto entry = entries.find(request_id);
    if (entry == entries.end()) {
      return false;
    }

    send_xml(entry->second.response, 403, "Pairing request rejected");
    BOOST_LOG(info) << "Pairing request rejected: id=" << mask_for_log(request_id);
    erase_locked(entry);
    return true;
  }

  pending_pair_snapshot_t
  snapshot() {
    std::lock_guard lock { mutex };
    purge_expired_locked(std::chrono::steady_clock::now());

    pending_pair_snapshot_t result;
    result.generation = generation;
    std::vector<const entry_t *> ordered;
    ordered.reserve(entries.size());
    for (const auto &[id, entry] : entries) {
      ordered.push_back(&entry);
    }
    std::sort(ordered.begin(), ordered.end(), [](const entry_t *left, const entry_t *right) {
      if (left->started_at != right->started_at) {
        return left->started_at < right->started_at;
      }
      return left->request_id < right->request_id;
    });

    for (const auto *entry : ordered) {
      pending_pair_request_t request;
      request.id = entry->request_id;
      request.device_name = entry->device_name.empty() ? "Unknown device"s : entry->device_name;
      request.started_at = std::chrono::duration_cast<std::chrono::seconds>(
        entry->started_at.time_since_epoch())
                             .count();
      request.expires_at = std::chrono::duration_cast<std::chrono::seconds>(
        entry->expires_at.time_since_epoch())
                             .count();
      result.requests.push_back(std::move(request));
    }
    return result;
  }

  bool
  wait(std::uint64_t expected_generation, std::chrono::milliseconds timeout) {
    std::unique_lock lock { mutex };
    if (generation != expected_generation) {
      return true;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (generation == expected_generation) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        break;
      }
      const auto slice = std::min<std::chrono::steady_clock::duration>(
        deadline - now, 250ms);
      changed.wait_for(lock, slice);
      if (generation != expected_generation) {
        break;
      }
      purge_expired_locked(std::chrono::steady_clock::now());
    }
    return generation != expected_generation;
  }

#ifdef SUNSHINE_TESTS
  void
  set_timeout_for_test(std::chrono::milliseconds timeout) {
    std::lock_guard lock { mutex };
    pending_timeout = timeout;
  }

  void
  reset_for_test() {
    std::lock_guard lock { mutex };
    entries.clear();
    generation = 0;
    pending_timeout = default_pending_timeout;
    creation_limiter.attempts.clear();
    changed.notify_all();
    tray_state::clear_pairing_required();
  }
#endif
}  // namespace nvhttp::pairing_pending

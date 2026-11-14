/**
 * @file src/confighttp/session_store.cpp
 * @brief Server-side Web UI session table.
 *
 * Durable records contain only random-token verifiers and are written with
 * the same restricted file policy as local-auth.token [p0-owasp-session-20261109].
 */

#include "session_store.h"
#include "src/crypto.h"
#include "src/utility.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <utility>

namespace confighttp::sessions {
  namespace {
    constexpr auto maximum_remember_absolute = std::chrono::seconds(24 * 30 * 60 * 60);

    std::string
    hash_hex(const std::string_view value) {
      return util::hex(crypto::hash(value)).to_string();
    }

    bool
    constant_time_equal(const std::string_view left, const std::string_view right) {
      // Both sides are fixed-length digests or the same fixed-length token.
      if (left.size() != right.size()) {
        return false;
      }
      return CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
    }

    std::string
    mask_address(const std::string_view address) {
      auto value = std::string { address };
      if (const auto scope = value.find('%'); scope != std::string::npos) {
        value.erase(scope);
      }

      if (value.find(':') != std::string::npos) {
        std::vector<std::string> groups;
        std::size_t begin = 0;
        while (begin < value.size() && groups.size() < 4) {
          const auto end = value.find(':', begin);
          auto group = value.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
          if (!group.empty()) {
            groups.push_back(std::move(group));
          }
          if (end == std::string::npos) {
            break;
          }
          begin = end + 1;
        }
        if (groups.size() < 4) {
          return "::x";
        }
        std::string masked;
        for (std::size_t index = 0; index != groups.size(); ++index) {
          if (index != 0) {
            masked.push_back(':');
          }
          masked.append(groups[index]);
        }
        masked.append("::x");
        return masked;
      }

      const auto first = value.find('.');
      const auto second = first == std::string::npos ? std::string::npos : value.find('.', first + 1);
      const auto third = second == std::string::npos ? std::string::npos : value.find('.', second + 1);
      if (third == std::string::npos) {
        return "masked";
      }
      return value.substr(0, third) + ".x";
    }

    std::string
    short_user_agent(const std::string_view value) {
      constexpr std::size_t maximum = 96;
      std::string shortened;
      const auto length = std::min(value.size(), maximum);
      shortened.reserve(length + 3);
      for (std::size_t index = 0; index != length; ++index) {
        const auto character = static_cast<unsigned char>(value[index]);
        shortened.push_back(character < 0x20 || character == 0x7F ? '?' : static_cast<char>(character));
      }
      if (value.size() > maximum) {
        shortened.append("...");
      }
      return shortened;
    }

    time_point_t
    earliest(const time_point_t left, const time_point_t right) {
      return left < right ? left : right;
    }

    bool
    valid_hash(const std::string_view value) {
      if (value.size() != 64) return false;
      return std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f') ||
               (character >= 'A' && character <= 'F');
      });
    }

    std::optional<time_point_t>
    time_from_json(const nlohmann::json &value) {
      if (!value.is_number_integer()) return std::nullopt;
      const auto seconds = value.get<std::int64_t>();
      if (seconds < 0) return std::nullopt;
      return time_point_t { std::chrono::seconds { seconds } };
    }

    std::int64_t
    time_to_json(const time_point_t value) {
      return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
    }
  }  // namespace

  store_t::store_t(policy_t policy, std::function<time_point_t()> clock):
      policy_(std::move(policy)),
      clock_(std::move(clock)) {
    policy_.remember_absolute = std::min(policy_.remember_absolute, maximum_remember_absolute);
  }

  void
  store_t::configure(policy_t policy, std::filesystem::path persistence_path, persistence_writer_t writer) {
    std::lock_guard lock(mutex_);
    policy_ = std::move(policy);
    policy_.remember_absolute = std::min(policy_.remember_absolute, maximum_remember_absolute);
    if (persistence_loaded_ && persistence_path == persistence_path_) return;
    records_.clear();
    persistence_path_ = std::move(persistence_path);
    persistence_writer_ = std::move(writer);
    persistence_loaded_ = true;
    load_locked();
  }

  void
  store_t::load_locked() {
    if (persistence_path_.empty() || !persistence_writer_) return;

    std::ifstream input(persistence_path_, std::ios::binary);
    if (!input) return;
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length <= 0 || length > 128 * 1024) return;
    input.seekg(0, std::ios::beg);
    std::string contents(static_cast<std::size_t>(length), '\0');
    input.read(contents.data(), length);
    if (!input) return;

    try {
      const auto document = nlohmann::json::parse(contents);
      if (!document.is_object() || document.value("version", 0) != 1 ||
          !document.contains("sessions") || !document["sessions"].is_array()) {
        return;
      }

      for (const auto &value : document["sessions"]) {
        if (records_.size() >= maximum_sessions || !value.is_object() ||
            value.value("remember", false) != true) {
          continue;
        }
        const auto token_hash = value.value("token_hash", std::string {});
        const auto csrf_hash = value.value("csrf_hash", std::string {});
        const auto credential_hash = value.value("credential_hash", std::string {});
        const auto username = value.value("username", std::string {});
        const auto masked_address = value.value("masked_address", std::string {});
        const auto short_agent = value.value("short_user_agent", std::string {});
        const auto created_at = time_from_json(value.value("created_at", nlohmann::json {}));
        const auto last_seen = time_from_json(value.value("last_seen", nlohmann::json {}));
        const auto idle_expires_at = time_from_json(value.value("idle_expires_at", nlohmann::json {}));
        const auto absolute_expires_at = time_from_json(value.value("absolute_expires_at", nlohmann::json {}));
        if (!valid_hash(token_hash) || !valid_hash(csrf_hash) || !valid_hash(credential_hash) ||
            username.empty() || username.size() > 128 || !created_at || !last_seen ||
            !idle_expires_at || !absolute_expires_at || *absolute_expires_at <= now() ||
            *idle_expires_at <= now()) {
          continue;
        }
        records_.emplace(token_hash, record_t {
                                       .token_hash = token_hash,
                                       .csrf_hash = csrf_hash,
                                       .username = username,
                                       .created_at = *created_at,
                                       .last_seen = *last_seen,
                                       .idle_expires_at = *idle_expires_at,
                                       .absolute_expires_at = *absolute_expires_at,
                                       .credential_hash = credential_hash,
                                       .masked_address = masked_address,
                                       .short_user_agent = short_agent,
                                       .remember = true,
                                     });
      }
    }
    catch (const std::exception &) {
      records_.clear();
    }
  }

  bool
  store_t::persist_locked() const {
    if (persistence_path_.empty() || !persistence_writer_) return true;

    nlohmann::json sessions = nlohmann::json::array();
    for (const auto &[token_hash, record] : records_) {
      if (!record.remember) continue;
      sessions.push_back({
        { "token_hash", token_hash },
        { "csrf_hash", record.csrf_hash },
        { "username", record.username },
        { "created_at", time_to_json(record.created_at) },
        { "last_seen", time_to_json(record.last_seen) },
        { "idle_expires_at", time_to_json(record.idle_expires_at) },
        { "absolute_expires_at", time_to_json(record.absolute_expires_at) },
        { "credential_hash", record.credential_hash },
        { "masked_address", record.masked_address },
        { "short_user_agent", record.short_user_agent },
        { "remember", true },
      });
    }
    const nlohmann::json document {
      { "version", 1 },
      { "sessions", std::move(sessions) },
    };
    return persistence_writer_(persistence_path_, document.dump());
  }

  time_point_t
  store_t::now() const {
    return clock_ ? clock_() : clock_t::now();
  }

  std::optional<created_session_t>
  store_t::create(std::string username, bool remember, client_t client,
    std::string credential_fingerprint,
    std::optional<std::string_view> rotate_from) {
    const auto token_bytes = crypto::rand(32);
    const auto csrf_bytes = crypto::rand(32);
    if (token_bytes.size() != 32 || csrf_bytes.size() != 32) {
      return std::nullopt;
    }

    auto token = util::hex_vec(token_bytes);
    auto csrf_token = util::hex_vec(csrf_bytes);
    auto token_hash = hash_hex(token);
    const auto at = now();

    record_t record {
      .token_hash = token_hash,
      .csrf_hash = hash_hex(csrf_token),
      .username = std::move(username),
      .created_at = at,
      .last_seen = at,
      .idle_expires_at = at + (remember ? policy_.remember_idle : policy_.idle),
      .absolute_expires_at = at + (remember ? policy_.remember_absolute : policy_.absolute),
      .credential_hash = std::move(credential_fingerprint),
      .masked_address = mask_address(client.address),
      .short_user_agent = short_user_agent(client.user_agent),
      .remember = remember,
    };

    created_session_t created;
    created.token = std::move(token);
    created.csrf_token = std::move(csrf_token);
    created.public_id = token_hash.substr(0, 32);
    created.username = record.username;
    created.created_at = at;
    created.last_seen = at;
    created.expires_at = earliest(record.idle_expires_at, record.absolute_expires_at);
    created.absolute_expires_at = record.absolute_expires_at;
    created.remember = remember;

    std::lock_guard lock(mutex_);
    if (rotate_from) {
      records_.erase(hash_hex(*rotate_from));
    }
    records_.insert_or_assign(token_hash, std::move(record));
    enforce_capacity_locked();
    if (remember && !persist_locked()) {
      records_.erase(token_hash);
      return std::nullopt;
    }
    return created;
  }

  store_t::record_t *
  store_t::find_locked(const std::string_view token_hash, const std::string_view credential_fingerprint, const time_point_t at) {
    const auto found = records_.find(std::string { token_hash });
    if (found == records_.end()) {
      return nullptr;
    }

    auto &record = found->second;
    if (at >= record.absolute_expires_at || at >= record.idle_expires_at ||
        !constant_time_equal(record.credential_hash, credential_fingerprint)) {
      records_.erase(found);
      return nullptr;
    }
    return &record;
  }

  std::optional<session_t>
  store_t::lookup(const std::string_view token, const std::string_view credential_fingerprint) {
    const auto at = now();
    const auto token_hash = hash_hex(token);

    std::lock_guard lock(mutex_);
    auto *record = find_locked(token_hash, credential_fingerprint, at);
    if (!record) {
      return std::nullopt;
    }

    record->last_seen = at;
    record->idle_expires_at = at + (record->remember ? policy_.remember_idle : policy_.idle);
    if (record->remember && !persist_locked()) {
      return std::nullopt;
    }

    session_t session {
      .public_id = token_hash.substr(0, 32),
      .username = record->username,
      .created_at = record->created_at,
      .last_seen = record->last_seen,
      .expires_at = earliest(record->idle_expires_at, record->absolute_expires_at),
      .absolute_expires_at = record->absolute_expires_at,
      .remember = record->remember,
    };
    return session;
  }

  std::optional<resumed_session_t>
  store_t::resume(const std::string_view token, const std::string_view credential_fingerprint) {
    const auto at = now();
    const auto old_hash = hash_hex(token);
    const auto token_bytes = crypto::rand(32);
    const auto csrf_bytes = crypto::rand(32);
    if (token_bytes.size() != 32 || csrf_bytes.size() != 32) return std::nullopt;

    const auto new_token = util::hex_vec(token_bytes);
    const auto new_csrf = util::hex_vec(csrf_bytes);
    const auto new_hash = hash_hex(new_token);
    std::lock_guard lock(mutex_);
    auto found = records_.find(old_hash);
    if (found == records_.end()) return std::nullopt;
    if (!found->second.remember) return std::nullopt;
    if (at >= found->second.absolute_expires_at || at >= found->second.idle_expires_at ||
        !constant_time_equal(found->second.credential_hash, credential_fingerprint)) {
      records_.erase(found);
      persist_locked();
      return std::nullopt;
    }

    auto record = std::move(found->second);
    const auto old_csrf_hash = record.csrf_hash;
    const auto old_idle_expires_at = record.idle_expires_at;
    records_.erase(found);
    record.token_hash = new_hash;
    record.csrf_hash = hash_hex(new_csrf);
    record.last_seen = at;
    record.idle_expires_at = at + policy_.remember_idle;
    records_.emplace(new_hash, record);
    if (!persist_locked()) {
      records_.erase(new_hash);
      record.token_hash = old_hash;
      record.csrf_hash = old_csrf_hash;
      record.idle_expires_at = old_idle_expires_at;
      records_.emplace(old_hash, std::move(record));
      return std::nullopt;
    }

    resumed_session_t resumed;
    resumed.token = new_token;
    resumed.csrf_token = new_csrf;
    resumed.public_id = new_hash.substr(0, 32);
    resumed.username = record.username;
    resumed.created_at = record.created_at;
    resumed.last_seen = at;
    resumed.expires_at = earliest(record.idle_expires_at, record.absolute_expires_at);
    resumed.absolute_expires_at = record.absolute_expires_at;
    resumed.remember = true;
    return resumed;
  }

  bool
  store_t::csrf_matches(const std::string_view token, const std::string_view header, const std::string_view cookie) const {
    if (header.empty() || cookie.empty() || header.size() != cookie.size()) {
      return false;
    }
    if (!constant_time_equal(header, cookie)) {
      return false;
    }

    const auto at = now();
    const auto token_hash = hash_hex(token);
    std::lock_guard lock(mutex_);
    const auto found = records_.find(token_hash);
    if (found == records_.end() || at >= found->second.absolute_expires_at || at >= found->second.idle_expires_at) {
      return false;
    }
    return constant_time_equal(hash_hex(header), found->second.csrf_hash);
  }

  bool
  store_t::rekey(const std::string_view token, const std::string_view credential_fingerprint) {
    const auto at = now();
    const auto token_hash = hash_hex(token);
    std::lock_guard lock(mutex_);

    const auto found = records_.find(token_hash);
    if (found == records_.end() || at >= found->second.absolute_expires_at || at >= found->second.idle_expires_at) {
      return false;
    }
    found->second.credential_hash = credential_fingerprint;
    found->second.last_seen = at;
    found->second.idle_expires_at = at + (found->second.remember ? policy_.remember_idle : policy_.idle);
    if (found->second.remember && !persist_locked()) return false;
    return true;
  }

  bool
  store_t::revoke(const std::string_view token) {
    std::lock_guard lock(mutex_);
    const auto token_hash = hash_hex(token);
    const auto found = records_.find(token_hash);
    if (found == records_.end()) return false;
    auto record = std::move(found->second);
    records_.erase(found);
    if (!persist_locked()) {
      records_.emplace(token_hash, std::move(record));
      return false;
    }
    return true;
  }

  std::size_t
  store_t::revoke_all(const std::optional<std::string_view> keep) {
    std::lock_guard lock(mutex_);
    if (!keep) {
      const auto count = records_.size();
      auto previous = std::move(records_);
      records_.clear();
      if (!persist_locked()) {
        records_ = std::move(previous);
        return 0;
      }
      return count;
    }

    const auto keep_hash = hash_hex(*keep);
    auto previous = records_;
    std::size_t revoked = 0;
    for (auto it = records_.begin(); it != records_.end();) {
      if (constant_time_equal(it->first, keep_hash)) {
        ++it;
        continue;
      }
      it = records_.erase(it);
      ++revoked;
    }
    if (revoked && !persist_locked()) {
      records_ = std::move(previous);
      return 0;
    }
    return revoked;
  }

  bool
  store_t::revoke_public(const std::string_view public_id) {
    if (public_id.size() != 32) {
      return false;
    }

    std::lock_guard lock(mutex_);
    for (auto it = records_.begin(); it != records_.end(); ++it) {
      if (it->first.substr(0, 32) != public_id) {
        continue;
      }
      const auto token_hash = it->first;
      auto record = std::move(it->second);
      records_.erase(it);
      if (!persist_locked()) {
        records_.emplace(token_hash, std::move(record));
        return false;
      }
      return true;
    }
    return false;
  }

  std::vector<summary_t>
  store_t::list(const std::string_view current_token) {
    const auto at = now();
    const auto current_hash = hash_hex(current_token);

    std::lock_guard lock(mutex_);
    std::vector<summary_t> summaries;
    summaries.reserve(records_.size());
    bool changed = false;
    for (auto it = records_.begin(); it != records_.end();) {
      if (at >= it->second.absolute_expires_at || at >= it->second.idle_expires_at) {
        it = records_.erase(it);
        changed = true;
        continue;
      }
      summary_t summary;
      summary.public_id = it->first.substr(0, 32);
      summary.username = it->second.username;
      summary.created_at = it->second.created_at;
      summary.last_seen = it->second.last_seen;
      summary.expires_at = earliest(it->second.idle_expires_at, it->second.absolute_expires_at);
      summary.absolute_expires_at = it->second.absolute_expires_at;
      summary.remember = it->second.remember;
      summary.masked_address = it->second.masked_address;
      summary.short_user_agent = it->second.short_user_agent;
      summary.current = constant_time_equal(it->first, current_hash);
      summaries.push_back(std::move(summary));
      ++it;
    }
    std::sort(summaries.begin(), summaries.end(), [](const summary_t &left, const summary_t &right) {
      return left.last_seen > right.last_seen;
    });
    if (changed) persist_locked();
    return summaries;
  }

  std::size_t
  store_t::purge_expired() {
    const auto at = now();
    std::lock_guard lock(mutex_);
    std::size_t removed = 0;
    for (auto it = records_.begin(); it != records_.end();) {
      if (at >= it->second.absolute_expires_at || at >= it->second.idle_expires_at) {
        it = records_.erase(it);
        ++removed;
      }
      else {
        ++it;
      }
    }
    if (removed) persist_locked();
    return removed;
  }

  void
  store_t::clear(const bool persist) {
    std::lock_guard lock(mutex_);
    auto previous = std::move(records_);
    records_.clear();
    if (persist) {
      if (!persist_locked()) records_ = std::move(previous);
    }
    else {
      // The next configure call reloads the durable records, modelling a
      // process restart without deleting the test or production file.
      persistence_loaded_ = false;
    }
  }

  std::size_t
  store_t::size() const {
    std::lock_guard lock(mutex_);
    return records_.size();
  }

  void
  store_t::enforce_capacity_locked() {
    while (records_.size() > maximum_sessions) {
      auto oldest = records_.begin();
      for (auto it = records_.begin(); it != records_.end(); ++it) {
        if (it->second.last_seen < oldest->second.last_seen) {
          oldest = it;
        }
      }
      records_.erase(oldest);
    }
  }
}  // namespace confighttp::sessions

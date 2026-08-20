/**
 * @file src/nvhttp/tls_client_identity_store.h
 * @brief Connection-scoped authenticated client identity storage.
 */
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace nvhttp {
  /**
   * Keeps the authenticated client identity for the lifetime of a TLS
   * connection. HTTP keep-alive creates a new Request object for every request,
   * so request object addresses cannot be used as authentication state keys.
   */
  class tls_client_identity_store_t {
  public:
    void
    remember(std::string connection_key, std::weak_ptr<void> connection_lifetime, std::string client_uuid) {
      if (connection_key.empty() || connection_lifetime.expired() || client_uuid.empty()) {
        return;
      }

      std::lock_guard lock(_mutex);
      prune_expired_locked();
      _identities.insert_or_assign(
        std::move(connection_key),
        entry_t { std::move(connection_lifetime), std::move(client_uuid) });
    }

    std::string
    lookup(std::string_view connection_key) {
      if (connection_key.empty()) {
        return {};
      }

      std::lock_guard lock(_mutex);
      auto it = _identities.find(connection_key);
      if (it == _identities.end()) {
        return {};
      }
      if (it->second.connection_lifetime.expired()) {
        _identities.erase(it);
        return {};
      }
      return it->second.client_uuid;
    }

    void
    forget(std::string_view connection_key) {
      std::lock_guard lock(_mutex);
      if (!connection_key.empty()) {
        if (auto it = _identities.find(connection_key); it != _identities.end()) {
          _identities.erase(it);
        }
      }
      prune_expired_locked();
    }

  private:
    struct entry_t {
      std::weak_ptr<void> connection_lifetime;
      std::string client_uuid;
    };

    void
    prune_expired_locked() {
      std::erase_if(_identities, [](const auto &entry) {
        return entry.second.connection_lifetime.expired();
      });
    }

    std::mutex _mutex;
    std::map<std::string, entry_t, std::less<>> _identities;
  };
}  // namespace nvhttp

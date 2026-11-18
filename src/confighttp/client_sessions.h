/**
 * @file src/confighttp/client_sessions.h
 * @brief Ends the streaming sessions of every paired client before the pairing list is cleared.
 */
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace confighttp::clients {
  /**
   * @brief Calls @p terminate once for every client in the list returned by nvhttp::get_all_clients().
   *
   * Entries without a string uuid are skipped. Unpairing a single client ends that client's sessions,
   * so removing all clients has to do the same for each of them.
   * @return how many sessions were asked to end
   */
  template<class Terminate>
  std::size_t
  terminate_sessions_of_all(const nlohmann::json &clients, Terminate &&terminate) {
    std::size_t count = 0;
    if (!clients.is_array()) return count;
    for (const auto &client : clients) {
      if (!client.is_object()) continue;
      const auto uuid = client.find("uuid");
      if (uuid == client.end() || !uuid->is_string() || uuid->get_ref<const std::string &>().empty()) continue;
      terminate(uuid->get_ref<const std::string &>());
      ++count;
    }
    return count;
  }
}  // namespace confighttp::clients

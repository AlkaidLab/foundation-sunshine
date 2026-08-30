#include "service.h"
#include "easytier.h"

#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

#include <openssl/rand.h>

#include "src/config.h"
#include "src/logging.h"

namespace remote_connect {
  namespace {
    std::mutex service_mutex;
    easytier::runtime_t runtime;
    std::string last_error;

    std::string
    random_hex(std::size_t byte_count) {
      std::vector<unsigned char> bytes(byte_count);
      if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("Unable to generate secure random data");
      }

      static constexpr char alphabet[] = "0123456789abcdef";
      std::string result;
      result.reserve(byte_count * 2);
      for (auto value : bytes) {
        result.push_back(alphabet[value >> 4]);
        result.push_back(alphabet[value & 0x0f]);
      }
      return result;
    }

    bool
    ensure_config_locked() {
      const auto previous = enrollment_t {
        config::nvhttp.remote_connect_profile,
        config::nvhttp.remote_connect_virtual_ip,
        config::nvhttp.remote_connect_network_name,
        config::nvhttp.remote_connect_network_secret,
        config::nvhttp.remote_connect_peer,
      };

      std::map<std::string, std::string> updates;
      try {
        std::optional<std::string> seed;
        const auto get_seed = [&seed]() -> const std::string & {
          if (!seed) seed = random_hex(16);
          return *seed;
        };
        if (config::nvhttp.remote_connect_profile.empty()) {
          const auto &seed = get_seed();
          config::nvhttp.remote_connect_profile = "host-" + seed.substr(0, 16);
          updates["remote_connect_profile"] = config::nvhttp.remote_connect_profile;
        }
        if (config::nvhttp.remote_connect_virtual_ip.empty()) {
          const auto &seed = get_seed();
          const auto octet_a = 64 + (std::stoul(seed.substr(0, 2), nullptr, 16) % 64);
          const auto octet_b = 1 + (std::stoul(seed.substr(2, 2), nullptr, 16) % 253);
          config::nvhttp.remote_connect_virtual_ip =
            "10." + std::to_string(octet_a) + "." + std::to_string(octet_b) + ".1";
          updates["remote_connect_virtual_ip"] = config::nvhttp.remote_connect_virtual_ip;
        }
        if (config::nvhttp.remote_connect_network_name.empty()) {
          const auto &seed = get_seed();
          config::nvhttp.remote_connect_network_name = "remote-" + seed.substr(0, 20);
          updates["remote_connect_network_name"] = config::nvhttp.remote_connect_network_name;
        }
        if (config::nvhttp.remote_connect_network_secret.empty()) {
          config::nvhttp.remote_connect_network_secret = random_hex(32);
          updates["remote_connect_network_secret"] = config::nvhttp.remote_connect_network_secret;
        }
        if (config::nvhttp.remote_connect_peer.empty()) {
          config::nvhttp.remote_connect_peer = "udp://public.easytier.top:11010";
          updates["remote_connect_peer"] = config::nvhttp.remote_connect_peer;
        }

        if (updates.empty() || config::update_config(updates)) return true;
        last_error = "Unable to persist remote connection configuration";
      }
      catch (const std::exception &e) {
        last_error = e.what();
      }

      config::nvhttp.remote_connect_profile = previous.profile;
      config::nvhttp.remote_connect_virtual_ip = previous.virtual_ip;
      config::nvhttp.remote_connect_network_name = previous.network_name;
      config::nvhttp.remote_connect_network_secret = previous.network_secret;
      config::nvhttp.remote_connect_peer = previous.peer;
      return false;
    }

    status_t
    status_locked() {
      const bool running = runtime.running(last_error);
      const bool available = runtime.available();
      auto error = last_error;
      if (!available && error.empty()) {
        error = "The remote connection component is unavailable. Repair or reinstall Foundation Sunshine.";
      }
      return {
        config::nvhttp.remote_connect_enabled,
        running,
        available,
        config::nvhttp.remote_connect_virtual_ip,
        std::move(error),
      };
    }

    bool
    start_locked() {
      if (!ensure_config_locked()) return false;
      const bool started = runtime.start(enrollment_t {
                                           config::nvhttp.remote_connect_profile,
                                           config::nvhttp.remote_connect_virtual_ip,
                                           config::nvhttp.remote_connect_network_name,
                                           config::nvhttp.remote_connect_network_secret,
                                           config::nvhttp.remote_connect_peer,
                                         },
                                         config::nvhttp.sunshine_name,
                                         last_error);
      if (started) last_error.clear();
      return started;
    }

    void
    stop_locked() {
      runtime.stop();
      last_error.clear();
    }
  }  // namespace

  status_t
  status() {
    std::lock_guard lock(service_mutex);
    return status_locked();
  }

  enrollment_t
  enrollment() {
    std::lock_guard lock(service_mutex);
    return {
      config::nvhttp.remote_connect_profile,
      config::nvhttp.remote_connect_virtual_ip,
      config::nvhttp.remote_connect_network_name,
      config::nvhttp.remote_connect_network_secret,
      config::nvhttp.remote_connect_peer,
    };
  }

  bool
  start() {
    std::lock_guard lock(service_mutex);
    return start_locked();
  }

  void
  stop() {
    std::lock_guard lock(service_mutex);
    stop_locked();
  }

  void
  start_if_enabled() {
    std::lock_guard lock(service_mutex);
    if (config::nvhttp.remote_connect_enabled && !start_locked()) {
      BOOST_LOG(warning) << "Remote connection failed to start: " << last_error;
    }
  }

  operation_result_t
  set_enabled(bool enabled) {
    std::lock_guard lock(service_mutex);
    const bool previous = config::nvhttp.remote_connect_enabled;

    if (enabled) {
      if (!start_locked()) return {false, status_locked()};
    }
    else {
      stop_locked();
    }

    if (previous != enabled) {
      config::nvhttp.remote_connect_enabled = enabled;
      if (!config::update_config({{"remote_connect_enabled", enabled ? "true" : "false"}})) {
        config::nvhttp.remote_connect_enabled = previous;
        if (!previous) {
          stop_locked();
        }
        else {
          start_locked();
        }
        last_error = "Unable to persist remote connection setting";
        return {false, status_locked()};
      }
    }

    return {true, status_locked()};
  }

}  // namespace remote_connect

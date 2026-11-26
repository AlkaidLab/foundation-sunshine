#include "service.h"
#include "device_authorization.h"
#include "device_record_store.h"
#include "easytier.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

#include <openssl/rand.h>

#include "src/config.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/platform/common.h"

namespace remote_connect {
  namespace {
    std::mutex service_mutex;
    easytier::runtime_t runtime;
    std::string last_error;

    // The authorized client keys are a projection of the device record: the
    // EasyTier public keys the core is told to authorize when it next starts,
    // and the ones the running core was started with. They are set only by the
    // authorizer's apply effect, so they cannot drift from the record.
    std::vector<std::string> authorized_client_keys;

    std::mutex adoption_mutex;
    // The device record that has been read and recovered, and from where. It
    // is adopted once per location, before the core is first started.
    std::filesystem::path adopted_record_path;
    bool devices_adopted = false;

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

    std::string
    random_virtual_ip() {
      for (int attempt = 0; attempt < 64; ++attempt) {
        const auto seed = random_hex(2);
        const auto octet_b = 64 + (std::stoul(seed.substr(0, 2), nullptr, 16) % 64);
        const auto octet_c = std::stoul(seed.substr(2, 2), nullptr, 16);
        const auto candidate = "100." + std::to_string(octet_b) + "." + std::to_string(octet_c) + ".1";
        if (!easytier::virtual_subnet_conflicts(candidate)) return candidate;
      }
      throw std::runtime_error("Unable to find a remote virtual subnet that does not overlap an active local network");
    }

    enrollment_t
    current_enrollment() {
      return {
        config::nvhttp.remote_connect_profile,
        config::nvhttp.remote_connect_virtual_ip,
        config::nvhttp.remote_connect_network_name,
        config::nvhttp.remote_connect_network_secret,
        config::nvhttp.remote_connect_peer,
      };
    }

    void
    apply_enrollment(const enrollment_t &value) {
      config::nvhttp.remote_connect_profile = value.profile;
      config::nvhttp.remote_connect_virtual_ip = value.virtual_ip;
      config::nvhttp.remote_connect_network_name = value.network_name;
      config::nvhttp.remote_connect_network_secret = value.network_secret;
      config::nvhttp.remote_connect_peer = value.peer;
    }

    std::map<std::string, std::string>
    enrollment_updates(const enrollment_t &value) {
      return {
        {"remote_connect_profile", value.profile},
        {"remote_connect_virtual_ip", value.virtual_ip},
        {"remote_connect_network_name", value.network_name},
        {"remote_connect_network_secret", value.network_secret},
        {"remote_connect_peer", value.peer},
      };
    }

    bool
    ensure_config_locked() {
      const auto previous = current_enrollment();

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
          config::nvhttp.remote_connect_virtual_ip = random_virtual_ip();
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
          config::nvhttp.remote_connect_peer = default_peer;
          updates["remote_connect_peer"] = config::nvhttp.remote_connect_peer;
        }

        if (updates.empty() || config::update_config(updates)) return true;
        last_error = "Unable to persist remote connection configuration";
      }
      catch (const std::exception &e) {
        last_error = e.what();
      }

      apply_enrollment(previous);
      return false;
    }

    status_t
    status_locked() {
      const bool running = runtime.running(last_error);
      const bool available = runtime.available();
      auto error = last_error;
      if (!available && error.empty()) {
        error = easytier::installation_required_error;
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
      const bool started = runtime.start(current_enrollment(), authorized_client_keys, config::nvhttp.sunshine_name, last_error);
      if (started) last_error.clear();
      return started;
    }

    void
    stop_locked() {
      runtime.stop();
      last_error.clear();
    }

    // The core is told who is authorized only when it starts, so putting a
    // changed authorized set in force means a restart, which interrupts every
    // current remote session on this host.
    bool
    realign_runtime_locked() {
      runtime.stop();
      const bool started = start_locked();
      if (!started) runtime.stop();
      return started;
    }

    std::filesystem::path
    device_record_path() {
      return platf::appdata() / "remote-devices.json";
    }

    // Lock order: the authorizer's lock is taken before the service lock, by
    // the apply effect below. Nothing that holds the service lock may call
    // into the authorizer, or the two would deadlock.
    device_authorization_effects_t
    authorization_effects() {
      device_authorization_effects_t effects;
      effects.persist = [](const credential_transaction::record_t &record) {
        return device_record_store::store(device_record_path(), record);
      };
      effects.apply = [](const std::vector<std::string> &keys) {
        std::lock_guard lock(service_mutex);
        authorized_client_keys = keys;
        std::string ignored;
        // Not running: the next start picks the set up.
        if (!runtime.running(ignored)) return true;
        return realign_runtime_locked();
      };
      effects.next_transaction_id = []() -> std::optional<credential_transaction::transaction_id_t> {
        credential_transaction::transaction_id_t id {};
        // A zero id is what reduce() treats as "no transaction".
        if (RAND_bytes(id.data(), static_cast<int>(id.size())) != 1 ||
            std::all_of(id.begin(), id.end(), [](std::uint8_t byte) { return byte == 0; })) {
          return std::nullopt;
        }
        return id;
      };
      return effects;
    }

    // The session that takes enrollments. No endpoint issues invitations yet.
    enrollment_session_t enrollment_session;
    std::unique_ptr<device_authorizer_t> authorizer = std::make_unique<device_authorizer_t>(enrollment_session, authorization_effects());

    // Reads the device record and recovers one a crash left mid-transaction.
    // It has to run before the service lock is taken: adopting a record
    // realigns the runtime, and that takes the service lock.
    bool
    ensure_devices_adopted() {
      std::lock_guard lock(adoption_mutex);
      const auto path = device_record_path();
      if (devices_adopted && adopted_record_path == path) return true;

      const auto loaded = device_record_store::load(path);
      const auto refuse = [&path](const std::string &message) {
        {
          std::lock_guard lock(service_mutex);
          last_error = message;
        }
        BOOST_LOG(error) << message;
        return false;
      };

      // Neither of these may be treated as "no devices". Starting from an
      // empty record would authorize nobody, which is safe by itself, but the
      // next enrollment would then overwrite a file that may still hold this
      // host's devices.
      switch (loaded.status) {
        case device_record_store::load_status_e::invalid:
          return refuse("The remote device record is not valid, so remote access stays off to keep it from being overwritten: " + file_handler::path_to_utf8(path));
        case device_record_store::load_status_e::unreadable:
          return refuse("The remote device record could not be read, so remote access stays off: " + file_handler::path_to_utf8(path));
        case device_record_store::load_status_e::fresh:
        case device_record_store::load_status_e::loaded:
          break;
      }

      const auto adopted = authorizer->adopt(loaded.record);
      if (!adopted.success) return refuse(adopted.error);
      adopted_record_path = path;
      devices_adopted = true;
      return true;
    }
  }  // namespace

  status_t
  status() {
    std::lock_guard lock(service_mutex);
    return status_locked();
  }

  pairing_state_t
  prepare_pairing() {
    if (!ensure_devices_adopted()) {
      std::lock_guard lock(service_mutex);
      return { false, config::nvhttp.remote_connect_enabled, std::nullopt, last_error };
    }
    std::lock_guard lock(service_mutex);
    if (!config::nvhttp.remote_connect_enabled) {
      return { true, false, std::nullopt, {} };
    }
    if (!start_locked()) {
      return { false, true, std::nullopt, last_error };
    }
    return { true, true, current_enrollment(), {} };
  }

  void
  stop() {
    std::lock_guard lock(service_mutex);
    stop_locked();
  }

  void
  start_if_enabled() {
    if (!ensure_devices_adopted()) {
      std::string message;
      {
        std::lock_guard lock(service_mutex);
        message = last_error;
      }
      BOOST_LOG(warning) << "Remote connection failed to start: " << message;
      return;
    }
    std::lock_guard lock(service_mutex);
    if (config::nvhttp.remote_connect_enabled && !start_locked()) {
      BOOST_LOG(warning) << "Remote connection failed to start: " << last_error;
    }
  }

  operation_result_t
  set_enabled(bool enabled) {
    if (enabled && !ensure_devices_adopted()) {
      std::lock_guard lock(service_mutex);
      return {false, status_locked()};
    }
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

  operation_result_t
  reset_enrollment() {
    // This can start the core, so it is held to the same rule as the other
    // entry points: no core is started until the device record is adopted.
    if (!ensure_devices_adopted()) {
      std::lock_guard lock(service_mutex);
      return {false, status_locked()};
    }
    std::lock_guard lock(service_mutex);
    const auto previous = current_enrollment();

    try {
      const auto seed = random_hex(16);
      enrollment_t replacement {
        "host-" + seed.substr(0, 16),
        random_virtual_ip(),
        "remote-" + seed.substr(0, 20),
        random_hex(32),
        previous.peer.empty() ? default_peer : previous.peer,
      };

      runtime.stop();
      apply_enrollment(replacement);
      if (!config::update_config(enrollment_updates(replacement))) {
        apply_enrollment(previous);
        if (config::nvhttp.remote_connect_enabled) start_locked();
        last_error = "Unable to persist new remote access credentials";
        return {false, status_locked()};
      }

      last_error.clear();
      if (config::nvhttp.remote_connect_enabled && !start_locked()) {
        const auto replacement_error = last_error;
        runtime.stop();
        apply_enrollment(previous);
        if (!config::update_config(enrollment_updates(previous))) {
          last_error = replacement_error + "; unable to persist the previous credentials during rollback";
          return {false, status_locked()};
        }

        last_error.clear();
        if (!start_locked()) {
          const auto rollback_error = last_error;
          last_error = replacement_error + "; previous credentials were restored, but their runtime also failed to start: " + rollback_error;
          return {false, status_locked()};
        }
        last_error = replacement_error + "; previous credentials and runtime were restored";
        return {false, status_locked()};
      }
      return {true, status_locked()};
    }
    catch (const std::exception &e) {
      apply_enrollment(previous);
      last_error = e.what();
      return {false, status_locked()};
    }
  }

  std::vector<credential_transaction::device_t>
  devices() {
    if (!ensure_devices_adopted()) return {};
    return authorizer->devices();
  }

  operation_result_t
  revoke_device(const std::string &device_id) {
    if (!ensure_devices_adopted()) {
      std::lock_guard lock(service_mutex);
      return {false, status_locked()};
    }
    // Holds the authorizer's lock; its apply effect takes the service lock
    // to restart the core with the device's key removed.
    const auto revoked = authorizer->revoke(device_id);
    std::lock_guard lock(service_mutex);
    if (!revoked.success) {
      last_error = revoked.error;
      BOOST_LOG(error) << "Remote device was not revoked: " << revoked.error;
    }
    return {revoked.success, status_locked()};
  }
}  // namespace remote_connect

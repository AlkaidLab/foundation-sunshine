/**
 * @file src/platform/linux/niri_ipc.cpp
 * @brief niri availability gate, IPC socket discovery and bounded runner.
 */
#include "niri_ipc.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <vector>

#include "src/display_device/vdd_utils.h"
#include "src/logging.h"

#include <nlohmann/json.hpp>

namespace platf::niri {
  namespace {
    std::atomic<std::chrono::steady_clock::rep> probe_after { 0 };
    std::atomic<bool> unavailability_logged { false };

    /// A socket that answered once keeps being used; a failed request clears it
    /// so the next call looks again (niri restarts with a new socket name).
    std::mutex socket_mutex;
    std::string cached_socket;
    std::chrono::steady_clock::time_point search_after {};

    /// At most this many sockets are probed per search: a crashed instance
    /// leaves its socket behind, and each probe costs a full kProbeTimeout.
    constexpr std::size_t kMaxSocketCandidates = 4;

    std::chrono::steady_clock::time_point
    now() {
      return std::chrono::steady_clock::now();
    }

    std::string
    env(const char *name) {
      const char *value = ::getenv(name);
      return value ? std::string { value } : std::string {};
    }

    bool
    mentions(const std::string &value, const char *needle) {
      std::string lowered;
      lowered.reserve(value.size());
      for (const char c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }
      return lowered.find(needle) != std::string::npos;
    }

    /// One request against an explicit socket. Kept separate from run() so the
    /// socket search cannot recurse into itself.
    exec_result_t
    run_with_socket(const std::string &socket, const std::string &args, std::chrono::milliseconds timeout) {
      // vdd_utils::run_logged bounds the child (niri msg blocks on a wedged or
      // stale socket) and keeps inherited descriptors out of it. The socket is
      // passed through the child's environment because that is the only way
      // niri msg learns which instance to talk to; it is a path under the
      // user's runtime directory, so quoting it is enough.
      const std::string command = (socket.empty() ? std::string {} : "NIRI_SOCKET='" + socket + "' ") + "niri msg " + args;
      const auto result = display_device::vdd_utils::run_logged(command, timeout);
      return { result.exit_code, result.output };
    }

    std::vector<std::filesystem::path>
    candidate_sockets() {
      std::vector<std::filesystem::path> candidates;

      std::vector<std::filesystem::path> dirs;
      if (const auto runtime = env("XDG_RUNTIME_DIR"); !runtime.empty()) {
        dirs.emplace_back(runtime);
      }
      // Sunshine runs with file capabilities, so AT_SECURE makes
      // secure_getenv() hide XDG_RUNTIME_DIR; a service started by the user
      // manager still finds the socket here.
      const std::filesystem::path run_user = std::filesystem::path { "/run/user" } / std::to_string(::getuid());
      if (std::find(dirs.begin(), dirs.end(), run_user) == dirs.end()) {
        dirs.push_back(run_user);
      }

      for (const auto &dir : dirs) {
        std::error_code ec;
        std::filesystem::directory_iterator it { dir, ec };
        if (ec) {
          continue;
        }
        for (const auto &entry : it) {
          const auto name = entry.path().filename().string();
          // niri names its socket "niri.<wayland display>.<pid>.sock".
          if (name.rfind("niri.", 0) != 0 || entry.path().extension() != ".sock") {
            continue;
          }
          candidates.push_back(entry.path());
        }
      }

      // Newest first: the running session's socket is the most recent one, and
      // a leftover from a crashed instance only costs a probe.
      std::sort(candidates.begin(), candidates.end(), [](const auto &left, const auto &right) {
        std::error_code left_ec, right_ec;
        const auto left_time = std::filesystem::last_write_time(left, left_ec);
        const auto right_time = std::filesystem::last_write_time(right, right_ec);
        if (left_ec || right_ec) {
          return left.string() > right.string();
        }
        return left_time > right_time;
      });
      if (candidates.size() > kMaxSocketCandidates) {
        candidates.resize(kMaxSocketCandidates);
      }
      return candidates;
    }

    std::string
    discover_socket() {
      std::lock_guard lock { socket_mutex };
      if (!cached_socket.empty()) {
        return cached_socket;
      }

      const auto current = now();
      if (current < search_after) {
        // A search is already running or just failed; do not repeat it.
        return {};
      }
      search_after = current + kRetryCooldown;

      for (const auto &candidate : candidate_sockets()) {
        const auto result = run_with_socket(candidate.string(), "--json version", kProbeTimeout);
        if (result.exit_code == 0) {
          cached_socket = candidate.string();
          BOOST_LOG(debug) << "niri: using IPC socket " << cached_socket;
          return cached_socket;
        }
      }
      return {};
    }
  }  // namespace

  bool
  session_supports_niri() {
    // niri exports this to everything it starts; it is authoritative even when
    // XDG_CURRENT_DESKTOP claims KDE for toolkit compatibility.
    if (!env("NIRI_SOCKET").empty()) {
      return true;
    }

    const auto desktop = env("XDG_CURRENT_DESKTOP");
    if (mentions(desktop, "kde") || mentions(desktop, "plasma")) {
      // Plasma: there is no niri to find, and scanning for one would only add a
      // directory walk to every display query.
      return false;
    }
    if (mentions(desktop, "niri")) {
      return true;
    }

    // Neither variable is conclusive (service started at boot, or a session
    // that does not set them): a live socket under the runtime directory is
    // the remaining observable. Bounded by the search cooldown.
    return !discover_socket().empty();
  }

  bool
  probe_allowed() {
    return now().time_since_epoch().count() >= probe_after.load(std::memory_order_relaxed);
  }

  void
  note_unavailable(const std::string &reason) {
    probe_after.store((now() + kRetryCooldown).time_since_epoch().count(), std::memory_order_relaxed);
    if (!unavailability_logged.exchange(true)) {
      BOOST_LOG(info) << "niri output management unavailable (" << reason
                      << "); the session keeps niri's own layout until it answers";
    }
  }

  void
  note_available() {
    probe_after.store(0, std::memory_order_relaxed);
  }

  exec_result_t
  run(const std::string &args, std::chrono::milliseconds timeout) {
    std::string socket = env("NIRI_SOCKET");
    if (socket.empty()) {
      socket = discover_socket();
    }

    auto result = run_with_socket(socket, args, timeout);
    if (result.exit_code != 0 && !socket.empty() && env("NIRI_SOCKET").empty()) {
      // The cached socket may belong to a compositor that has since exited;
      // drop it so the next call searches again.
      std::lock_guard lock { socket_mutex };
      cached_socket.clear();
      search_after = {};
    }
    return result;
  }

  std::string
  socket_path() {
    const auto from_env = env("NIRI_SOCKET");
    if (!from_env.empty()) {
      return from_env;
    }
    return discover_socket();
  }

  std::vector<output_t>
  parse_outputs(std::string_view json, std::string *error) {
    std::vector<output_t> outputs;

    nlohmann::json parsed;
    try {
      parsed = nlohmann::json::parse(json.begin(), json.end());
    }
    catch (const nlohmann::json::exception &err) {
      if (error) {
        *error = err.what();
      }
      return outputs;
    }
    if (!parsed.is_object()) {
      if (error) {
        *error = "the payload is not an object";
      }
      return outputs;
    }

    for (const auto &[key, value] : parsed.items()) {
      if (!value.is_object()) {
        continue;
      }

      output_t output;
      output.name = value.value("name", key);

      if (const auto modes = value.find("modes"); modes != value.end() && modes->is_array()) {
        for (const auto &entry : *modes) {
          if (!entry.is_object()) {
            continue;
          }
          mode_t mode;
          mode.width = entry.value("width", 0u);
          mode.height = entry.value("height", 0u);
          // niri reports millihertz; the rest of the layer works in Hz.
          mode.refresh = static_cast<double>(entry.value("refresh_rate", 0u)) / 1000.0;
          mode.preferred = entry.value("is_preferred", false);
          output.modes.push_back(mode);
        }
      }

      // An index outside the advertised list (or a null) means the output is
      // off; niri never reports an active mode it did not also list.
      if (const auto current = value.find("current_mode"); current != value.end() && current->is_number_unsigned()) {
        const auto position = current->get<std::size_t>();
        if (position < output.modes.size()) {
          output.current_mode = position;
        }
      }

      outputs.push_back(std::move(output));
    }
    return outputs;
  }
}  // namespace platf::niri

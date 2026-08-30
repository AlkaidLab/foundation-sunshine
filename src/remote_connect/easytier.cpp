#include "easytier.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

#include <boost/filesystem/path.hpp>
#include <boost/process/v1.hpp>

#include "src/file_handler.h"
#include "src/platform/common.h"
#include "src/platform/run_command.h"
#include "src/process.h"

using namespace std::literals;

namespace remote_connect::easytier {
  namespace {
    namespace fs = std::filesystem;

    fs::path
    config_path() {
      return platf::appdata() / "remote-connect.toml";
    }

    fs::path
    find_core() {
#ifdef _WIN32
      constexpr auto executable_name = "easytier-core.exe";
#else
      constexpr auto executable_name = "easytier-core";
#endif
      const std::array candidates {
        platf::appdata().parent_path() / "tools" / "easytier" / executable_name,
        platf::appdata().parent_path() / executable_name,
        platf::appdata() / executable_name,
      };
      for (const auto &candidate : candidates) {
        if (fs::is_regular_file(candidate)) return candidate;
      }

      const auto searched = boost::process::v1::search_path(executable_name);
      return searched.empty() ? fs::path {} : fs::path {searched.string()};
    }

    std::string
    toml_escape(const std::string &value) {
      std::string escaped;
      escaped.reserve(value.size());
      for (const auto ch : value) {
        if (ch == '\\' || ch == '"') escaped.push_back('\\');
        if (ch == '\n') {
          escaped += "\\n";
        }
        else if (ch != '\r') {
          escaped.push_back(ch);
        }
      }
      return escaped;
    }

    bool
    write_config(const enrollment_t &enrollment, const std::string &hostname) {
      std::ostringstream toml;
      toml << "instance_name = \"Sunshine Remote\"\n"
           << "hostname = \"" << toml_escape(hostname) << "\"\n"
           << "ipv4 = \"" << enrollment.virtual_ip << "/24\"\n"
           << "dhcp = false\n"
           << "listeners = [\"tcp://0.0.0.0:11010\", \"udp://0.0.0.0:11010\"]\n"
           << "rpc_portal = \"127.0.0.1:0\"\n\n"
           << "[network_identity]\n"
           << "network_name = \"" << toml_escape(enrollment.network_name) << "\"\n"
           << "network_secret = \"" << toml_escape(enrollment.network_secret) << "\"\n\n"
           << "[[peer]]\n"
           << "uri = \"" << toml_escape(enrollment.peer) << "\"\n\n"
           << "[flags]\n"
           << "latency_first = true\n";
      return file_handler::write_file(file_handler::path_to_utf8(config_path()).c_str(), toml.str()) == 0;
    }
  }  // namespace

  struct runtime_t::state_t {
    std::optional<boost::process::v1::child> process;
    std::optional<boost::process::v1::group> process_group;
  };

  runtime_t::runtime_t():
      state_(std::make_unique<state_t>()) {
  }

  runtime_t::~runtime_t() {
    stop();
  }

  bool
  runtime_t::available() const {
    return !find_core().empty();
  }

  bool
  runtime_t::running(std::string &error) {
    if (!state_->process || !state_->process->valid()) return false;

    std::error_code error_code;
    const bool is_running = state_->process->running(error_code);
    if (error_code) {
      error = error_code.message();
      return false;
    }
    if (!is_running && error.empty()) {
      error = "Remote connection stopped unexpectedly";
    }
    return is_running;
  }

  bool
  runtime_t::start(const enrollment_t &enrollment, const std::string &hostname, std::string &error) {
    if (running(error)) return true;

    state_->process.reset();
    state_->process_group.reset();
    error.clear();

    if (!write_config(enrollment, hostname)) {
      error = "Unable to write EasyTier configuration";
      return false;
    }

    const auto executable = find_core();
    if (executable.empty()) {
      error = "The remote connection component is unavailable. Repair or reinstall Foundation Sunshine.";
      return false;
    }

    const auto executable_utf8 = file_handler::path_to_utf8(executable);
    const auto config_utf8 = file_handler::path_to_utf8(config_path());
    const std::string command = "\"" + executable_utf8 + "\" --config-file \"" + config_utf8 + "\"";
    boost::filesystem::path working_dir(executable.parent_path().string());
    auto environment = boost::this_process::environment();
    std::error_code error_code;
    state_->process_group.emplace();
    auto child = platf::run_command(
      true, false, command, working_dir, environment, nullptr, error_code, &*state_->process_group
    );
    if (error_code || !child.valid()) {
      error = error_code ? error_code.message() : "Unable to start EasyTier core";
      state_->process_group.reset();
      return false;
    }

    state_->process.emplace(std::move(child));
    return true;
  }

  void
  runtime_t::stop() {
    std::string ignored_error;
    if (running(ignored_error)) {
      if (state_->process_group) {
        proc::terminate_process_group(*state_->process, *state_->process_group, 5s);
      }
      else {
        state_->process->terminate();
      }
    }
    state_->process.reset();
    state_->process_group.reset();
  }

}  // namespace remote_connect::easytier

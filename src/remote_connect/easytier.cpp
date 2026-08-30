#include "easytier.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/asio/ip/address_v4.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/process/v1.hpp>

#include <openssl/evp.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <iphlpapi.h>
#endif

#include "src/crypto.h"
#include "src/file_handler.h"
#include "src/network.h"
#include "src/nvhttp.h"
#include "src/platform/common.h"
#include "src/platform/run_command.h"
#include "src/process.h"
#include "src/rtsp.h"
#include "src/stream.h"

using namespace std::literals;

namespace remote_connect::easytier {
  namespace {
    namespace fs = std::filesystem;

    fs::path
    config_path() {
      return platf::appdata() / "remote-connect.toml";
    }

#ifdef _WIN32
    struct runtime_component_t {
      const char *filename;
      const char *expected_sha256;
    };

    std::optional<std::string>
    calculate_sha256(const fs::path &path) {
      crypto::md_ctx_t context {EVP_MD_CTX_create()};
      if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) return std::nullopt;

      std::ifstream file(path, std::ios::binary);
      if (!file) return std::nullopt;
      char buffer[16 * 1024];
      while (file.good()) {
        file.read(buffer, sizeof(buffer));
        if (EVP_DigestUpdate(context.get(), buffer, static_cast<std::size_t>(file.gcount())) != 1) {
          return std::nullopt;
        }
      }
      if (!file.eof()) return std::nullopt;

      unsigned char digest[EVP_MAX_MD_SIZE];
      unsigned int digest_size = 0;
      if (EVP_DigestFinal_ex(context.get(), digest, &digest_size) != 1) return std::nullopt;

      std::ostringstream encoded;
      encoded << std::hex << std::setfill('0');
      for (unsigned int index = 0; index < digest_size; ++index) {
        encoded << std::setw(2) << static_cast<unsigned int>(digest[index]);
      }
      return encoded.str();
    }

    fs::path
    find_core() {
#if defined(EASYTIER_EASYTIER_CORE_EXE_SHA256) && defined(EASYTIER_PACKET_DLL_SHA256) && \
    defined(EASYTIER_WINDIVERT64_SYS_SHA256) && defined(EASYTIER_WINTUN_DLL_SHA256)
      const auto runtime_dir = platf::appdata().parent_path() / "tools" / "easytier";
      constexpr runtime_component_t components[] = {
        {"easytier-core.exe", EASYTIER_EASYTIER_CORE_EXE_SHA256},
        {"Packet.dll", EASYTIER_PACKET_DLL_SHA256},
        {"WinDivert64.sys", EASYTIER_WINDIVERT64_SYS_SHA256},
        {"wintun.dll", EASYTIER_WINTUN_DLL_SHA256},
      };
      for (const auto &component : components) {
        const auto path = runtime_dir / component.filename;
        const auto digest = calculate_sha256(path);
        if (!digest || *digest != component.expected_sha256) return {};
      }
      return runtime_dir / "easytier-core.exe";
#else
      return {};
#endif
    }
#else
    fs::path
    find_core() {
      return {};
    }
#endif

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
           << "listeners = [\"tcp://0.0.0.0:0\", \"udp://0.0.0.0:0\"]\n"
           << "rpc_portal = \"127.0.0.1:0\"\n\n"
           << "exit_nodes = []\n"
           << "routes = []\n"
           << "proxy_network = []\n"
           << "tcp_whitelist = [\"" << net::map_port(nvhttp::PORT_HTTPS) << "\", \""
           << net::map_port(nvhttp::PORT_HTTP) << "\", \""
           << net::map_port(rtsp_stream::RTSP_SETUP_PORT) << "\"]\n"
           << "udp_whitelist = [\"" << net::map_port(stream::VIDEO_STREAM_PORT) << "\", \""
           << net::map_port(stream::CONTROL_PORT) << "\", \""
           << net::map_port(stream::AUDIO_STREAM_PORT) << "\", \""
           << net::map_port(stream::MIC_STREAM_PORT) << "\"]\n\n"
           << "[network_identity]\n"
           << "network_name = \"" << toml_escape(enrollment.network_name) << "\"\n"
           << "network_secret = \"" << toml_escape(enrollment.network_secret) << "\"\n\n"
           << "[[peer]]\n"
           << "uri = \"" << toml_escape(enrollment.peer) << "\"\n\n"
           << "[flags]\n"
           << "latency_first = true\n"
           << "enable_ipv6 = false\n"
           << "enable_exit_node = false\n"
           << "proxy_forward_by_system = false\n"
           << "accept_dns = false\n"
           << "relay_network_whitelist = \"\"\n";
      return file_handler::write_file(file_handler::path_to_utf8(config_path()).c_str(), toml.str()) == 0;
    }
  }  // namespace

  bool
  virtual_subnet_conflicts(const std::string &virtual_ip) {
#ifdef _WIN32
    boost::system::error_code parse_error;
    const auto address = boost::asio::ip::make_address_v4(virtual_ip, parse_error);
    if (parse_error) return true;

    constexpr std::uint32_t target_mask = 0xffffff00u;
    const auto target_start = address.to_uint() & target_mask;
    const auto target_end = target_start | ~target_mask;

    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    auto adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    auto result = GetAdaptersAddresses(
      AF_INET,
      GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
      nullptr,
      adapters,
      &size
    );
    if (result == ERROR_BUFFER_OVERFLOW) {
      buffer.resize(size);
      adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
      result = GetAdaptersAddresses(
        AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr,
        adapters,
        &size
      );
    }
    if (result != NO_ERROR) return true;

    for (auto adapter = adapters; adapter; adapter = adapter->Next) {
      if (adapter->OperStatus != IfOperStatusUp) continue;
      for (auto unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
        if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET ||
            unicast->OnLinkPrefixLength > 32) {
          continue;
        }
        const auto sockaddr = reinterpret_cast<const sockaddr_in *>(unicast->Address.lpSockaddr);
        const auto local = ntohl(sockaddr->sin_addr.s_addr);
        const auto prefix = static_cast<unsigned int>(unicast->OnLinkPrefixLength);
        const auto mask = prefix == 0 ? 0u : 0xffffffffu << (32 - prefix);
        const auto local_start = local & mask;
        const auto local_end = local_start | ~mask;
        if (target_start <= local_end && local_start <= target_end) return true;
      }
    }
#else
    (void) virtual_ip;
#endif
    return false;
  }

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

    if (virtual_subnet_conflicts(enrollment.virtual_ip)) {
      error = "The remote virtual subnet overlaps an active local network. Reset remote access to choose a safe address.";
      return false;
    }

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

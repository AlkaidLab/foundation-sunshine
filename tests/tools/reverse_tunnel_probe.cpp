// Standalone driver for the production tunnel service. The synthetic helper
// waits for a reply across TLS before completing attach, reproducing usbip-win2.
#include "src/remote_usb/reverse_tunnel_service.h"
#include "src/logging_severity.h"

#include <boost/asio.hpp>
#include <openssl/pem.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> warning;

int main(int argc, char **argv) {
  if (argc != 9) {
    std::cerr << "usage: probe cert key peer-cert token-file port usbip-or-synthetic stop-file mode\n";
    return 2;
  }
  using namespace std::chrono_literals;
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  remote_usb::usbip_host_controller_config host;
  host.executable = argv[6];
  host.backend = remote_usb::usbip_host_backend::usbip_win2;
  asio::io_context helper_io;
  std::shared_ptr<tcp::socket> helper_socket;
  const std::string mode = argv[8];
  if (host.executable == "synthetic") {
    host.command_runner = [&](const std::string &, const std::vector<std::string> &args,
                              std::chrono::milliseconds timeout,
                              const std::shared_ptr<std::atomic_bool> &cancel, std::size_t) {
      remote_usb::usbip_command_result result;
      if (args.front() == "detach") {
        helper_socket.reset();
        std::cout << "DETACHED" << std::endl;
        result.exit_code = 0;
        return result;
      }
      auto socket = std::make_shared<tcp::socket>(helper_io);
      socket->connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), std::stoi(args[1])));
      const std::string request("\0IMPORT\n", 8);
      asio::write(*socket, asio::buffer(request));
      socket->non_blocking(true);
      std::string reply;
      const auto end = std::chrono::steady_clock::now() + timeout;
      while (reply.size() < 8 && std::chrono::steady_clock::now() < end && !cancel->load()) {
        char bytes[8];
        boost::system::error_code error;
        const auto n = socket->read_some(asio::buffer(bytes, 8 - reply.size()), error);
        if (!error) reply.append(bytes, n);
        else if (error != asio::error::would_block && error != asio::error::try_again) break;
        std::this_thread::sleep_for(1ms);
      }
      if (reply != std::string("REPLY\0\r\n", 8)) {
        result.cancelled = cancel->load();
        result.standard_error = "helper did not receive its import reply across TLS";
        return result;
      }
      std::cout << "IMPORT_EXCHANGED" << std::endl;
      if (mode == "fail-after-import") return result;
      helper_socket = std::move(socket);
      result.exit_code = 0;
      result.standard_output = "7\n";
      return result;
    };
  }
  remote_usb::reverse_tunnel_config config;
  config.bind_address = "127.0.0.1";
  config.port = static_cast<std::uint16_t>(std::stoi(argv[5]));
  config.certificate_file = argv[1];
  config.private_key_file = argv[2];
  std::ifstream token(argv[4]);
  std::getline(token, config.session_token);
  std::ifstream peer_file(argv[3]);
  const std::string pem((std::istreambuf_iterator<char>(peer_file)), {});
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
  std::unique_ptr<X509, decltype(&X509_free)> peer(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free);
  if (!peer) return 2;
  config.verify_client_cert = [&](X509 *cert) { return X509_cmp(cert, peer.get()) == 0; };
  if (mode == "limited") config.max_sessions = 2;
  if (mode == "invalid-address") config.bind_address = "invalid-address";
  if (mode == "missing-verifier") config.verify_client_cert = {};
  if (mode == "zero-limit") config.max_sessions = 0;
  remote_usb::reverse_tunnel_service service(std::move(host));
  if (!service.start(std::move(config))) return 1;
  std::cout << "LISTENING " << service.bound_port() << std::endl;
  const auto deadline = std::chrono::steady_clock::now() + 120s;
  while (!std::filesystem::exists(argv[7]) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  service.stop();
  std::cout << "STOPPED" << std::endl;
}

/**
 * @file tests/tools/ai_beast_pilot.cpp
 * @brief Manually runnable loopback TLS pilot using the real curl AI proxy.
 */
#include "src/ai/beast_server.h"
#include "src/ai/credential_store.h"
#include <atomic>
#include <boost/log/sources/severity_logger.hpp>
#include <charconv>
#include <csignal>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <thread>

boost::log::sources::severity_logger<int> verbose, debug, info, warning, error, fatal;
namespace {
  nlohmann::json ai_configuration;
  std::atomic_bool interrupted { false };
  static_assert(std::atomic_bool::is_always_lock_free);
  struct CurlRuntime {
    CurlRuntime() {
      if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("Could not initialize curl");
    }
    ~CurlRuntime() { curl_global_cleanup(); }
  };
  unsigned
  number(std::string_view value, unsigned maximum) {
    unsigned result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() || result > maximum) throw std::invalid_argument("Invalid numeric pilot option");
    return result;
  }
  std::filesystem::path
  utf8_path(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
  }
  void
  interrupt(int) { interrupted.store(true, std::memory_order_relaxed); }
  std::string
  read(const std::string &path) {
    std::ifstream file(utf8_path(path), std::ios::binary);
    if (!file) throw std::runtime_error("Could not read the specified input file");
    if (std::filesystem::file_size(utf8_path(path)) > 1024 * 1024) throw std::invalid_argument("Pilot input file exceeds 1 MiB");
    return std::string(std::istreambuf_iterator<char>(file), {});
  }
}  // namespace
namespace ai {
  // The executable keeps configuration immutable. It reads existing secure
  // credentials but does not migrate or write Sunshine's production files.
  nlohmann::json
  loadAiConfig() { return ai_configuration; }
}  // namespace ai

int
main(int argc, char **argv) {
  if (argc < 5 || argc > 8 || std::string_view(argv[1]) == "--help") {
    std::cout << "Usage: ai_beast_pilot <cert.pem> <key.pem> <token-file> <ai_config.json> [port] [client-ca.pem] [duration-seconds]\n"
                 "Binds 127.0.0.1 only. POST /api/ai/chat/completions with stream=true. Press Ctrl+C to stop.\n";
    return argc < 2 || std::string_view(argv[1]) == "--help" ? 0 : 2;
  }
  try {
    ai::beast_http::Options options;
    options.certificate_file = argv[1];
    options.private_key_file = argv[2];
    if (argc > 5) {
      const auto port = number(argv[5], 65535);
      options.port = static_cast<unsigned short>(port);
    }
    if (argc > 6) options.client_ca_file = argv[6];
    auto token = read(argv[3]);
    while (!token.empty() && (token.back() == '\r' || token.back() == '\n')) token.pop_back();
    if (token.size() < 16 || token.size() > 4096 || token.find_first_of("\r\n") != std::string::npos) {
      throw std::invalid_argument("Token file must contain one token of 16-4096 bytes");
    }
    try {
      ai_configuration = nlohmann::json::parse(read(argv[4]));
    }
    catch (const nlohmann::json::exception &) {
      throw std::invalid_argument("Invalid JSON in AI configuration");
    }
    if (!ai_configuration.is_object()) throw std::invalid_argument("AI config must be a JSON object");
    const auto credential = credential_store::read_llm_api_key(utf8_path(argv[4]).parent_path() / "ai_llm_credential.bin");
    if (credential.status == credential_store::read_status_e::error) throw std::runtime_error("Secure AI credential could not be loaded");
    if (credential.status == credential_store::read_status_e::success) ai_configuration["apiKey"] = credential.secret;
    const std::string expected = "Bearer " + token;
    CurlRuntime curl;
    ai::beast_http::Server server(options, [expected](const auto &request, const auto &) {
      const auto received = request[boost::beast::http::field::authorization];
      return received.size() == expected.size() && CRYPTO_memcmp(received.data(), expected.data(), expected.size()) == 0; }, ai::processAiChatStream);
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    const auto duration = argc > 7 ? number(argv[7], 3600) : 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
    const auto port = server.start();
    std::cout << "Beast AI/SSE pilot: https://127.0.0.1:" << port << "/api/ai/chat/completions\n"
              << std::flush;
    while (!interrupted && (!duration || std::chrono::steady_clock::now() < deadline)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server.stop();
    return 0;
  }
  catch (const std::exception &exception) {
    std::cerr << "Beast pilot startup failed: " << exception.what() << '\n';
    return 1;
  }
}

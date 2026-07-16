/**
 * @file src/httpcommon.cpp
 * @brief Definitions for common HTTP.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "process.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <utility>
#include <vector>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include <cstring>

#include <boost/asio/ssl/context.hpp>

#include <Simple-Web-Server/server_http.hpp>
#include <Simple-Web-Server/server_https.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <curl/curl.h>

#include "config.h"
#include "crypto.h"
#include "file_handler.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "rtsp.h"
#include "utility.h"
#include "uuid.h"

namespace http {
  using namespace std::literals;
  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  int
  reload_user_creds(const std::string &file);
  bool
  user_creds_exist(const std::string &file);

  std::string unique_id;
  net::net_e origin_web_ui_allowed;

  int
  init() {
    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];
    origin_web_ui_allowed = net::from_enum_string(config::nvhttp.origin_web_ui_allowed);

    if (clean_slate) {
      unique_id = uuid_util::uuid_t::generate().string();
      auto dir = std::filesystem::temp_directory_path() / "Sunshine"sv;
      config::nvhttp.cert = (dir / ("cert-"s + unique_id)).string();
      config::nvhttp.pkey = (dir / ("pkey-"s + unique_id)).string();
    }

    if ((!fs::exists(config::nvhttp.pkey) || !fs::exists(config::nvhttp.cert)) &&
        create_creds(config::nvhttp.pkey, config::nvhttp.cert)) {
      return -1;
    }
    if (!user_creds_exist(config::sunshine.credentials_file)) {
      BOOST_LOG(info) << "Open the Web UI to set your new username and password and getting started";
    } else if (reload_user_creds(config::sunshine.credentials_file)) {
      return -1;
    }
    return 0;
  }

  int
  save_user_creds(const std::string &file, const std::string &username, const std::string &password, bool run_our_mouth) {
    pt::ptree outputTree;

    if (fs::exists(file)) {
      try {
        pt::read_json(file, outputTree);
      }
      catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't read user credentials: "sv << e.what();
        return -1;
      }
    }

    auto salt = crypto::rand_alphabet(16);
    outputTree.put("username", username);
    outputTree.put("salt", salt);
    outputTree.put("password", util::hex(crypto::hash(password + salt)).to_string());
    try {
      pt::write_json(file, outputTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(error) << "error writing to the credentials file, perhaps try this again as an administrator? Details: "sv << e.what();
      return -1;
    }

    BOOST_LOG(info) << "New credentials have been created"sv;
    return 0;
  }

  bool
  user_creds_exist(const std::string &file) {
    if (!fs::exists(file)) {
      return false;
    }

    pt::ptree inputTree;
    try {
      pt::read_json(file, inputTree);
      return inputTree.find("username") != inputTree.not_found() &&
             inputTree.find("password") != inputTree.not_found() &&
             inputTree.find("salt") != inputTree.not_found();
    }
    catch (std::exception &e) {
      BOOST_LOG(error) << "validating user credentials: "sv << e.what();
    }

    return false;
  }

  int
  reload_user_creds(const std::string &file) {
    pt::ptree inputTree;
    try {
      pt::read_json(file, inputTree);
      config::sunshine.username = inputTree.get<std::string>("username");
      config::sunshine.password = inputTree.get<std::string>("password");
      config::sunshine.salt = inputTree.get<std::string>("salt");
    }
    catch (std::exception &e) {
      BOOST_LOG(error) << "loading user credentials: "sv << e.what();
      return -1;
    }
    return 0;
  }

  int
  create_creds(const std::string &pkey, const std::string &cert) {
    fs::path pkey_path = pkey;
    fs::path cert_path = cert;

    auto creds = crypto::gen_creds("Sunshine Gamestream Host"sv, 2048);

    auto pkey_dir = pkey_path;
    auto cert_dir = cert_path;
    pkey_dir.remove_filename();
    cert_dir.remove_filename();

    std::error_code err_code {};
    fs::create_directories(pkey_dir, err_code);
    if (err_code) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << pkey_dir << "] :"sv << err_code.message();
      return -1;
    }

    fs::create_directories(cert_dir, err_code);
    if (err_code) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << cert_dir << "] :"sv << err_code.message();
      return -1;
    }

    if (file_handler::write_file(pkey.c_str(), creds.pkey)) {
      BOOST_LOG(error) << "Couldn't open ["sv << config::nvhttp.pkey << ']';
      return -1;
    }

    if (file_handler::write_file(cert.c_str(), creds.x509)) {
      BOOST_LOG(error) << "Couldn't open ["sv << config::nvhttp.cert << ']';
      return -1;
    }

    fs::permissions(pkey_path,
      fs::perms::owner_read | fs::perms::owner_write,
      fs::perm_options::replace, err_code);

    if (err_code) {
      BOOST_LOG(error) << "Couldn't change permissions of ["sv << config::nvhttp.pkey << "] :"sv << err_code.message();
      return -1;
    }

    fs::permissions(cert_path,
      fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read | fs::perms::owner_write,
      fs::perm_options::replace, err_code);

    if (err_code) {
      BOOST_LOG(error) << "Couldn't change permissions of ["sv << config::nvhttp.cert << "] :"sv << err_code.message();
      return -1;
    }

    return 0;
  }

  bool download_file(const std::string &url, const std::string &file, long ssl_version) {
    BOOST_LOG(info) << "Downloading external resource: " << url;
    // sonar complains about weak ssl and tls versions; however sonar cannot detect the fix
    CURL *curl = curl_easy_init();  // NOSONAR
    if (!curl) {
      BOOST_LOG(error) << "Couldn't create CURL instance ["sv << url << ']';
      return false;
    }

    if (std::string file_dir = file_handler::get_parent_directory(file); !file_handler::make_directory(file_dir)) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << file_dir << "] for ["sv << url << ']';
      curl_easy_cleanup(curl);
      return false;
    }

    FILE *fp = fopen(file.c_str(), "wb");
    if (!fp) {
      BOOST_LOG(error) << "Couldn't open ["sv << file << "] for ["sv << url << ']';
      curl_easy_cleanup(curl);
      return false;
    }

    curl_easy_setopt(curl, CURLOPT_SSLVERSION, ssl_version);  // NOSONAR
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

    // Security limits
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)10 * 1024 * 1024); // 10MB limit
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L); // Disable 302 redirects

    long response_code = 0;
    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    if (result != CURLE_OK || response_code != 200) {
      if (result != CURLE_OK) {
        BOOST_LOG(error) << "Couldn't download ["sv << url << ", code:" << result << ']';
      } else {
        BOOST_LOG(error) << "Download failed: HTTP " << response_code << " [" << url << "]";
      }
      // Force result to fail state if we got a non-200 response code
      result = (result == CURLE_OK) ? CURLE_HTTP_RETURNED_ERROR : result;
    }

    curl_easy_cleanup(curl);
    fclose(fp);
    if (result != CURLE_OK) {
        // Cleanup partial file
        if (fs::exists(file)) {
            boost::system::error_code ec;
            fs::remove(file, ec); // Don't crash if delete fails
        }
    }
    return result == CURLE_OK;
  }

  size_t string_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    auto *str = static_cast<std::string*>(userp);
    
    // Safety check: Don't allow string to grow beyond strict limits
    if (str->size() + realsize > 10 * 1024 * 1024) {
      BOOST_LOG(error) << "Fetch URL: memory limit exceeded";
      return 0;
    }
    
    str->append(static_cast<char*>(contents), realsize);
    return realsize;
  }

  bool fetch_url(const std::string &url, std::string &content, long ssl_version) {
    BOOST_LOG(info) << "Fetching external resource: " << url;
    CURL *curl = curl_easy_init();
    if (!curl) {
      BOOST_LOG(error) << "Couldn't create CURL instance ["sv << url << ']';
      return false;
    }

    content.clear();
    // Reserve some memory to reduce reallocations
    content.reserve(4096);

    curl_easy_setopt(curl, CURLOPT_SSLVERSION, ssl_version);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, string_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &content);

    // Security limits
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)10 * 1024 * 1024); // 10MB limit
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    long response_code = 0;
    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_easy_cleanup(curl);

    if (result != CURLE_OK || response_code != 200) {
      if (result != CURLE_OK) {
        BOOST_LOG(error) << "Couldn't fetch ["sv << url << ", code:" << result << ']';
      } else {
        BOOST_LOG(error) << "Fetch failed: HTTP " << response_code << " [" << url << "]";
      }
      return false;
    }

    return true;
  }

  bool post_json(const std::string &url, const std::string &body, const std::map<std::string, std::string> &headers, std::string &response_body, long &http_code, long timeout_seconds) {
    BOOST_LOG(info) << "POST JSON to: " << url;
    CURL *curl = curl_easy_init();
    if (!curl) {
      BOOST_LOG(error) << "Couldn't create CURL instance for POST ["sv << url << ']';
      return false;
    }

    response_body.clear();
    response_body.reserve(4096);

    // Build custom headers
    struct curl_slist *header_list = nullptr;
    header_list = curl_slist_append(header_list, "Content-Type: application/json");
    for (const auto &[key, value] : headers) {
      std::string header_line = key + ": " + value;
      header_list = curl_slist_append(header_list, header_line.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, string_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    // Security & timeout
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)10 * 1024 * 1024);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
      BOOST_LOG(error) << "POST failed ["sv << url << ", curl code:" << result << ']';
      return false;
    }

    return true;
  }

  std::string url_escape(const std::string &url) {
    char *string = curl_easy_escape(nullptr, url.c_str(), static_cast<int>(url.length()));
    std::string result(string);
    curl_free(string);
    return result;
  }

  std::string
  url_get_host(const std::string &url) {
    CURLU *curlu = curl_url();
    curl_url_set(curlu, CURLUPART_URL, url.c_str(), static_cast<unsigned int>(url.length()));
    char *host;
    if (curl_url_get(curlu, CURLUPART_HOST, &host, 0) != CURLUE_OK) {
      curl_url_cleanup(curlu);
      return "";
    }
    std::string result(host);
    curl_free(host);
    curl_url_cleanup(curlu);
    return result;
  }
}  // namespace http

namespace {
  constexpr auto COVER_DOWNLOAD_MAX_BYTES = static_cast<std::size_t>(10 * 1024 * 1024);
  constexpr auto COVER_DNS_TIMEOUT = std::chrono::seconds(10);

  struct ParsedDownloadUrl {
    std::string scheme;
    std::string host;
    std::string port;
  };

  struct PublicHostResolution {
    bool is_address_literal = false;
    std::vector<std::string> addresses;
  };

  std::string to_lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return value;
  }

  std::string normalize_host(std::string host) {
    host = to_lower_ascii(std::move(host));
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
      host = host.substr(1, host.size() - 2);
    }
    while (!host.empty() && host.back() == '.') {
      host.pop_back();
    }
    return host;
  }

  bool curl_url_get_string(CURLU *curlu, CURLUPart part, std::string &value, unsigned int flags = 0) {
    char *raw = nullptr;
    if (curl_url_get(curlu, part, &raw, flags) != CURLUE_OK) {
      return false;
    }

    value = raw;
    curl_free(raw);
    return true;
  }

  bool parse_download_url(const std::string &url, ParsedDownloadUrl &parts) {
    CURLU *curlu = curl_url();
    if (!curlu) {
      return false;
    }

    if (curl_url_set(curlu, CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) {
      curl_url_cleanup(curlu);
      return false;
    }

    const bool ok = curl_url_get_string(curlu, CURLUPART_SCHEME, parts.scheme) &&
                    curl_url_get_string(curlu, CURLUPART_HOST, parts.host);
    curl_url_get_string(curlu, CURLUPART_PORT, parts.port);
    curl_url_cleanup(curlu);

    if (!ok) {
      return false;
    }

    parts.scheme = to_lower_ascii(parts.scheme);
    parts.host = normalize_host(std::move(parts.host));
    return true;
  }

  bool is_private_ipv4(std::uint32_t address) {
    return address == 0xffffffffu ||
           (address & 0xff000000u) == 0x00000000u ||
           (address & 0xff000000u) == 0x0a000000u ||
           (address & 0xff000000u) == 0x7f000000u ||
           (address & 0xffc00000u) == 0x64400000u ||
           (address & 0xffff0000u) == 0xa9fe0000u ||
           (address & 0xfff00000u) == 0xac100000u ||
           (address & 0xffff0000u) == 0xc0a80000u ||
           (address & 0xffffff00u) == 0xc0000000u ||
           (address & 0xffffff00u) == 0xc0000200u ||
           (address & 0xffffff00u) == 0xc0586300u ||
           (address & 0xffff0000u) == 0xc6120000u ||
           (address & 0xffffff00u) == 0xc6336400u ||
           (address & 0xffffff00u) == 0xcb007100u ||
           (address & 0xf0000000u) == 0xe0000000u ||
           (address & 0xf0000000u) == 0xf0000000u;
  }

  bool is_local_or_private_address(const boost::asio::ip::address &address) {
    if (address.is_unspecified() || address.is_loopback() || address.is_multicast()) {
      return true;
    }

    if (address.is_v4()) {
      return is_private_ipv4(address.to_v4().to_uint());
    }

    const auto bytes = address.to_v6().to_bytes();
    const bool is_v4_mapped = std::all_of(bytes.begin(), bytes.begin() + 10, [](unsigned char value) { return value == 0; }) &&
                              bytes[10] == 0xff &&
                              bytes[11] == 0xff;
    if (is_v4_mapped) {
      return true;
    }

    const auto has_prefix = [&bytes](std::initializer_list<unsigned char> prefix) {
      return std::equal(prefix.begin(), prefix.end(), bytes.begin());
    };
    const auto is_zero_range = [&bytes](std::size_t begin, std::size_t end) {
      return std::all_of(bytes.begin() + begin, bytes.begin() + end, [](unsigned char value) { return value == 0; });
    };

    const bool is_v4_compatible = is_zero_range(0, 12);
    const bool is_nat64_well_known = has_prefix({0x00, 0x64, 0xff, 0x9b}) && is_zero_range(4, 12); // 64:ff9b::/96
    const bool is_nat64_local_use = has_prefix({0x00, 0x64, 0xff, 0x9b, 0x00, 0x01}); // 64:ff9b:1::/48
    const bool is_discard_only = has_prefix({0x01, 0x00}) && is_zero_range(2, 8); // 100::/64
    const bool is_dummy_prefix = has_prefix({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}); // 100:0:0:1::/64
    const bool is_ietf_protocol_assignment = has_prefix({0x20, 0x01}) && (bytes[2] & 0xfe) == 0x00; // 2001::/23
    const bool is_orchid_deprecated = has_prefix({0x20, 0x01, 0x00}) && (bytes[3] & 0xf0) == 0x10; // 2001:10::/28
    const bool is_orchid_v2 = has_prefix({0x20, 0x01, 0x00}) && (bytes[3] & 0xf0) == 0x20; // 2001:20::/28
    const bool is_documentation_2001 = has_prefix({0x20, 0x01, 0x0d, 0xb8}); // 2001:db8::/32
    const bool is_6to4 = has_prefix({0x20, 0x02}); // 2002::/16
    const bool is_documentation_3fff = has_prefix({0x3f, 0xff}) && (bytes[2] & 0xf0) == 0x00; // 3fff::/20
    const bool is_srv6_sid = has_prefix({0x5f, 0x00}); // 5f00::/16
    const bool is_unique_local = (bytes[0] & 0xfe) == 0xfc; // fc00::/7
    const bool is_link_or_site_local = bytes[0] == 0xfe && (bytes[1] & 0xc0) != 0x00; // fe80::/10 and deprecated fec0::/10
    const bool is_current_global_unicast = (bytes[0] & 0xe0) == 0x20; // 2000::/3

    return !is_current_global_unicast ||
           is_v4_compatible ||
           is_nat64_well_known ||
           is_nat64_local_use ||
           is_discard_only ||
           is_dummy_prefix ||
           is_ietf_protocol_assignment ||
           is_orchid_deprecated ||
           is_orchid_v2 ||
           is_documentation_2001 ||
           is_6to4 ||
           is_documentation_3fff ||
           is_srv6_sid ||
           is_unique_local ||
           is_link_or_site_local;
  }

  bool resolve_public_host(const std::string &host, PublicHostResolution &resolution) {
    if (host.empty() || host == "localhost" || host.ends_with(".localhost")) {
      return false;
    }

    boost::system::error_code ec;
    const auto literal = boost::asio::ip::make_address(host, ec);
    if (!ec) {
      if (is_local_or_private_address(literal)) {
        return false;
      }
      resolution.is_address_literal = true;
      return true;
    }

    boost::asio::io_context io;
    boost::asio::ip::tcp::resolver resolver(io);
    boost::asio::steady_timer timer(io);
    boost::asio::ip::tcp::resolver::results_type results;
    bool timed_out = false;

    resolver.async_resolve(host, "443", [&](const boost::system::error_code &resolve_ec, const boost::asio::ip::tcp::resolver::results_type &resolve_results) {
      ec = resolve_ec;
      results = resolve_results;
      timer.cancel();
    });
    timer.expires_after(COVER_DNS_TIMEOUT);
    timer.async_wait([&](const boost::system::error_code &timer_ec) {
      if (!timer_ec) {
        timed_out = true;
        resolver.cancel();
      }
    });
    io.run();

    if (timed_out) {
      BOOST_LOG(warning) << "Cover host resolution timed out ["sv << host << ']';
      return false;
    }
    if (ec || results.empty()) {
      BOOST_LOG(warning) << "Cover host resolution failed or returned no addresses ["sv << host << ']';
      return false;
    }

    for (const auto &result : results) {
      const auto address = result.endpoint().address();
      if (is_local_or_private_address(address)) {
        BOOST_LOG(warning) << "Cover host resolved to a blocked address ["sv << host << " -> " << address.to_string() << ']';
        return false;
      }
      resolution.addresses.push_back(address.to_string());
    }

    return !resolution.addresses.empty();
  }

  bool resolve_public_https_cover_url(const std::string &url, ParsedDownloadUrl &parts, PublicHostResolution &resolution) {
    if (!parse_download_url(url, parts)) {
      return false;
    }

    if (parts.scheme != "https" || parts.host.empty()) {
      return false;
    }

    if (!parts.port.empty() && parts.port != "443") {
      return false;
    }

    return resolve_public_host(parts.host, resolution);
  }

  struct ImageCheckContext {
    std::string filename;
    std::string url;
    FILE *fp = nullptr;
    unsigned char buffer[12]; // Buffer for magic bytes
    size_t buffer_len = 0;
    size_t bytes_received = 0;
    bool checked = false;
    bool valid = false;

    // Ensure buffer is large enough for our checks to avoid overflow
    static_assert(sizeof(buffer) >= 12, "Image check buffer too small");
  };

  bool has_supported_image_magic(const unsigned char *magic) {
    return (magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47) || // PNG
           (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) || // JPG
           (magic[0] == 0x42 && magic[1] == 0x4D) || // BMP
           (memcmp(magic, "RIFF", 4) == 0 && memcmp(magic + 8, "WEBP", 4) == 0) || // WEBP
           (magic[0] == 0x00 && magic[1] == 0x00 && magic[2] == 0x01 && magic[3] == 0x00); // ICO
  }

  bool write_image_data(ImageCheckContext &ctx, const void *data, size_t length) {
    if (length == 0) {
      return true;
    }

    if (fwrite(data, 1, length, ctx.fp) != length) {
      BOOST_LOG(error) << "Couldn't write image data to ["sv << ctx.filename << ']';
      return false;
    }
    return true;
  }

  size_t image_write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    try {
      if (!ptr || !userdata) {
        return 0;
      }

      // Check for overflow in size calculation
      if (size > 0 && nmemb > SIZE_MAX / size) {
        auto *ctx = static_cast<ImageCheckContext *>(userdata);
        BOOST_LOG(error) << "Image check size overflow ["sv << (ctx ? ctx->url : "unknown") << ']';
        return 0;
      }

      auto *ctx = static_cast<ImageCheckContext *>(userdata);
      size_t total_size = size * nmemb;
      if (total_size == 0) {
        return 0;
      }
      if (ctx->bytes_received > COVER_DOWNLOAD_MAX_BYTES || total_size > COVER_DOWNLOAD_MAX_BYTES - ctx->bytes_received) {
        BOOST_LOG(warning) << "Image download exceeded "sv << COVER_DOWNLOAD_MAX_BYTES << " bytes [" << ctx->url << ']';
        return 0;
      }
      ctx->bytes_received += total_size;

      const unsigned char *data = static_cast<const unsigned char *>(ptr);

      // If not yet checked, accumulating bytes
      if (!ctx->checked) {
        size_t needed = sizeof(ctx->buffer) - ctx->buffer_len;
        size_t to_copy = std::min(needed, total_size);

        memcpy(ctx->buffer + ctx->buffer_len, data, to_copy);
        ctx->buffer_len += to_copy;

        // Have we accumulated enough?
        if (ctx->buffer_len == sizeof(ctx->buffer)) {
          ctx->checked = true;
          ctx->valid = has_supported_image_magic(ctx->buffer);

          if (!ctx->valid) {
            BOOST_LOG(warning) << "Streaming validation failed: Invalid magic bytes ["sv << ctx->url << ']';
            return 0; // Stop download
          }

          // Check passed, open file
          ctx->fp = fopen(ctx->filename.c_str(), "wb");
          if (!ctx->fp) {
            BOOST_LOG(error) << "Couldn't open ["sv << ctx->filename << "] for ["sv << ctx->url << ']';
            return 0;
          }

          // Flush buffer to file
          if (!write_image_data(*ctx, ctx->buffer, ctx->buffer_len)) {
            return 0;
          }
        }
        
        // If we have leftovers in this chunk that weren't part of the buffer fill
        if (total_size > to_copy) {
          if (ctx->valid && ctx->fp) {
            const size_t remaining = total_size - to_copy;
            if (!write_image_data(*ctx, data + to_copy, remaining)) {
              return 0;
            }
          } else if (!ctx->valid && ctx->checked) {
             // Should have returned 0 above, but just in case logic flows here
             return 0;
          }
        }
      } else {
        // Already checked and valid, just write
        if (ctx->valid && ctx->fp) {
          if (!write_image_data(*ctx, ptr, total_size)) {
            return 0;
          }
        } else {
          return 0;
        }
      }

      return total_size;
    } catch (...) {
      BOOST_LOG(error) << "Exception in image_write_callback";
      return 0;
    }
  }

  std::string format_curl_resolve_address(const std::string &address) {
    boost::system::error_code ec;
    const auto parsed = boost::asio::ip::make_address(address, ec);
    if (!ec && parsed.is_v6()) {
      return "["s + address + "]";
    }
    return address;
  }

  curl_slist *build_curl_resolve_list(const ParsedDownloadUrl &parts, const std::string &address) {
    std::string entry = parts.host + ":443:" + format_curl_resolve_address(address);
    return curl_slist_append(nullptr, entry.c_str());
  }

  bool perform_image_download_with_magic_check(const std::string &url, const std::string &file, long ssl_version, const ParsedDownloadUrl *resolved_parts = nullptr, const std::string *resolved_address = nullptr) {
    BOOST_LOG(info) << "Downloading external image with magic check: " << url;
    CURL *curl = curl_easy_init();
    if (!curl) {
      BOOST_LOG(error) << "Couldn't create CURL instance ["sv << url << ']';
      return false;
    }

    curl_slist *resolve_list = nullptr;
    if (resolved_parts && resolved_address) {
      resolve_list = build_curl_resolve_list(*resolved_parts, *resolved_address);
      if (!resolve_list) {
        BOOST_LOG(error) << "Couldn't pin resolved cover host addresses ["sv << url << ']';
        curl_easy_cleanup(curl);
        return false;
      }
      curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve_list);
    }

    if (std::string file_dir = file_handler::get_parent_directory(file); !file_handler::make_directory(file_dir)) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << file_dir << "] for ["sv << url << ']';
      curl_slist_free_all(resolve_list);
      curl_easy_cleanup(curl);
      return false;
    }

    ImageCheckContext ctx;
    ctx.filename = file;
    ctx.url = url;

    curl_easy_setopt(curl, CURLOPT_SSLVERSION, ssl_version);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, image_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    // Security limits
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(COVER_DOWNLOAD_MAX_BYTES));

    // Disable redirects
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    // Timeouts
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    long response_code = 0;
    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    if (ctx.fp) {
      fclose(ctx.fp);
    }

    curl_slist_free_all(resolve_list);
    curl_easy_cleanup(curl);

    bool http_ok = (response_code == 200);

    if (result != CURLE_OK || !http_ok) {
      if (result != CURLE_OK) {
        BOOST_LOG(error) << "Download failed or rejected ["sv << url << ", code:" << result << ']';
      } else {
        BOOST_LOG(error) << "Download failed: HTTP " << response_code << " [" << url << "]";
      }

      // Cleanup partial file if it exists (though usually it shouldn't be much)
      if (boost::filesystem::exists(file)) {
        boost::system::error_code remove_ec;
        boost::filesystem::remove(file, remove_ec);
      }
      return false;
    }

    // Double check: if download finished but we never got enough bytes to check?
    // Treat as failure (empty or too small file)
    if (!ctx.checked) {
      BOOST_LOG(warning) << "Download too small to validate magic bytes ["sv << url << ']';
      // Cleanup if file was created
      if (boost::filesystem::exists(file)) {
        boost::system::error_code remove_ec;
        boost::filesystem::remove(file, remove_ec);
      }
      return false;
    }

    return true;
  }
}

namespace http {
  bool download_image_with_magic_check(const std::string &url, const std::string &file, long ssl_version) {
    return perform_image_download_with_magic_check(url, file, ssl_version);
  }

  bool download_public_cover_image(const std::string &url, const std::string &file, long ssl_version) {
    ParsedDownloadUrl parts;
    PublicHostResolution resolution;
    if (!resolve_public_https_cover_url(url, parts, resolution)) {
      BOOST_LOG(warning) << "Blocked cover download from non-public HTTPS URL ["sv << url << ']';
      return false;
    }

    if (resolution.is_address_literal) {
      return perform_image_download_with_magic_check(url, file, ssl_version);
    }

    for (const auto &address : resolution.addresses) {
      if (perform_image_download_with_magic_check(url, file, ssl_version, &parts, &address)) {
        return true;
      }
    }

    return false;
  }
}


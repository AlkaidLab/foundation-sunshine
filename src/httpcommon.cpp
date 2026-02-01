/**
 * @file src/httpcommon.cpp
 * @brief Definitions for common HTTP.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "process.h"

#include <filesystem>
#include <utility>

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
  struct ImageCheckContext {
    std::string filename;
    std::string url;
    FILE *fp = nullptr;
    unsigned char buffer[12]; // Buffer for magic bytes
    size_t buffer_len = 0;
    bool checked = false;
    bool valid = false;

    // Ensure buffer is large enough for our checks to avoid overflow
    static_assert(sizeof(buffer) >= 12, "Image check buffer too small");
  };

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
          unsigned char *magic = ctx->buffer;

          // Perform Magic Byte Check
          // PNG: 89 50 4E 47
          if (magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47) ctx->valid = true;
          // JPG: FF D8 FF
          else if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) ctx->valid = true;
          // BMP: 42 4D
          else if (magic[0] == 0x42 && magic[1] == 0x4D) ctx->valid = true;
          // WEBP: RIFF ... WEBP
          else if (memcmp(magic, "RIFF", 4) == 0 && memcmp(magic + 8, "WEBP", 4) == 0) ctx->valid = true;
          // ICO: 00 00 01 00
          else if (magic[0] == 0x00 && magic[1] == 0x00 && magic[2] == 0x01 && magic[3] == 0x00) ctx->valid = true;

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
          fwrite(ctx->buffer, 1, ctx->buffer_len, ctx->fp);
        }
        
        // If we have leftovers in this chunk that weren't part of the buffer fill
        if (total_size > to_copy) {
          if (ctx->valid && ctx->fp) {
            fwrite(data + to_copy, 1, total_size - to_copy, ctx->fp);
          } else if (!ctx->valid && ctx->checked) {
             // Should have returned 0 above, but just in case logic flows here
             return 0;
          }
        }
      } else {
        // Already checked and valid, just write
        if (ctx->valid && ctx->fp) {
          fwrite(ptr, size, nmemb, ctx->fp);
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
}

namespace http {
  bool download_image_with_magic_check(const std::string &url, const std::string &file, long ssl_version) {
    BOOST_LOG(info) << "Downloading external image with magic check: " << url;
    CURL *curl = curl_easy_init();
    if (!curl) {
      BOOST_LOG(error) << "Couldn't create CURL instance ["sv << url << ']';
      return false;
    }

    if (std::string file_dir = file_handler::get_parent_directory(file); !file_handler::make_directory(file_dir)) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << file_dir << "] for ["sv << url << ']';
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
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)10 * 1024 * 1024); // 10MB limit

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
        boost::system::error_code ec;
        boost::filesystem::remove(file, ec);
      }
      return false;
    }

    // Double check: if download finished but we never got enough bytes to check?
    // Treat as failure (empty or too small file)
    if (!ctx.checked) {
       BOOST_LOG(warning) << "Download too small to validate magic bytes ["sv << url << ']';
       // Cleanup if file was created
       if (boost::filesystem::exists(file)) {
         boost::system::error_code ec;
         boost::filesystem::remove(file, ec);
       }
       return false;
    }

    return true;
  }
}


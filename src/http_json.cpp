/**
 * @file src/http_json.cpp
 * @brief JSON POST transport reusable without host initialization.
 */
#include "httpcommon.h"
#include "logging.h"
#include <curl/curl.h>

using namespace std::literals;
namespace http {
  namespace {
    size_t
    string_write_callback(void *contents, size_t size, size_t nmemb, void *userp) noexcept {
      try {
        constexpr size_t limit = 10 * 1024 * 1024;
        auto &body = *static_cast<std::string *>(userp);
        if (size && nmemb > limit / size) return 0;
        const size_t bytes = size * nmemb;
        if (body.size() > limit || bytes > limit - body.size()) return 0;
        body.append(static_cast<char *>(contents), bytes);
        return bytes;
      }
      catch (...) {
        return 0;
      }
    }
  }  // namespace

  bool
  post_json(const std::string &url, const std::string &body, const std::map<std::string, std::string> &headers, std::string &response_body, long &http_code, long timeout_seconds) {
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
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t) 10 * 1024 * 1024);
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

}  // namespace http

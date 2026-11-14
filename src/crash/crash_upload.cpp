/**
 * @file src/crash/crash_upload.cpp
 * @brief Uploads the pending crash reports to POST <endpoint>/v1/crash (multipart: "meta" JSON + optional "dump").
 *
 * Runs on its own thread and only ever waits on the network, never on anything the host needs to start. A report is deleted after a 2xx and
 * after a 4xx the server will never accept (except 408/429); a network failure or 5xx leaves it for the next pass. At most 20 stay on disk.
 */
#include "crash_state.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <curl/curl.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <unistd.h>
#endif

namespace crash {
  namespace fs = std::filesystem;
  using namespace detail;

  namespace {
    std::thread g_uploader;
    std::atomic<bool> g_stop_upload { false };
    std::mutex g_wait_mutex;
    std::condition_variable g_wait_cv;
    std::once_flag g_curl_once;

    std::string
    read_all(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      std::ostringstream ss;
      ss << in.rdbuf();
      return ss.str();
    }

    size_t
    discard(char *, size_t size, size_t count, void *) {
      return size * count;
    }

    int
    stop_requested(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
      return g_stop_upload.load() ? 1 : 0;
    }

    /// 0: network failure or a server error (try again later); otherwise the HTTP status.
    long
    post(const std::string &url, const std::string &meta, const std::string &dump) {
      CURL *curl = curl_easy_init();
      if (!curl) return 0;
      curl_mime *mime = curl_mime_init(curl);
      auto *part = curl_mime_addpart(mime);
      curl_mime_name(part, "meta");
      curl_mime_data(part, meta.data(), meta.size());
      curl_mime_type(part, "application/json");
      if (!dump.empty()) {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "dump");
        curl_mime_data(part, dump.data(), dump.size());
        curl_mime_filename(part, "crash.dmp");
        curl_mime_type(part, "application/octet-stream");
      }
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "FoundationSunshine-crash/1");
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stop_requested);
      const CURLcode rc = curl_easy_perform(curl);
      long status = 0;
      if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
      curl_mime_free(mime);
      curl_easy_cleanup(curl);
      return status;
    }

    /// What names this account and this computer, for the privacy pass over a dump (redact_private_names).
    std::vector<std::string>
    private_names() {
      std::vector<std::string> names;
      auto add = [&](const char *value) {
        if (!value || !*value) return;
        std::string v = value;
        std::string lower = v;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char) std::tolower(c); });
        if (lower == "system" || lower == "root") return;  // not a person, and words the runtime's own names contain
        names.push_back(std::move(v));
      };
      for (const char *key : { "USERNAME", "USER", "LOGNAME", "COMPUTERNAME", "HOSTNAME" }) add(std::getenv(key));
  #ifdef _WIN32
      char buffer[256];
      DWORD size = sizeof(buffer);
      if (GetComputerNameA(buffer, &size)) add(buffer);
  #else
      char host[256] = {};
      if (gethostname(host, sizeof(host) - 1) == 0) add(host);
  #endif
      return names;
    }

    bool
    settled(long status) {
      if (status >= 200 && status < 300) return true;
      return status >= 400 && status < 500 && status != 408 && status != 429;  // it will not be accepted later either
    }

    void
    uploader_main() {
      using namespace std::chrono_literals;
      auto wait = [](std::chrono::seconds s) {
        std::unique_lock lock(g_wait_mutex);
        return !g_wait_cv.wait_for(lock, s, [] { return g_stop_upload.load(); });
      };
      std::chrono::seconds delay = 20s;
      if (const char *env = std::getenv("SUNSHINE_CRASH_UPLOAD_DELAY_S"); env && *env) delay = std::chrono::seconds(std::max(0, std::atoi(env)));
      if (!wait(delay)) return;
      do {
        if (enabled()) {
          const auto dir = current_config_dir();
          upload_pending_once(dir, endpoint_from(current_conf_text()), read_or_create_install_id(dir, true));
        }
      } while (wait(30min));
    }
  }  // namespace

  namespace {
    /// "-p<pid hex>." as the stems the handlers write carry it
    std::string
    own_pid_tag() {
      char tag[24];
  #ifdef _WIN32
      const unsigned long pid = GetCurrentProcessId();
  #else
      const unsigned long pid = (unsigned long) getpid();
  #endif
      std::snprintf(tag, sizeof(tag), "-p%08lx.", pid);
      return tag;
    }
  }  // namespace

  int
  upload_pending_once(const fs::path &config_dir, const std::string &endpoint, const std::string &install_id) {
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    const auto dir = pending_dir(config_dir);
    trim_pending(dir);
    int done = 0;
    const auto now = fs::file_time_type::clock::now();
    std::string base = endpoint;
    while (!base.empty() && base.back() == '/') base.pop_back();
    const auto url = base + "/v1/crash";
    for (const auto &report : list_pending(dir)) {
      if (g_stop_upload.load()) break;
      // one this process is still writing (a hang report: the dump follows the meta only when the meta is last, which it is not, but be safe)
      if (now - report.time < std::chrono::seconds(10) && report.meta.filename().string().find(own_pid_tag()) != std::string::npos) continue;
      auto meta = nlohmann::json::parse(read_all(report.meta), nullptr, false);
      std::string body;
      if (meta.is_discarded() || !meta.is_object()) {
        std::error_code ec;
        fs::remove(report.meta, ec);
        if (!report.dump.empty()) fs::remove(report.dump, ec);
        continue;
      }
      if (!meta.contains("install_id") && !install_id.empty()) meta["install_id"] = install_id;
      if (!meta.contains("v")) meta["v"] = 1;
      body = meta.dump();
      std::string dump;
      if (!report.dump.empty()) {
        std::error_code ec;
        if (fs::file_size(report.dump, ec) <= max_dump_bytes) {
          dump = read_all(report.dump);
          // the dump type keeps heap and data out; this blanks the account and computer names that a path or a stack word still carries
          redact_private_names(dump, private_names());
        }
      }
      const long status = post(url, body, dump);
      // 429 (a per-install limit, wait Retry-After) and 503 (storage failed, nothing counted): keep everything and stop this pass
      if (status == 429 || status == 503) break;
      if (!settled(status)) continue;
      std::error_code ec;
      fs::remove(report.meta, ec);
      if (!report.dump.empty()) fs::remove(report.dump, ec);
      ++done;
    }
    return done;
  }

  // lives here, not in crash_common.cpp: the service wrapper links the capture without the uploader
  void
  run_test_switch() {
    const char *what = std::getenv("SUNSHINE_TEST_CRASH");
    if (!what || !*what) return;
    if (std::string_view(what) == "upload-only") {
      // one synchronous upload pass, then the process ends: what a test needs from a real start, without the servers
      if (enabled()) {
        const auto dir = current_config_dir();
        upload_pending_once(dir, endpoint_from(current_conf_text()), read_or_create_install_id(dir, true));
      }
      std::_Exit(0);
    }
    platform_test_crash(what);
  }

  void
  start_uploader() {
    if (g_uploader.joinable()) return;
    g_stop_upload.store(false);
    g_uploader = std::thread(uploader_main);
  }

  void
  stop() {
    g_stop_upload.store(true);
    g_wait_cv.notify_all();
    if (g_uploader.joinable()) g_uploader.join();
    stop_watchdog();
  }
}  // namespace crash

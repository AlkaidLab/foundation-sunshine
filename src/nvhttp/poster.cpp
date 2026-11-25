/**
 * @file src/nvhttp/poster.cpp
 * @brief Asynchronous application-poster representations and HTTP validators.
 */
#include "poster.h"

// fs-stb-image-api-20261112: use the bundled stb loader/writer APIs; no new
// image dependency is introduced for preview generation.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"  // the static stb implementation carries entry points this file never calls
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../stb_image_write.h"
#pragma GCC diagnostic pop

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <openssl/evp.h>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#elif defined(__linux__)
  #include <sys/resource.h>
  #include <sys/syscall.h>
  #include <unistd.h>
#else
  #include <unistd.h>
#endif

// After the platform block: <windows.h> has to see NOMINMAX first, and the platform header includes it again.
#include "src/file_handler.h"
#include "src/logging_severity.h"
#include "src/platform/common.h"

namespace nvhttp::apps::poster {

  namespace {

    struct source_info_t {
      std::filesystem::path path;
      std::string normalized;  // the absolute, lexically normal path as UTF-8: lossless, unlike path::string() under the system code page
      std::uintmax_t size = 0;
      std::filesystem::file_time_type modified {};
      std::string key;
    };

    struct pixels_t {
      std::vector<unsigned char> rgba;
      int width = 0;
      int height = 0;
      int source_channels = 0;
    };

    struct jpg_buffer_t {
      std::vector<unsigned char> bytes;
    };

    struct work_queue_t {
      std::mutex mutex;
      std::condition_variable condition;
      std::condition_variable drained;
      std::deque<std::filesystem::path> pending;
      std::unordered_set<std::string> queued;
      std::unordered_set<std::string> attempted;  // keys of the source versions whose variants were built (or tried) in this run
      bool started = false;
      bool busy = false;  // the worker holds a source it popped and has not finished with it
      std::string current;  // the queue key of the source the worker holds: a waiter tells a version still in its hands from one it gave up
      bool worker_running = false;  // the worker thread exists; without it nothing produces a preview
      bool cache_available = true;  // the startup cleanup found or made the cache directory
    };

    struct digest_cache_t {
      std::mutex mutex;
      std::unordered_map<std::string, std::string> values;
    };

    work_queue_t &
    work_queue() {
      static auto *queue = new work_queue_t;
      return *queue;
    }

    digest_cache_t &
    digest_cache() {
      static auto *cache = new digest_cache_t;
      return *cache;
    }

    // fs-poster-cache-20261125: every poster cache file lives under the configuration directory. Beside the source it
    // polluted the user's folders, and a name made from the source name went through the system code page when the file
    // was created and came out garbled. The name is the key alone, which is ASCII.
    // Leaked on purpose, like the queue and the digest cache: the worker is detached and can still be running when
    // the static objects of the process are destroyed.
    std::filesystem::path
    cache_root() {
      static const auto *root = new std::filesystem::path(platf::appdata() / "covers" / "cache" / "poster");
      return *root;
    }

    /** The absolute, lexically normal path as UTF-8, or an empty string with `error` set when it cannot be resolved. */
    std::string
    normalized_path(const std::filesystem::path &path, std::error_code &error) {
      const auto absolute = std::filesystem::absolute(path, error);
      if (error) return {};
      return file_handler::path_to_utf8(absolute.lexically_normal());
    }

    std::string
    process_id_text() {
      // Leaked like cache_root(): the worker names its temporary files with it.
#ifdef _WIN32
      static const auto *id = new std::string(std::to_string(GetCurrentProcessId()));
#else
      static const auto *id = new std::string(std::to_string(static_cast<unsigned long>(getpid())));
#endif
      return *id;
    }

    /** `name` ends in `.tmp-<digits>`: the temporary name `write_atomically` gives a cache file. */
    bool
    is_temporary_name(const std::string &name) {
      const auto marker = name.rfind(".tmp-");
      return marker != std::string::npos && marker + 5 < name.size() && name.find_first_not_of("0123456789", marker + 5) == std::string::npos;
    }

    std::string
    hex_digest(const unsigned char *bytes, unsigned int length) {
      static constexpr char digits[] = "0123456789abcdef";
      std::string result;
      result.reserve(length * 2);
      for (unsigned int index = 0; index < length; ++index) {
        result.push_back(digits[bytes[index] >> 4]);
        result.push_back(digits[bytes[index] & 0x0f]);
      }
      return result;
    }

    std::string
    sha256_file(const std::filesystem::path &path) {
      std::ifstream input(path, std::ios::binary);
      if (!input) return {};
      auto *context = EVP_MD_CTX_new();
      if (context == nullptr || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(context);
        return {};
      }
      std::array<unsigned char, 64 * 1024> buffer {};
      while (input) {
        input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) != 1) {
          EVP_MD_CTX_free(context);
          return {};
        }
      }
      if (!input.eof()) {
        EVP_MD_CTX_free(context);
        return {};
      }
      unsigned char digest[EVP_MAX_MD_SIZE] {};
      unsigned int length = 0;
      const auto okay = EVP_DigestFinal_ex(context, digest, &length) == 1;
      EVP_MD_CTX_free(context);
      return okay ? hex_digest(digest, length) : std::string {};
    }

    std::string
    cached_sha256(const source_info_t &source, const std::filesystem::path &path) {
      const auto cache_key = source.key + ":" + file_handler::path_to_utf8(path);
      auto &cache = digest_cache();
      {
        std::lock_guard lock(cache.mutex);
        if (const auto found = cache.values.find(cache_key); found != cache.values.end()) return found->second;
      }
      const auto digest = sha256_file(path);
      if (!digest.empty()) {
        std::lock_guard lock(cache.mutex);
        cache.values[cache_key] = digest;
      }
      return digest;
    }

    std::optional<source_info_t>
    source_info(const std::filesystem::path &path) {
      std::error_code error;
      const auto size = std::filesystem::file_size(path, error);
      if (error) return std::nullopt;
      const auto modified = std::filesystem::last_write_time(path, error);
      if (error) return std::nullopt;
      auto normalized = normalized_path(path, error);
      if (error || normalized.empty()) return std::nullopt;
      const auto key = std::to_string(std::hash<std::string> {}(
        normalized + ":" + std::to_string(size) + ":" + std::to_string(modified.time_since_epoch().count())));
      return source_info_t { path, std::move(normalized), size, modified, key };
    }

    std::filesystem::path
    full_cache_path(const source_info_t &source) {
      return cache_root() / (source.key + "-full.png");
    }

    std::filesystem::path
    preview_cache_path(const source_info_t &source, bool alpha) {
      return cache_root() / (source.key + (alpha ? "-preview.png" : "-preview.jpg"));
    }

    bool
    read_bytes(const std::filesystem::path &path, std::vector<unsigned char> &bytes) {
      std::ifstream input(path, std::ios::binary | std::ios::ate);
      if (!input) return false;
      const auto end = input.tellg();
      if (end < 0 || static_cast<std::uintmax_t>(end) > std::numeric_limits<int>::max()) return false;
      bytes.resize(static_cast<std::size_t>(end));
      input.seekg(0);
      return bytes.empty() || static_cast<bool>(input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
    }

    bool
    load_pixels(const std::filesystem::path &path, pixels_t &pixels) {
      std::vector<unsigned char> bytes;
      if (!read_bytes(path, bytes) || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
      int channels = 0;
      int width = 0;
      int height = 0;
      auto *decoded = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
      if (decoded == nullptr || width <= 0 || height <= 0) return false;
      pixels.width = width;
      pixels.height = height;
      pixels.source_channels = channels;
      pixels.rgba.assign(decoded, decoded + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
      stbi_image_free(decoded);
      return true;
    }

    std::vector<unsigned char>
    resize_rgba(const pixels_t &source, int width, int height) {
      std::vector<unsigned char> result(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
      for (int y = 0; y < height; ++y) {
        const auto source_y = std::min(source.height - 1, y * source.height / height);
        for (int x = 0; x < width; ++x) {
          const auto source_x = std::min(source.width - 1, x * source.width / width);
          const auto source_offset = (static_cast<std::size_t>(source_y) * source.width + source_x) * 4;
          const auto destination_offset = (static_cast<std::size_t>(y) * width + x) * 4;
          std::copy_n(source.rgba.data() + source_offset, 4, result.data() + destination_offset);
        }
      }
      return result;
    }

    void
    jpg_write(void *context, void *data, int size) {
      auto &buffer = *static_cast<jpg_buffer_t *>(context);
      const auto *begin = static_cast<unsigned char *>(data);
      buffer.bytes.insert(buffer.bytes.end(), begin, begin + size);
    }

    std::optional<std::vector<unsigned char>>
    encode_png(const std::vector<unsigned char> &rgba, int width, int height) {
      int size = 0;
      auto *encoded = stbi_write_png_to_mem(rgba.data(), width * 4, width, height, 4, &size);
      if (encoded == nullptr || size <= 0) {
        STBIW_FREE(encoded);
        return std::nullopt;
      }
      std::vector<unsigned char> result(encoded, encoded + size);
      STBIW_FREE(encoded);
      return result;
    }

    std::optional<std::vector<unsigned char>>
    encode_jpeg(const std::vector<unsigned char> &rgba, int width, int height, int quality) {
      std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
      for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(width) * static_cast<std::size_t>(height); ++pixel) {
        std::copy_n(rgba.data() + pixel * 4, 3, rgb.data() + pixel * 3);
      }
      jpg_buffer_t output;
      if (!stbi_write_jpg_to_func(jpg_write, &output, width, height, 3, rgb.data(), quality) || output.bytes.empty()) return std::nullopt;
      return output.bytes;
    }

    bool
    write_atomically(const std::filesystem::path &path, const std::vector<unsigned char> &bytes) {
      // fs-poster-cache-20261125: the temporary name stays in the path domain. A narrow name is created through the system
      // code page and renamed through UTF-8, and the two disagree under a non-ASCII directory (an installation path with
      // Chinese characters): the rename then never finds the file that was just written. The process id keeps the
      // temporary files of two instances apart; the pass is one thread per process.
      auto temporary = path;
      temporary += ".tmp-" + process_id_text();
      {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output) {
          std::error_code ignored;
          std::filesystem::remove(temporary, ignored);
          return false;
        }
      }
      std::error_code error;
      std::filesystem::rename(temporary, path, error);
      if (!error) return true;
      std::filesystem::remove(temporary, error);
      return false;
    }

    void
    lower_worker_priority() noexcept {
#ifdef _WIN32
      // fs-win-setthreadpriority-20261112: background poster work uses the
      // documented below-normal thread priority.
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
      // fs-linux-setpriority-20261112: Linux nice is a per-thread attribute.
      const auto thread_id = static_cast<id_t>(::syscall(SYS_gettid));
      (void) setpriority(PRIO_PROCESS, thread_id, 10);
#endif
    }

    void
    build_variants(const std::filesystem::path &path) {
      const auto source = source_info(path);
      if (!source || source->size <= preview_limit_bytes) return;
      // schedule() runs for every application on every app-list and every poster request, so the same file version arrives again and again. The variants are named after
      // the file's path, size and modification time: when the preview is already on disk, or a generation for it already ran in this run without producing one, there
      // is nothing to do.
      // Without this each request decoded the poster and encoded it again (several seconds of one core for a large one).
      // The error_code overloads: a cache name that the file system refuses (a source name close to the length limit, with the prefix and key added) must count as a miss, not throw out of the worker.
      std::error_code probe_error;
      if (std::filesystem::is_regular_file(preview_cache_path(*source, true), probe_error) || std::filesystem::is_regular_file(preview_cache_path(*source, false), probe_error)) return;
      {
        auto &queue = work_queue();
        std::lock_guard lock(queue.mutex);
        // The startup cleanup found the cache directory unusable: nothing this run writes would land.
        if (!queue.cache_available) return;
        if (!queue.attempted.insert(source->key).second) return;
      }
      pixels_t pixels;
      if (!load_pixels(path, pixels)) return;

      const auto alpha = pixels.source_channels == 2 || pixels.source_channels == 4;
      bool full_written = false;
      bool preview_written = false;
      if (const auto full_png = encode_png(pixels.rgba, pixels.width, pixels.height)) {
        if (full_png->size() < source->size) full_written = write_atomically(full_cache_path(*source), *full_png);
      }

      int width = pixels.width;
      int height = pixels.height;
      const auto scale = std::min(1.0, static_cast<double>(preview_longest_side) / std::max(width, height));
      width = std::max(1, static_cast<int>(width * scale));
      height = std::max(1, static_cast<int>(height * scale));
      for (int attempt = 0; attempt < 12; ++attempt) {
        const auto resized = resize_rgba(pixels, width, height);
        std::optional<std::vector<unsigned char>> encoded = alpha ? encode_png(resized, width, height) : encode_jpeg(resized, width, height, 85);
        if (encoded && encoded->size() <= preview_limit_bytes) {
          preview_written = write_atomically(preview_cache_path(*source, alpha), *encoded);
          break;
        }
        width = std::max(1, static_cast<int>(width * 0.8));
        height = std::max(1, static_cast<int>(height * 0.8));
      }
      if (!full_written && !preview_written) return;
      if (preview_written) {
        auto &queue = work_queue();
        std::lock_guard lock(queue.mutex);
        // The attempt record throttles the sources whose preview cannot be built; this one landed on disk, and the
        // disk check above takes over the deduplication. If the capacity limit evicts the file later, the next
        // request may rebuild it. A write that failed keeps the record: rebuilding would decode and encode the same
        // source again for nothing.
        queue.attempted.erase(source->key);
      }
      // fs-poster-cache-20261125: the startup pass sweeps the leftovers and sets the initial limit; every generation
      // that landed a file repeats the same bounded scan here, so a long-lived process cannot grow the cache past
      // cache_file_limit. Eviction is by age and the files just written have the newest timestamps.
      const auto maintenance = cleanup_cache_dir(cache_root());
      if (maintenance.evicted != 0) {
        BOOST_LOG(info) << "Poster cache evicted " << maintenance.evicted << " old file(s) after one more pass";
      }
    }

    void
    run_worker() {
      lower_worker_priority();
      try {
        // fs-poster-cache-20261125: once per process, on this thread and never on a request thread: the temporary files
        // of a run that ended mid-write, and the oldest cache files beyond the limit. Every generation repeats the
        // same scan afterwards to keep the limit in a long-lived process.
        const auto cleanup = cleanup_cache_dir(cache_root());
        auto &queue = work_queue();
        {
          std::lock_guard lock(queue.mutex);
          queue.cache_available = cleanup.available;
        }
        if (!cleanup.available) {
          BOOST_LOG(warning) << "Poster cache directory is not usable, posters are not cached in this run: " << file_handler::path_to_utf8(cache_root());
        }
        else if (cleanup.removed_temporary != 0 || cleanup.evicted != 0) {
          BOOST_LOG(info) << "Poster cache startup removed " << cleanup.removed_temporary << " temporary file(s) and " << cleanup.evicted << " old cache file(s)";
        }
      }
      catch (...) {
        // The cleanup is best effort by contract. A failure here costs the log line, nothing else.
      }
      auto &queue = work_queue();
      for (;;) {
        std::filesystem::path path;
        bool processing = false;
        try {
          {
            std::unique_lock lock(queue.mutex);
            queue.condition.wait(lock, [&]() { return !queue.pending.empty(); });
            path = std::move(queue.pending.front());
            queue.pending.pop_front();
            std::error_code error;
            auto key = normalized_path(path, error);
            queue.queued.erase(key);
            queue.current = std::move(key);
            queue.busy = true;
            processing = true;
          }
          build_variants(path);
        }
        catch (const std::exception &e) {
          // fs-poster-cache-20261125: the worker is detached, so an exception that leaves this thread ends the process.
          // One source that fails costs its preview, nothing else.
          BOOST_LOG(warning) << "Poster pass dropped one source: " << e.what();
        }
        catch (...) {
          BOOST_LOG(warning) << "Poster pass dropped one source: unknown error";
        }
        if (processing) {
          std::lock_guard lock(queue.mutex);
          queue.busy = false;
          queue.current.clear();
        }
        queue.drained.notify_all();
      }
    }

    std::optional<representation_t>
    representation(const std::filesystem::path &path, bool preview_request) {
      const auto source = source_info(path);
      if (!source) return std::nullopt;
      const auto full_path = full_cache_path(*source);
      // fs-poster-cache-20261125: the error_code overloads. A throwing one failed the whole asset request on a path
      // the file system refuses; here that is a miss.
      std::error_code probe_error;
      const auto chosen_full = std::filesystem::is_regular_file(full_path, probe_error) ? full_path : source->path;
      auto chosen = chosen_full;
      if (preview_request) {
        const auto png_preview = preview_cache_path(*source, true);
        const auto jpg_preview = preview_cache_path(*source, false);
        if (std::filesystem::is_regular_file(png_preview, probe_error)) {
          chosen = png_preview;
        }
        else if (std::filesystem::is_regular_file(jpg_preview, probe_error)) {
          chosen = jpg_preview;
        }
        else if (source->size > preview_limit_bytes) {
          return std::nullopt;
        }
      }
      std::error_code error;
      const auto length = std::filesystem::file_size(chosen, error);
      if (error) return std::nullopt;
      const auto digest = cached_sha256(*source, chosen);
      if (digest.empty()) return std::nullopt;
      auto extension = chosen.extension().string();
      std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
      const auto content_type = extension == ".jpg" || extension == ".jpeg" ? "image/jpeg" :
                                extension == ".webp"                        ? "image/webp" :
                                extension == ".svg"                         ? "image/svg+xml" :
                                                                              "image/png";
      return representation_t { chosen, content_type, "\"" + digest + "\"", length };
    }

    /**
     * The preview of a source of more than preview_limit_bytes, for a caller that has just scheduled it: waits while the
     * background pass still has this version, and no longer than `limit`. `source` is what `source_info()` found for `path`;
     * a preview of this version is known not to be on disk yet.
     */
    std::optional<representation_t>
    wait_for_preview(const std::filesystem::path &path, const source_info_t &source, std::chrono::milliseconds limit) {
      auto &queue = work_queue();
      const auto deadline = std::chrono::steady_clock::now() + limit;
      std::unique_lock lock(queue.mutex);
      for (;;) {
        // Nothing produces a preview without the worker or without a directory to write it to.
        if (!queue.worker_running || !queue.cache_available) return std::nullopt;
        const bool queued = queue.queued.count(source.normalized) != 0;
        const bool in_progress = queue.busy && queue.current == source.normalized;
        // The pass was never asked for this version or is done with it, built or given up: nothing more will appear.
        const bool finished = !queued && !in_progress;
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (finished || remaining <= std::chrono::steady_clock::duration::zero()) {
          lock.unlock();
          auto ready = representation(path, true);  // the pass may have finished in between
          if (ready || finished) return ready;
          BOOST_LOG(info) << "Poster preview was not ready in " << limit.count() << " ms, the request gets the default cover: " << source.normalized;
          return std::nullopt;
        }
        queue.drained.wait_for(lock, remaining);
        // Woken by the pass: look at the directory without holding the lock its threads take.
        lock.unlock();
        auto ready = representation(path, true);
        if (ready) return ready;
        lock.lock();
      }
    }

  }  // namespace

  cleanup_result_t
  cleanup_cache_dir(const std::filesystem::path &directory) noexcept {
    cleanup_result_t result;
    try {
      std::error_code error;
      if (!std::filesystem::is_directory(directory, error)) {
        std::filesystem::create_directories(directory, error);
      }
      // Also the answer when a regular file occupies the directory or one of its parents.
      if (!std::filesystem::is_directory(directory, error)) return result;
      // A directory that exists can still refuse files, a read-only configuration directory for one. Ask with a probe
      // file; it carries a temporary name, so a run that ends before the removal leaves nothing behind for good.
      const auto probe = directory / (".probe-" + process_id_text() + ".tmp-0");
      {
        std::ofstream output(probe, std::ios::binary);
        if (!output) return result;
      }
      std::error_code probe_error;
      std::filesystem::remove(probe, probe_error);
      result.available = true;
      struct cache_file_t {
        std::filesystem::path path;
        std::filesystem::file_time_type written;
      };
      std::vector<cache_file_t> cache_files;
      std::filesystem::directory_iterator entry(directory, error), end;
      while (!error && entry != end) {
        std::error_code file_error;
        if (entry->is_regular_file(file_error)) {
          const auto name = file_handler::path_to_utf8(entry->path().filename());
          if (is_temporary_name(name)) {
            if (std::filesystem::remove(entry->path(), file_error)) ++result.removed_temporary;
          }
          else {
            const auto written = std::filesystem::last_write_time(entry->path(), file_error);
            if (!file_error) cache_files.push_back({ entry->path(), written });
          }
        }
        entry.increment(error);
      }
      if (cache_files.size() > cache_file_limit) {
        std::sort(cache_files.begin(), cache_files.end(), [](const cache_file_t &left, const cache_file_t &right) {
          return left.written < right.written;
        });
        const auto excess = cache_files.size() - cache_file_limit;
        for (std::size_t index = 0; index < excess; ++index) {
          std::error_code remove_error;
          if (std::filesystem::remove(cache_files[index].path, remove_error)) ++result.evicted;
        }
      }
    }
    catch (...) {
      // Best effort by contract: whatever failed stays as it is, and the result tells how far it got.
    }
    return result;
  }

  void
  schedule(const std::filesystem::path &source) {
    auto &queue = work_queue();
    // A path that cannot be resolved is not queued: its poster is served from the source.
    std::error_code error;
    const auto key = normalized_path(source, error);
    if (error) return;
    {
      std::lock_guard lock(queue.mutex);
      if (queue.queued.insert(key).second) queue.pending.push_back(source);
      if (!queue.started) {
        // One attempt per process: without the worker nothing is cached, and nobody waits for what nobody produces.
        queue.started = true;
        try {
          std::thread(run_worker).detach();
          queue.worker_running = true;
        }
        catch (const std::exception &e) {
          BOOST_LOG(warning) << "Poster pass could not start, posters are not cached in this run: " << e.what();
        }
      }
    }
    queue.condition.notify_one();
  }

  bool
  wait_idle(std::chrono::milliseconds timeout) {
    auto &queue = work_queue();
    std::unique_lock lock(queue.mutex);
    return queue.drained.wait_for(lock, timeout, [&]() { return queue.pending.empty() && !queue.busy; });
  }

  std::optional<representation_t>
  full(const std::filesystem::path &source) {
    return representation(source, false);
  }

  std::optional<representation_t>
  preview(const std::filesystem::path &source) {
    return representation(source, true);
  }

  std::optional<representation_t>
  preview_when_ready(const std::filesystem::path &source, std::chrono::milliseconds limit) {
    const auto info = source_info(source);
    if (!info) return std::nullopt;
    // A source of at most preview_limit_bytes is its own preview, and one from an earlier run is simply there.
    if (auto ready = representation(source, true)) return ready;
    try {
      return wait_for_preview(source, *info, limit);
    }
    catch (const std::system_error &e) {
      // fs-poster-cache-20261125: condition_variable::wait_for is the one call on this path that can fail. A miss is the answer then.
      BOOST_LOG(warning) << "Poster preview wait failed, the request gets no preview: " << e.what();
      return std::nullopt;
    }
  }

  std::optional<byte_range_t>
  parse_range(std::string_view value, std::uintmax_t length) noexcept {
    if (length == 0 || value.size() < 7 || value.substr(0, 6) != "bytes=") return std::nullopt;
    value.remove_prefix(6);
    const auto dash = value.find('-');
    if (dash == std::string_view::npos || dash == 0) return std::nullopt;
    auto parse_uint = [](std::string_view text, std::uintmax_t &result) {
      if (text.empty()) return false;
      result = 0;
      for (const auto character : text) {
        if (character < '0' || character > '9') return false;
        const auto digit = static_cast<std::uintmax_t>(character - '0');
        if (result > (std::numeric_limits<std::uintmax_t>::max() - digit) / 10) return false;
        result = result * 10 + digit;
      }
      return true;
    };
    byte_range_t result;
    if (!parse_uint(value.substr(0, dash), result.first) || result.first >= length) return std::nullopt;
    if (dash + 1 == value.size())
      result.last = length - 1;
    else if (!parse_uint(value.substr(dash + 1), result.last) || result.last < result.first)
      return std::nullopt;
    result.last = std::min(result.last, length - 1);
    return result;
  }

  bool
  if_none_match(std::string_view value, std::string_view etag) noexcept {
    while (!value.empty()) {
      const auto comma = value.find(',');
      auto token = value.substr(0, comma);
      while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
      while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
      if (token == "*" || token == etag) return true;
      if (comma == std::string_view::npos) break;
      value.remove_prefix(comma + 1);
    }
    return false;
  }

  bool
  if_range_matches(std::string_view value, std::string_view etag) noexcept {
    auto trim = [](std::string_view text) {
      while (!text.empty() && (text.front() == ' ' || text.front() == '	')) text.remove_prefix(1);
      while (!text.empty() && (text.back() == ' ' || text.back() == '	')) text.remove_suffix(1);
      return text;
    };
    // fs-rfc9110-ranges-20261112 section 13.1.5: a DQUOTE in the first three
    // characters is an entity-tag; anything else is an HTTP-date. Poster
    // responses carry a strong ETag and no Last-Modified, so a date does not
    // match. Section 8.8.3.2: strong comparison requires both tags to be
    // strong and their opaque-tags to be identical.
    value = trim(value);
    etag = trim(etag);
    auto quoted = [](std::string_view text) {
      const auto head = text.substr(0, std::min<std::size_t>(text.size(), 3));
      return head.find('"') != std::string_view::npos;
    };
    if (!quoted(value) || !quoted(etag)) return false;
    auto opaque = [](std::string_view text, bool &weak) {
      weak = text.size() >= 2 && text[0] == 'W' && text[1] == '/';
      if (weak) text.remove_prefix(2);
      return text;
    };
    bool value_weak = false;
    bool etag_weak = false;
    const auto value_tag = opaque(value, value_weak);
    const auto etag_tag = opaque(etag, etag_weak);
    return !value_weak && !etag_weak && value_tag == etag_tag;
  }

}  // namespace nvhttp::apps::poster

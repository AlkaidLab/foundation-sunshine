/**
 * @file src/file_transfer/file_transfer.cpp
 * @brief See file_transfer.h.
 */
#include "file_transfer.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "src/crypto.h"
#include "src/logging.h"

#ifdef _WIN32
  #include <windows.h>

  #include "src/platform/windows/misc.h"
#else
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

using namespace std::literals;
namespace fs = std::filesystem;

namespace file_transfer {
  namespace {
    using clock_t = std::chrono::steady_clock;

    struct stored_t {
      offer_t offer;
      std::vector<fs::path> sources;  ///< parallel to offer.entries; empty for folders
      clock_t::time_point expires_at;
    };

    std::mutex g_mu;
    std::unordered_map<std::string, stored_t> g_offers;
    std::deque<std::string> g_order;  ///< ids, oldest first

    std::string
    make_id() {
      const auto random = crypto::rand(32);
      if (random.size() != 32) {
        throw std::runtime_error("file_transfer: CSPRNG failed");
      }
      static constexpr char kHex[] = "0123456789abcdef";
      std::string s;
      s.reserve(64);
      for (const auto byte : random) {
        const auto b = static_cast<std::uint8_t>(byte);
        s += kHex[b >> 4];
        s += kHex[b & 0xF];
      }
      return s;
    }

    /// Caller holds g_mu.
    void
    sweep_locked(clock_t::time_point now) {
      for (auto it = g_order.begin(); it != g_order.end();) {
        auto found = g_offers.find(*it);
        if (found == g_offers.end() || found->second.expires_at <= now) {
          if (found != g_offers.end()) g_offers.erase(found);
          it = g_order.erase(it);
        }
        else {
          ++it;
        }
      }
    }

    fs::path
    from_utf8(const std::string &s) {
      return fs::path(std::u8string(reinterpret_cast<const char8_t *>(s.data()), s.size()));
    }

    std::string
    to_utf8(const std::u8string &s) {
      return std::string(reinterpret_cast<const char *>(s.data()), s.size());
    }

    /// UTF-16 code units of a UTF-8 string (4-byte sequences take two).
    std::size_t
    utf16_units(std::string_view s) {
      std::size_t units = 0;
      for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) units += (c >= 0xF0) ? 2 : 1;
      }
      return units;
    }

    std::string
    fold_ascii(std::string s) {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
      return s;
    }

    std::int64_t
    unix_mtime(const fs::path &p) {
      std::error_code ec;
      const auto t = fs::last_write_time(p, ec);
      if (ec) return 0;
      const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
      return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
    }

    /// Links are neither offered nor descended into: a junction can loop back
    /// up the tree. Other reparse points (OneDrive and other cloud placeholders)
    /// are ordinary files and folders to the user and stay in.
    bool
    is_link_like(const fs::directory_entry &entry) {
      std::error_code ec;
      if (entry.is_symlink(ec)) return true;
#ifdef _WIN32
      WIN32_FIND_DATAW data {};
      HANDLE find = FindFirstFileW(entry.path().c_str(), &data);
      if (find == INVALID_HANDLE_VALUE) return false;
      FindClose(find);
      if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        // For a reparse point dwReserved0 holds its tag.
        return data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || data.dwReserved0 == IO_REPARSE_TAG_SYMLINK;
      }
#endif
      return false;
    }

    /// Run `body` as the interactive user. Returns false when there is no such user.
    /// `body` must not throw: an exception would skip RevertToSelf().
    template <class F>
    bool
    as_user(F &&body) {
#ifdef _WIN32
      if (!platf::is_running_as_system()) {
        // A portable Sunshine already runs as the user.
        body();
        return true;
      }
      HANDLE token = platf::retrieve_interactive_users_token(false);
      if (token == nullptr) {
        return false;
      }
      const auto ec = platf::impersonate_current_user(token, [&]() { body(); });
      CloseHandle(token);
      return !ec;
#else
      body();
      return true;
#endif
    }

    struct listing_t {
      std::vector<entry_t> entries;
      std::vector<fs::path> sources;
      std::uint64_t total = 0;
      std::string error;
      std::string detail;
    };

    /// True when `text` is well-formed UTF-8. Names that are not (possible outside
    /// Windows) would all turn into the same replacement characters in the offer.
    bool
    valid_utf8(std::string_view text) {
      for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
          ++i;
          continue;
        }
        std::size_t extra = 0;
        std::uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
          extra = 1;
          cp = c & 0x1F;
        }
        else if ((c & 0xF0) == 0xE0) {
          extra = 2;
          cp = c & 0x0F;
        }
        else if ((c & 0xF8) == 0xF0) {
          extra = 3;
          cp = c & 0x07;
        }
        else {
          return false;
        }
        if (text.size() - i <= extra) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
          const auto cc = static_cast<unsigned char>(text[i + k]);
          if ((cc & 0xC0) != 0x80) return false;
          cp = (cp << 6) | (cc & 0x3F);
        }
        // Overlong forms, surrogates and values past U+10FFFF.
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
          return false;
        }
        i += extra + 1;
      }
      return true;
    }

    /// Whether the user can open `source` for reading (open and close it right away): the type and size
    /// can be read without that right, and the paste would only fail later.
    bool
    readable(const fs::path &source) {
#ifdef _WIN32
      HANDLE h = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
      if (h == INVALID_HANDLE_VALUE) return false;
      CloseHandle(h);
      return true;
#else
      const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) return false;
      ::close(fd);
      return true;
#endif
    }

    bool
    add_entry(listing_t &out, std::string rel, const fs::path &source, bool dir, std::uint64_t size) {
      if (out.entries.size() >= kMaxEntries) {
        out.error = "too_many_entries";
        return false;
      }
      if (!valid_utf8(rel)) {
        out.error = "bad_name";
        out.detail = rel;
        return false;
      }
      if (!dir && !readable(source)) {
        out.error = "unreadable";
        out.detail = to_utf8(source.u8string());
        return false;
      }
      entry_t e;
      e.path = std::move(rel);
      e.dir = dir;
      e.size = dir ? 0 : size;
      e.mtime = unix_mtime(source);
      out.total += e.size;
      out.entries.push_back(std::move(e));
      out.sources.push_back(dir ? fs::path {} : source);
      return true;
    }

    /// Body of register_paths(), run as the user. Never throws.
    void
    list_as_user(const std::vector<std::string> &paths, listing_t &out) noexcept {
      try {
        std::set<std::string> top_names;
        for (const auto &raw : paths) {
          fs::path in = from_utf8(raw).lexically_normal();
          if (!in.is_absolute()) {
            out.error = "not_found";
            out.detail = raw;
            return;
          }
          if (in.filename().empty() && in.has_relative_path()) {
            in = in.parent_path();  // "D:\dir\" names the folder
          }
          const fs::path name = in.filename();
          if (name.empty()) {
            out.error = "unsupported";  // a drive root
            out.detail = raw;
            return;
          }
          if (!top_names.insert(fold_ascii(to_utf8(name.u8string()))).second) {
            out.error = "duplicate_name";
            out.detail = raw;
            return;
          }

          std::error_code ec;
          const auto st = fs::status(in, ec);
          if (ec || !fs::exists(st)) {
            out.error = "not_found";
            out.detail = raw;
            return;
          }
          auto top = relative_path(name, {});
          if (!top) {
            out.error = "path_too_long";
            out.detail = raw;
            return;
          }

          if (fs::is_regular_file(st)) {
            const auto size = fs::file_size(in, ec);
            if (ec) {
              out.error = "not_found";
              out.detail = raw;
              return;
            }
            if (!add_entry(out, *top, in, false, size)) return;
            continue;
          }
          if (!fs::is_directory(st)) {
            out.error = "unsupported";
            out.detail = raw;
            return;
          }

          if (!add_entry(out, *top, in, true, 0)) return;
          // A folder or file that cannot be read fails the offer: skipped, the
          // client would paste the folder without it and never know.
          fs::recursive_directory_iterator it(in, fs::directory_options::none, ec);
          const fs::recursive_directory_iterator end;
          fs::path last = in;
          for (; !ec && it != end; it.increment(ec)) {
            const auto &entry = *it;
            last = entry.path();
            if (is_link_like(entry)) {
              it.disable_recursion_pending();
              continue;
            }
            std::error_code entry_ec;
            const bool dir = entry.is_directory(entry_ec);
            const bool file = !dir && entry.is_regular_file(entry_ec);
            if (entry_ec) {
              out.error = "unreadable";
              out.detail = to_utf8(entry.path().u8string());
              return;
            }
            if (!dir && !file) {
              continue;  // devices, sockets and the like are not files to paste
            }
            auto rel = relative_path(name, entry.path().lexically_relative(in));
            if (!rel) {
              out.error = "path_too_long";
              out.detail = to_utf8(entry.path().u8string());
              return;
            }
            const std::uint64_t size = file ? entry.file_size(entry_ec) : 0;
            if (entry_ec) {
              out.error = "unreadable";
              out.detail = to_utf8(entry.path().u8string());
              return;
            }
            if (!add_entry(out, std::move(*rel), entry.path(), dir, size)) return;
          }
          if (ec) {
            // The folder (or one below it) vanished or could not be opened.
            out.error = ec == std::errc::no_such_file_or_directory ? "not_found" : "unreadable";
            out.detail = to_utf8(last.u8string());
            return;
          }
        }
      }
      catch (...) {
        out.error = "internal";
      }
    }
  }  // namespace

  std::optional<std::string>
  relative_path(const fs::path &root_name, const fs::path &below) {
    std::string out;
    const auto add = [&out](const fs::path &part) -> bool {
      for (const auto &component : part) {
        const auto text = to_utf8(component.u8string());
        if (text.empty() || text == "." || text == ".." || text.find_first_of("/\\:") != std::string::npos) {
          return false;
        }
        if (!out.empty()) out += '/';
        out += text;
      }
      return true;
    };
    if (root_name.empty() || !add(root_name) || !add(below)) {
      return std::nullopt;
    }
    if (utf16_units(out) > kMaxRelativePathUnits) {
      return std::nullopt;
    }
    return out;
  }

  register_result_t
  register_paths(const std::vector<std::string> &paths, std::string type) {
    register_result_t result;
    if (paths.empty()) {
      result.error = "no_paths";
      return result;
    }

    listing_t listing;
    if (!as_user([&]() { list_as_user(paths, listing); })) {
      result.error = "no_user";
      return result;
    }
    if (!listing.error.empty()) {
      result.error = std::move(listing.error);
      result.detail = std::move(listing.detail);
      return result;
    }

    offer_t offer;
    offer.id = make_id();
    offer.source = (type == "send") ? "send" : "clipboard";
    offer.entries = std::move(listing.entries);
    offer.total_size = listing.total;

    {
      std::lock_guard lk(g_mu);
      const auto now = clock_t::now();
      sweep_locked(now);
      while (g_order.size() >= kMaxOffers) {
        g_offers.erase(g_order.front());
        g_order.pop_front();
      }
      g_offers.emplace(offer.id, stored_t { offer, std::move(listing.sources), now + kOfferTtl });
      g_order.push_back(offer.id);
    }

    BOOST_LOG(info) << "File transfer: offered "sv << offer.entries.size() << " entries ("sv << offer.total_size << " bytes)"sv;
    result.offer = std::move(offer);
    return result;
  }

  nlohmann::json
  offer_json(const offer_t &offer) {
    nlohmann::json files = nlohmann::json::array();
    for (const auto &e : offer.entries) {
      files.push_back({ { "path", e.path }, { "size", e.size }, { "dir", e.dir }, { "mtime", e.mtime } });
    }
    const bool single = offer.source == "send" && offer.entries.size() == 1 && !offer.entries.front().dir;
    nlohmann::json out {
      { "version", 2 },
      { "id", offer.id },
      { "type", single ? "file" : "files" },
      { "source", offer.source },
      { "total_size", offer.total_size },
      { "expires_in", std::chrono::duration_cast<std::chrono::seconds>(kOfferTtl).count() },
      { "files", std::move(files) },
    };
    if (single) {
      out["name"] = offer.entries.front().path;
      out["size"] = offer.entries.front().size;
      out["mime"] = "application/octet-stream";
      out["download_url"] = "/api/v1/file-transfer/" + offer.id;
    }
    return out;
  }

  void
  clear() {
    std::lock_guard lk(g_mu);
    g_offers.clear();
    g_order.clear();
  }

  open_file_t::open_file_t(native_t native, std::uint64_t size):
      _native(native), _size(size) {}

  open_file_t::~open_file_t() {
    close();
  }

  open_file_t::open_file_t(open_file_t &&other) noexcept:
      _native(other.release()), _size(other._size) {}

  open_file_t &
  open_file_t::operator=(open_file_t &&other) noexcept {
    if (this != &other) {
      close();
      _size = other._size;
      _native = other.release();
    }
    return *this;
  }

  bool
  open_file_t::valid() const {
#ifdef _WIN32
    return _native != nullptr && _native != INVALID_HANDLE_VALUE;
#else
    return _native >= 0;
#endif
  }

  open_file_t::native_t
  open_file_t::release() {
    auto native = _native;
#ifdef _WIN32
    _native = nullptr;
#else
    _native = -1;
#endif
    return native;
  }

  void
  open_file_t::close() {
    if (!valid()) return;
#ifdef _WIN32
    CloseHandle(_native);
    _native = nullptr;
#else
    ::close(_native);
    _native = -1;
#endif
  }

  open_result_t
  open_entry(std::string_view id, std::string_view index_text) {
    open_result_t result;
    std::size_t index = 0;
    const auto [end, err] = std::from_chars(index_text.data(), index_text.data() + index_text.size(), index);
    if (!is_valid_id(id) || err != std::errc {} || end != index_text.data() + index_text.size()) {
      result.status = 404;
      result.error = "not_found";
      return result;
    }

    fs::path source;
    std::uint64_t expected = 0;
    {
      std::lock_guard lk(g_mu);
      sweep_locked(clock_t::now());
      const auto it = g_offers.find(std::string(id));
      if (it == g_offers.end() || index >= it->second.offer.entries.size() || it->second.offer.entries[index].dir) {
        result.status = 404;
        result.error = "not_found";
        return result;
      }
      source = it->second.sources[index];
      expected = it->second.offer.entries[index].size;
    }

    open_file_t::native_t native {};
    std::uint64_t size = 0;
    bool opened = false;
    const bool user = as_user([&]() {
#ifdef _WIN32
      // Writers and deleters may keep working on the file; a size change is caught below.
      HANDLE h = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
      if (h == INVALID_HANDLE_VALUE) return;
      LARGE_INTEGER li {};
      if (!GetFileSizeEx(h, &li)) {
        CloseHandle(h);
        return;
      }
      native = h;
      size = static_cast<std::uint64_t>(li.QuadPart);
      opened = true;
#else
      const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) return;
      struct stat st {};
      if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return;
      }
      native = fd;
      size = static_cast<std::uint64_t>(st.st_size);
      opened = true;
#endif
    });
    if (!user) {
      result.status = 503;
      result.error = "no_user";
      return result;
    }
    if (!opened) {
      result.status = 410;
      result.error = "gone";
      return result;
    }
    open_file_t file(native, size);
    if (size != expected) {
      // The client already announced the old size to the paste target; a different body would be cut or padded.
      result.status = 409;
      result.error = "changed";
      return result;
    }
    result.file = std::move(file);
    return result;
  }

  bool
  is_valid_id(std::string_view id) {
    return id.size() == 64 && std::all_of(id.begin(), id.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
  }

  bool
  is_single_file(std::string_view id) {
    if (!is_valid_id(id)) return false;
    std::lock_guard lk(g_mu);
    sweep_locked(clock_t::now());
    const auto it = g_offers.find(std::string(id));
    return it != g_offers.end() && it->second.offer.entries.size() == 1 && !it->second.offer.entries.front().dir;
  }

  std::optional<std::uint64_t>
  parse_range(std::string_view header, std::uint64_t size, bool &unsatisfiable) {
    unsatisfiable = false;
    constexpr auto prefix = "bytes="sv;
    if (!header.starts_with(prefix) || !header.ends_with('-')) {
      return std::nullopt;
    }
    const auto number = header.substr(prefix.size(), header.size() - prefix.size() - 1);
    std::uint64_t start = 0;
    const auto [end, err] = std::from_chars(number.data(), number.data() + number.size(), start);
    if (number.empty() || err != std::errc {} || end != number.data() + number.size()) {
      return std::nullopt;
    }
    if (start >= size) {
      // "bytes=0-" of an empty file is the whole (empty) file, not an error.
      if (start == 0) return std::nullopt;
      unsatisfiable = true;
      return std::nullopt;
    }
    return start;
  }
}  // namespace file_transfer

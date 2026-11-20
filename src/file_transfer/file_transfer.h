/**
 * @file src/file_transfer/file_transfer.h
 * @brief Files copied on the host, offered to streaming clients for pasting.
 *
 * The user-session GUI agent sees the user copy files (or pick "send to
 * client" in Explorer) and registers their paths here. Sunshine lists them,
 * remembers the list under a random id and returns the offer, which the agent
 * sends to clients as a clipboard frame (kind 4). A client that pastes pulls
 * each file over the paired nvhttp channel:
 *
 *   agent  -> POST /api/v1/file-transfer/offers            (confighttp, admin auth)
 *             body {"paths": ["C:\\a.txt", "D:\\dir"], "type": "clipboard"}
 *             200  offer JSON (see offer_json)
 *   client -> GET  /api/v1/file-transfer/<id>/<index>      (nvhttp, paired client cert)
 *             optional "Range: bytes=N-"; 200/206 with the file's bytes
 *
 * Compatibility with the first, single-file version of this protocol (an
 * agent that registers {"path": "<file>"}, clients that take {"type": "file",
 * "id", "name", "size", "download_url"} and GET /api/v1/file-transfer/<id>):
 * ids keep its shape (64 lower-case hex digits), that route answers for an
 * offer of one file, and offer_json() adds the old fields to an offer of one
 * file sent with "send to client" (type "file"). Everything else is type
 * "files", which an old client drops instead of downloading every copy.
 *
 * Nothing is copied: a download reads the file where it is. On Windows every
 * listing and every open runs as the interactive user (Sunshine's service runs
 * as SYSTEM), so only what that user can read is offered and the user's mapped
 * drives resolve. The id is a bearer capability like a clipboard blob id.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace file_transfer {
  /// Most entries (files and folders) one offer may list.
  constexpr std::size_t kMaxEntries = 10000;
  /// Longest relative path, in UTF-16 code units: Windows' FILEDESCRIPTORW holds MAX_PATH.
  constexpr std::size_t kMaxRelativePathUnits = 259;
  /// How long an offer stays downloadable. A copy may be pasted long after it was made.
  constexpr std::chrono::seconds kOfferTtl { 60 * 60 };
  /// Offers kept at once; registering another drops the oldest.
  constexpr std::size_t kMaxOffers = 8;

  struct entry_t {
    std::string path;  ///< relative to the offer, '/'-separated, UTF-8
    std::uint64_t size = 0;  ///< 0 for a folder
    bool dir = false;
    std::int64_t mtime = 0;  ///< seconds since the Unix epoch, 0 when unknown
  };

  struct offer_t {
    std::string id;
    std::string source;  ///< "clipboard" (a copy) or "send" (Explorer's "send to client")
    std::vector<entry_t> entries;
    std::uint64_t total_size = 0;
  };

  /// What the registration produced: the offer, or an error word for the agent.
  struct register_result_t {
    std::optional<offer_t> offer;
    std::string error;  ///< "no_paths", "not_found", "unreadable", "unsupported", "too_many_entries", "path_too_long", "duplicate_name", "no_user"
    std::string detail;  ///< the path the error is about, when there is one
  };

  /// List `paths` (absolute, UTF-8) as the interactive user and remember the
  /// offer. A folder is listed with everything below it, parents first;
  /// symbolic links and other non-regular entries are left out.
  register_result_t
  register_paths(const std::vector<std::string> &paths, std::string type);

  /// The offer as the agent and the clients receive it:
  /// {"version": 2, "id", "type": "file"|"files", "source", "total_size", "expires_in",
  ///  "files": [{"path","size","dir","mtime"}]}, plus "name", "size", "mime" and
  /// "download_url" when type is "file" (what single-file clients read).
  /// Type "file": one regular file, offered with "send to client".
  nlohmann::json
  offer_json(const offer_t &offer);

  /// Forget every offer (tests, shutdown).
  void
  clear();

  /// A file opened for download. Owns the native handle until release().
  class open_file_t {
  public:
#ifdef _WIN32
    using native_t = void *;
#else
    using native_t = int;
#endif
    open_file_t() = default;
    open_file_t(native_t native, std::uint64_t size);
    ~open_file_t();
    open_file_t(open_file_t &&other) noexcept;
    open_file_t &
    operator=(open_file_t &&other) noexcept;
    open_file_t(const open_file_t &) = delete;
    open_file_t &
    operator=(const open_file_t &) = delete;

    bool
    valid() const;
    std::uint64_t
    size() const { return _size; }
    /// Hand the handle to the caller, who closes it from now on.
    native_t
    release();

  private:
    void
    close();
#ifdef _WIN32
    native_t _native = nullptr;
#else
    native_t _native = -1;
#endif
    std::uint64_t _size = 0;
  };

  struct open_result_t {
    open_file_t file;
    int status = 200;  ///< HTTP status to answer with when the file did not open
    std::string error;  ///< "not_found" (no such offer or entry), "gone" (the file is no longer readable), "changed" (its size changed), "no_user"
  };

  /// True when offer `id` is a single regular file (what the single-file route serves).
  bool
  is_single_file(std::string_view id);

  /// Open entry `index` of offer `id` for reading, as the interactive user.
  open_result_t
  open_entry(std::string_view id, std::string_view index);

  /// True for the ids register_paths() makes: 64 lower-case hex digits (32 random bytes).
  bool
  is_valid_id(std::string_view id);

  /// Parse a Range header against a file of `size` bytes. Only a single
  /// "bytes=N-" range (what a resumed download asks for) is honoured:
  /// returns its start; any other form returns nullopt and the whole file is
  /// sent. Sets `unsatisfiable` when N is past the end.
  std::optional<std::uint64_t>
  parse_range(std::string_view header, std::uint64_t size, bool &unsatisfiable);

  /// Turn a path below a listed root into the offer's relative form, or
  /// nullopt when it does not fit (too long, or not representable).
  std::optional<std::string>
  relative_path(const std::filesystem::path &root_name, const std::filesystem::path &below);
}  // namespace file_transfer

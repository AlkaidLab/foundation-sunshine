/**
 * @file src/file_transfer/file_transfer_pull.h
 * @brief Files copied on a client, pasted on the host: Sunshine relays each
 *        chunk the host pastes from the client that offered the files.
 *
 * A client that copies files sends their list (clipboard kind 4) over the
 * stream; the bridge forwards it to the user-session GUI agent unchanged,
 * together with the streaming session's id. The agent puts virtual files on
 * the host clipboard. When something pastes them, the agent asks for the bytes
 * chunk by chunk. The client cannot be reached directly (it is usually behind
 * NAT), so Sunshine asks it over the stream and the client answers with an
 * HTTPS POST on the paired nvhttp channel:
 *
 *   agent  -> GET  /api/v1/file-transfer/inbound/<sid>/<id>/<index>?offset=N&length=M
 *             (confighttp, admin auth; waits for the chunk)
 *   host   -> clipboard frame kind 5 to session <sid>:
 *             {"ticket","id","index","offset","length"}
 *   client -> POST /api/v1/file-transfer/chunk/<ticket>   (nvhttp, paired client cert)
 *             body: the bytes from `offset`, at most `length` (fewer only at the end of the file);
 *             or an empty body with "X-File-Transfer-Error: <word>"
 *   agent  <- 200 with the bytes, or 4xx/5xx {"error": "<word>"}
 *
 * A ticket is single-use, belongs to the client of the session it was sent to
 * (the chunk's client certificate must match), and is answered with 504
 * "timeout" when the client does not post in time.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/http/server.h"

namespace file_transfer_pull {
  /// Clipboard frame kind of a chunk request (host -> client).
  constexpr std::uint8_t kKindPull = 5;
  /// Largest chunk the agent may ask for (the client's POST body is buffered).
  constexpr std::uint64_t kMaxChunk = 4 * 1024 * 1024;
  /// How long the client has to post a chunk.
  constexpr std::chrono::seconds kChunkTimeout { 30 };
  /// Chunks waiting at once, over every session.
  constexpr std::size_t kMaxPending = 32;

  /// Hooks to the rest of Sunshine; tests replace them.
  struct hooks_t {
    /// The client certificate uuid of running session `sid`, or nothing when no such session runs.
    std::function<std::optional<std::string>(std::uint64_t sid)> session_client;
    /// Send a clipboard payload to session `sid`.
    std::function<void(std::uint64_t sid, std::vector<std::uint8_t> payload)> send;
    /// How long a client has to post a chunk; zero means kChunkTimeout.
    std::chrono::milliseconds chunk_timeout { 0 };
  };

  /// Replace the hooks; an empty hooks_t restores the real ones.
  void
  set_hooks(hooks_t hooks);

  /// Answer every waiting chunk request with 503 and forget it (tests, shutdown).
  void
  clear();

  /// Chunk requests waiting for their client.
  std::size_t
  pending_count();

  /// The agent's request. path_match: [1] sid, [2] offer id, [3] index; query: offset, length.
  /// Answers later, from another thread, when the chunk arrives or the ticket times out.
  void
  handle_pull(std::shared_ptr<web::detail::ResponseBuffer> response, const web::Request &request);

  /// The client's chunk. path_match: [1] ticket.
  void
  handle_chunk(web::detail::ResponseBuffer &response, const web::Request &request);

  /// The kind 5 frame that asks a client for a chunk (version 1 clipboard framing).
  std::vector<std::uint8_t>
  make_pull_frame(std::string_view ticket, std::string_view id, std::uint32_t index, std::uint64_t offset, std::uint64_t length);
}  // namespace file_transfer_pull

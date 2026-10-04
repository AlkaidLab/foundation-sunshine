/**
 * @file src/platform/udp_send_impl.h
 * @brief Bounded UDP submission implementation shared by production and syscall tests.
 */
#pragma once

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #ifndef UDP_SEND_MSG_SIZE
    #define UDP_SEND_MSG_SIZE 2
  #endif
#else
  #include <arpa/inet.h>
  #include <cerrno>
  #include <netinet/in.h>
  #include <sys/socket.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <boost/asio/ip/address.hpp>
#include "src/platform/common.h"

namespace platf::udp_send_detail {
  inline udp_send_attempt_t
  invalid_request() {
    return {};
  }

  inline udp_send_attempt_t
  unknown_submission(size_t reported_bytes, int native_error = 0) {
    return {udp_send_status_e::unknown_submission, 0, 0, native_error, false, false, reported_bytes};
  }

  inline udp_send_attempt_t
  complete(size_t datagrams, size_t bytes) {
    return {udp_send_status_e::complete, datagrams, bytes, 0, false, true, bytes};
  }

  template<class LengthAt>
  udp_send_attempt_t
  interpret_batch_prefix(int native_count, size_t requested, size_t datagram_size, LengthAt length_at) {
    if (native_count < 0 || static_cast<size_t>(native_count) > requested || requested > max_try_send_batch) {
      return unknown_submission(0);
    }
    size_t confirmed = 0;
    size_t reported_bytes = 0;
    bool known = true;
    for (size_t i = 0; i < static_cast<size_t>(native_count); ++i) {
      const auto bytes = static_cast<size_t>(length_at(i));
      if (bytes > std::numeric_limits<size_t>::max() - reported_bytes) {
        return unknown_submission(std::numeric_limits<size_t>::max());
      }
      reported_bytes += bytes;
      if (bytes == datagram_size && known) {
        ++confirmed;
      }
      else {
        known = false;
      }
    }
    if (!known) {
      auto result = unknown_submission(reported_bytes);
      result.submitted_datagrams = confirmed;
      result.submitted_payload_bytes = confirmed * datagram_size;
      return result;
    }
    // After a positive sendmmsg prefix, Linux discards the first suffix error.
    // errno is stale and must not be reported. Retry is an owner's later action.
    return {confirmed == requested ? udp_send_status_e::complete : udp_send_status_e::partial,
            confirmed, confirmed * datagram_size, 0, confirmed != requested, true, reported_bytes};
  }

  inline bool
  valid_addresses(const boost::asio::ip::address &target, const boost::asio::ip::address &source, uint16_t port) {
    if (!port || target.is_unspecified()) {
      return false;
    }
    if ((target.is_v6() && target.to_v6().scope_id() > std::numeric_limits<uint32_t>::max()) ||
        (source.is_v6() && source.to_v6().scope_id() > std::numeric_limits<uint32_t>::max())) {
      return false;
    }
    if (source.is_unspecified()) {
      return true;
    }
    const bool target_v4 = target.is_v4() || target.to_v6().is_v4_mapped();
    const bool source_v4 = source.is_v4() || source.to_v6().is_v4_mapped();
    return target_v4 == source_v4;
  }

  inline bool
  valid_single(const send_info_t &info) {
    return valid_addresses(info.target_address, info.source_address, info.target_port) &&
           (info.header_size || info.payload_size) &&
           (!info.header_size || info.header) && (!info.payload_size || info.payload) &&
           info.header_size <= max_try_send_udp_payload &&
           info.payload_size <= max_try_send_udp_payload - info.header_size;
  }

  struct batch_layout_t {
    std::array<const char *, max_try_send_batch> payloads {};
    size_t datagram_size = 0;
  };

  // No allocation, unbounded descriptor scan, multiplication overflow, or syscall
  // happens before the complete batch has been validated. Buffer capacities for
  // the optional header array remain the caller's precondition (the legacy type
  // carries no header capacity).
  inline bool
  validate_batch(const batched_send_info_t &info, batch_layout_t &layout) {
    if (!valid_addresses(info.target_address, info.source_address, info.target_port) ||
        !info.block_count || info.block_count > max_try_send_batch ||
        !info.payload_size || info.header_size > max_try_send_udp_payload ||
        info.payload_size > max_try_send_udp_payload - info.header_size ||
        (info.header_size && !info.headers) || info.payload_buffers.empty() ||
        info.payload_buffers.size() > max_try_send_batch ||
        info.block_offset > std::numeric_limits<size_t>::max() - info.block_count) {
      return false;
    }
    const auto end_block = info.block_offset + info.block_count;
    if (end_block > std::numeric_limits<size_t>::max() / info.payload_size ||
        (info.header_size && end_block > std::numeric_limits<size_t>::max() / info.header_size)) {
      return false;
    }
    size_t total_payload = 0;
    for (const auto &desc : info.payload_buffers) {
      if (!desc.buffer || !desc.size || desc.size % info.payload_size ||
          desc.size > std::numeric_limits<size_t>::max() - total_payload) {
        return false;
      }
      total_payload += desc.size;
    }
    if (end_block * info.payload_size > total_payload) {
      return false;
    }
    size_t desc_index = 0;
    auto offset = info.block_offset * info.payload_size;
    for (size_t packet = 0; packet < info.block_count; ++packet) {
      while (offset >= info.payload_buffers[desc_index].size) {
        offset -= info.payload_buffers[desc_index++].size;
      }
      layout.payloads[packet] = info.payload_buffers[desc_index].buffer + offset;
      offset += info.payload_size;
    }
    layout.datagram_size = info.header_size + info.payload_size;
    return true;
  }

  inline boost::asio::ip::address_v4
  source_v4(const boost::asio::ip::address &source) {
    if (source.is_v4()) {
      return source.to_v4();
    }
    auto bytes = source.to_v6().to_bytes();
    return boost::asio::ip::address_v4({bytes[12], bytes[13], bytes[14], bytes[15]});
  }

#ifdef _WIN32
  inline udp_send_attempt_t
  native_failure(int error, bool batch) {
    if (error == WSA_IO_PENDING || error == WSAEINPROGRESS) {
      return unknown_submission(0, error);
    }
    if (error == WSAEWOULDBLOCK) {
      return {udp_send_status_e::would_block, 0, 0, error, true, true, 0};
    }
    if (error == WSAEINTR) {
      return {udp_send_status_e::interrupted, 0, 0, error, true, true, 0};
    }
    if (batch && (error == WSAENOPROTOOPT || error == WSAEOPNOTSUPP || error == WSAEPROTONOSUPPORT)) {
      return {udp_send_status_e::unsupported, 0, 0, error, false, true, 0};
    }
    return {udp_send_status_e::failed, 0, 0, error, error == WSAENOBUFS, true, 0};
  }

  struct native_calls_t {
    bool (*pin_source)(const boost::asio::ip::address &) = [](const boost::asio::ip::address &source) { return !source.is_unspecified(); };
    void (*on_failure)(int) = [](int) {};

    int
    nonblocking(SOCKET socket) {
      u_long mode = 1;
      return ioctlsocket(socket, FIONBIO, &mode);
    }

    int
    send_message(SOCKET socket, WSAMSG *message, DWORD *bytes) {
      return WSASendMsg(socket, message, 0, bytes, nullptr, nullptr);
    }

    int
    last_error() {
      return WSAGetLastError();
    }
  };

  struct message_context_t {
    WSAMSG msg {};
    SOCKADDR_IN target_v4 {};
    SOCKADDR_IN6 target_v6 {};
    std::array<WSABUF, max_try_send_batch * 2> buffers {};
    alignas(WSACMSGHDR) std::array<char,
      WSA_CMSG_SPACE(sizeof(DWORD)) +
      (WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)) > WSA_CMSG_SPACE(sizeof(IN_PKTINFO)) ?
         WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)) : WSA_CMSG_SPACE(sizeof(IN_PKTINFO)))> control {};

    void
    initialize(const boost::asio::ip::address &target, uint16_t port, const boost::asio::ip::address &source, size_t segment_size = 0, bool pin_source = true) {
      if (target.is_v6()) {
        target_v6.sin6_family = AF_INET6;
        target_v6.sin6_port = htons(port);
        target_v6.sin6_scope_id = static_cast<ULONG>(target.to_v6().scope_id());
        auto bytes = target.to_v6().to_bytes();
        std::memcpy(&target_v6.sin6_addr, bytes.data(), sizeof(target_v6.sin6_addr));
        msg.name = reinterpret_cast<SOCKADDR *>(&target_v6);
        msg.namelen = sizeof(target_v6);
      }
      else {
        target_v4.sin_family = AF_INET;
        target_v4.sin_port = htons(port);
        auto bytes = target.to_v4().to_bytes();
        std::memcpy(&target_v4.sin_addr, bytes.data(), sizeof(target_v4.sin_addr));
        msg.name = reinterpret_cast<SOCKADDR *>(&target_v4);
        msg.namelen = sizeof(target_v4);
      }
      msg.lpBuffers = buffers.data();
      msg.Control.buf = control.data();
      msg.Control.len = static_cast<ULONG>(control.size());
      auto *cm = WSA_CMSG_FIRSTHDR(&msg);
      ULONG used = 0;
      if (pin_source && !source.is_unspecified()) {
        if (source.is_v6() && !source.to_v6().is_v4_mapped()) {
          IN6_PKTINFO info {};
          auto bytes = source.to_v6().to_bytes();
          std::memcpy(&info.ipi6_addr, bytes.data(), sizeof(info.ipi6_addr));
          info.ipi6_ifindex = static_cast<ULONG>(source.to_v6().scope_id());
          cm->cmsg_level = IPPROTO_IPV6;
          cm->cmsg_type = IPV6_PKTINFO;
          cm->cmsg_len = WSA_CMSG_LEN(sizeof(info));
          std::memcpy(WSA_CMSG_DATA(cm), &info, sizeof(info));
          used = WSA_CMSG_SPACE(sizeof(info));
        }
        else {
          IN_PKTINFO info {};
          auto bytes = source_v4(source).to_bytes();
          std::memcpy(&info.ipi_addr, bytes.data(), sizeof(info.ipi_addr));
          cm->cmsg_level = IPPROTO_IP;
          cm->cmsg_type = IP_PKTINFO;
          cm->cmsg_len = WSA_CMSG_LEN(sizeof(info));
          std::memcpy(WSA_CMSG_DATA(cm), &info, sizeof(info));
          used = WSA_CMSG_SPACE(sizeof(info));
        }
      }
      if (segment_size) {
        cm = reinterpret_cast<WSACMSGHDR *>(control.data() + used);
        cm->cmsg_level = IPPROTO_UDP;
        cm->cmsg_type = UDP_SEND_MSG_SIZE;
        cm->cmsg_len = WSA_CMSG_LEN(sizeof(DWORD));
        const auto size = static_cast<DWORD>(segment_size);
        std::memcpy(WSA_CMSG_DATA(cm), &size, sizeof(size));
        used += WSA_CMSG_SPACE(sizeof(DWORD));
      }
      msg.Control.len = used;
      if (!used) {
        msg.Control.buf = nullptr;
      }
    }

    void
    append(const char *data, size_t size) {
      buffers[msg.dwBufferCount++] = {static_cast<ULONG>(size), const_cast<char *>(data)};
    }
  };

  template<class Calls>
  udp_send_attempt_t
  submit(std::uintptr_t socket, message_context_t &context, size_t packets, size_t expected_bytes, bool batch, Calls &calls) {
    // Winsock has no per-send MSG_DONTWAIT. A synchronous WSASendMsg inherits
    // FIONBIO even for an overlapped socket. The owner must forbid concurrent
    // socket mode changes/close; no blocking-mode assertion is trusted here.
    if (calls.nonblocking(static_cast<SOCKET>(socket)) == SOCKET_ERROR) {
      // This is a setup failure before submission, even if the error happens
      // to share a numeric value with an asynchronous send result.
      return {udp_send_status_e::failed, 0, 0, calls.last_error(), false, true, 0};
    }
    DWORD bytes = 0;
    if (calls.send_message(static_cast<SOCKET>(socket), &context.msg, &bytes) == SOCKET_ERROR) {
      const auto error = calls.last_error();
      if (error == WSAEINVAL) calls.on_failure(error);
      if (bytes) {
        return unknown_submission(bytes, error);
      }
      return native_failure(error, batch);
    }
    // UDP/USO sends are atomic. A short success is not a documented USO prefix
    // and cannot safely be rounded down, retried, or silently counted as zero.
    return bytes == expected_bytes ? complete(packets, bytes) : unknown_submission(bytes);
  }

  template<class Calls>
  udp_send_attempt_t
  try_send_impl(send_info_t &info, Calls &calls) {
    if (!valid_single(info)) {
      return invalid_request();
    }
    message_context_t context;
    context.initialize(info.target_address, info.target_port, info.source_address, 0, calls.pin_source(info.source_address));
    if (info.header_size) {
      context.append(info.header, info.header_size);
    }
    context.append(info.payload, info.payload_size);
    return submit(info.native_socket, context, 1, info.header_size + info.payload_size, false, calls);
  }

  template<class Calls>
  udp_send_attempt_t
  try_send_batch_impl(batched_send_info_t &info, Calls &calls) {
    info.submitted_blocks = 0;
    batch_layout_t layout;
    if (!validate_batch(info, layout)) {
      return invalid_request();
    }
    message_context_t context;
    context.initialize(info.target_address, info.target_port, info.source_address, info.block_count > 1 ? layout.datagram_size : 0,
      calls.pin_source(info.source_address));
    for (size_t i = 0; i < info.block_count; ++i) {
      if (info.header_size) {
        context.append(info.headers + (info.block_offset + i) * info.header_size, info.header_size);
      }
      context.append(layout.payloads[i], info.payload_size);
    }
    auto result = submit(info.native_socket, context, info.block_count, info.block_count * layout.datagram_size, info.block_count > 1, calls);
    info.submitted_blocks = result.submitted_datagrams;
    return result;
  }
#else
  inline udp_send_attempt_t
  native_failure(int error, bool batch) {
    if (error == EAGAIN || error == EWOULDBLOCK) {
      return {udp_send_status_e::would_block, 0, 0, error, true, true, 0};
    }
    if (error == EINTR) {
      return {udp_send_status_e::interrupted, 0, 0, error, true, true, 0};
    }
    if (batch && (error == ENOSYS || error == EOPNOTSUPP)) {
      return {udp_send_status_e::unsupported, 0, 0, error, false, true, 0};
    }
    return {udp_send_status_e::failed, 0, 0, error, error == ENOBUFS, true, 0};
  }

  struct native_calls_t {
    ssize_t
    send_message(int socket, const msghdr *message, int flags) {
      return sendmsg(socket, message, flags);
    }
#ifdef __linux__
    int
    send_messages(int socket, mmsghdr *messages, unsigned count, int flags) {
      return sendmmsg(socket, messages, count, flags);
    }
#endif
    int
    last_error() {
      return errno;
    }
  };

  struct message_context_t {
    msghdr msg {};
    sockaddr_in target_v4 {};
    sockaddr_in6 target_v6 {};
    std::array<iovec, 2> buffers {};
    alignas(cmsghdr) std::array<char,
      (CMSG_SPACE(sizeof(in6_pktinfo)) > CMSG_SPACE(sizeof(in_pktinfo)) ?
         CMSG_SPACE(sizeof(in6_pktinfo)) : CMSG_SPACE(sizeof(in_pktinfo)))> control {};

    void
    initialize(const boost::asio::ip::address &target, uint16_t port, const boost::asio::ip::address &source) {
      if (target.is_v6()) {
        target_v6.sin6_family = AF_INET6;
        target_v6.sin6_port = htons(port);
        target_v6.sin6_scope_id = static_cast<uint32_t>(target.to_v6().scope_id());
        auto bytes = target.to_v6().to_bytes();
        std::memcpy(&target_v6.sin6_addr, bytes.data(), sizeof(target_v6.sin6_addr));
#ifdef __APPLE__
        target_v6.sin6_len = sizeof(target_v6);
#endif
        msg.msg_name = &target_v6;
        msg.msg_namelen = sizeof(target_v6);
      }
      else {
        target_v4.sin_family = AF_INET;
        target_v4.sin_port = htons(port);
        auto bytes = target.to_v4().to_bytes();
        std::memcpy(&target_v4.sin_addr, bytes.data(), sizeof(target_v4.sin_addr));
#ifdef __APPLE__
        target_v4.sin_len = sizeof(target_v4);
#endif
        msg.msg_name = &target_v4;
        msg.msg_namelen = sizeof(target_v4);
      }
      msg.msg_iov = buffers.data();
      if (source.is_unspecified()) {
        return;
      }
      msg.msg_control = control.data();
      msg.msg_controllen = control.size();
      auto *cm = CMSG_FIRSTHDR(&msg);
      if (source.is_v6() && !source.to_v6().is_v4_mapped()) {
        in6_pktinfo info {};
        auto bytes = source.to_v6().to_bytes();
        std::memcpy(&info.ipi6_addr, bytes.data(), sizeof(info.ipi6_addr));
        info.ipi6_ifindex = static_cast<uint32_t>(source.to_v6().scope_id());
        cm->cmsg_level = IPPROTO_IPV6;
        cm->cmsg_type = IPV6_PKTINFO;
        cm->cmsg_len = CMSG_LEN(sizeof(info));
        std::memcpy(CMSG_DATA(cm), &info, sizeof(info));
        msg.msg_controllen = CMSG_SPACE(sizeof(info));
      }
      else {
        in_pktinfo info {};
        auto bytes = source_v4(source).to_bytes();
        std::memcpy(&info.ipi_spec_dst, bytes.data(), sizeof(info.ipi_spec_dst));
        cm->cmsg_level = IPPROTO_IP;
        cm->cmsg_type = IP_PKTINFO;
        cm->cmsg_len = CMSG_LEN(sizeof(info));
        std::memcpy(CMSG_DATA(cm), &info, sizeof(info));
        msg.msg_controllen = CMSG_SPACE(sizeof(info));
      }
    }

    void
    append(const char *data, size_t size) {
      buffers[msg.msg_iovlen++] = {const_cast<char *>(data), size};
    }
  };

  template<class Calls>
  udp_send_attempt_t
  try_send_impl(send_info_t &info, Calls &calls) {
    if (!valid_single(info) || info.native_socket > static_cast<std::uintptr_t>(std::numeric_limits<int>::max())) {
      return invalid_request();
    }
    message_context_t context;
    context.initialize(info.target_address, info.target_port, info.source_address);
    if (info.header_size) {
      context.append(info.header, info.header_size);
    }
    context.append(info.payload, info.payload_size);
    const auto sent = calls.send_message(static_cast<int>(info.native_socket), &context.msg, MSG_DONTWAIT);
    if (sent < 0) {
      return native_failure(calls.last_error(), false);
    }
    const auto bytes = static_cast<size_t>(sent);
    return bytes == info.header_size + info.payload_size ? complete(1, bytes) : unknown_submission(bytes);
  }

  template<class Calls>
  udp_send_attempt_t
  try_send_batch_impl(batched_send_info_t &info, Calls &calls) {
    info.submitted_blocks = 0;
    batch_layout_t layout;
    if (!validate_batch(info, layout) || info.native_socket > static_cast<std::uintptr_t>(std::numeric_limits<int>::max())) {
      return invalid_request();
    }
#ifdef __linux__
    std::array<message_context_t, max_try_send_batch> contexts;
    std::array<mmsghdr, max_try_send_batch> messages {};
    for (size_t i = 0; i < info.block_count; ++i) {
      contexts[i].initialize(info.target_address, info.target_port, info.source_address);
      if (info.header_size) {
        contexts[i].append(info.headers + (info.block_offset + i) * info.header_size, info.header_size);
      }
      contexts[i].append(layout.payloads[i], info.payload_size);
      messages[i].msg_hdr = contexts[i].msg;
    }
    const auto sent = calls.send_messages(static_cast<int>(info.native_socket), messages.data(), static_cast<unsigned>(info.block_count), MSG_DONTWAIT);
    if (sent < 0) {
      return native_failure(calls.last_error(), true);
    }
    auto result = interpret_batch_prefix(sent, info.block_count, layout.datagram_size, [&](size_t i) { return messages[i].msg_len; });
    info.submitted_blocks = result.submitted_datagrams;
    return result;
#else
    (void) calls;
    // No native multi-message API is assumed on macOS. The owner may perform
    // individually checked try_send calls; this function submits nothing.
    return {udp_send_status_e::unsupported, 0, 0, EOPNOTSUPP, false, true, 0};
#endif
  }
#endif
}  // namespace platf::udp_send_detail

/**
 * @file tests/unit/test_platform_udp_send.cpp
 * @brief Real UDP loopback and injected syscall tests of the production send implementation.
 */
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#endif
// The existing platform declarations contain unused virtual parameters and a
// legacy signed offset comparison. Keep strict warnings for the new code while
// avoiding unrelated changes to those declarations.
#ifdef __GNUC__
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wunused-parameter"
  #pragma GCC diagnostic ignored "-Wsign-compare"
  #pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "src/platform/common.h"
#ifdef __GNUC__
  #pragma GCC diagnostic pop
#endif
#include "src/platform/udp_send_impl.h"

#include <gtest/gtest.h>
#include <functional>
#include <string>
#include <vector>

namespace {
  using platf::udp_send_status_e;
  namespace detail = platf::udp_send_detail;

  struct request_t {
    boost::asio::ip::address target = boost::asio::ip::make_address("127.0.0.1");
    boost::asio::ip::address source = boost::asio::ip::address_v4::any();
    std::string headers = "ABCDEF";
    std::string payload = "000011112222333344445555";
    std::vector<platf::buffer_descriptor_t> buffers {{payload.data(), payload.size()}};

    platf::send_info_t
    single() {
      return {headers.data(), 2, payload.data(), 4, 42, target, 12345, source};
    }

    platf::batched_send_info_t
    batch(size_t offset = 0, size_t count = 3) {
      return {headers.data(), 1, buffers, 4, offset, count, 42, target, 12345, source};
    }
  };

  TEST(PlatformUdpLayout, BoundedBatchSelectsOffsetAcrossDescriptors) {
    request_t request;
    request.buffers = {{request.payload.data(), 8}, {request.payload.data() + 8, 16}};
    auto info = request.batch(1, 3);
    detail::batch_layout_t layout;
    ASSERT_TRUE(detail::validate_batch(info, layout));
    EXPECT_EQ(layout.datagram_size, 5u);
    EXPECT_EQ(std::string(layout.payloads[0], 4), "1111");
    EXPECT_EQ(std::string(layout.payloads[1], 4), "2222");
    EXPECT_EQ(std::string(layout.payloads[2], 4), "3333");
  }

  TEST(PlatformUdpLayout, MalformedDescriptorsAndIncompleteCoverageAreRejected) {
    request_t request;
    auto info = request.batch();
    detail::batch_layout_t layout;
    request.buffers = {{request.payload.data(), 7}};
    EXPECT_FALSE(detail::validate_batch(info, layout));
    request.buffers = {{request.payload.data(), 0}, {request.payload.data(), 24}};
    EXPECT_FALSE(detail::validate_batch(info, layout));
    request.buffers = {{nullptr, 24}};
    EXPECT_FALSE(detail::validate_batch(info, layout));
    request.buffers = {{request.payload.data(), 8}};
    EXPECT_FALSE(detail::validate_batch(info, layout));
    request.buffers.clear();
    EXPECT_FALSE(detail::validate_batch(info, layout));
  }

  TEST(PlatformUdpLayout, OverflowAndBatchBoundsAreRejected) {
    request_t request;
    detail::batch_layout_t layout;
    auto info = request.batch(std::numeric_limits<size_t>::max(), 2);
    EXPECT_FALSE(detail::validate_batch(info, layout));
    info.block_offset = std::numeric_limits<size_t>::max() / 4;
    info.block_count = 1;
    EXPECT_FALSE(detail::validate_batch(info, layout));
    info.block_offset = 0;
    info.block_count = platf::max_try_send_batch + 1;
    EXPECT_FALSE(detail::validate_batch(info, layout));
    info.block_count = 0;
    EXPECT_FALSE(detail::validate_batch(info, layout));
    info.block_count = 1;
    request.buffers.assign(platf::max_try_send_batch + 1, {request.payload.data(), 4});
    EXPECT_FALSE(detail::validate_batch(info, layout));
  }

  TEST(PlatformUdpLayout, InvalidPointersSizesAndAddressFamiliesAreRejected) {
    request_t request;
    auto single = request.single();
    single.payload = nullptr;
    EXPECT_FALSE(detail::valid_single(single));
    single.payload = request.payload.data();
    single.header = nullptr;
    EXPECT_FALSE(detail::valid_single(single));
    single.header = request.headers.data();
    single.payload_size = platf::max_try_send_udp_payload;
    EXPECT_FALSE(detail::valid_single(single));
    request.source = boost::asio::ip::make_address("::1");
    EXPECT_FALSE(detail::valid_single(request.single()));
    request.source = boost::asio::ip::address_v4::any();
    single.payload_size = 4;
    single.target_port = 0;
    EXPECT_FALSE(detail::valid_single(single));
    request.target = boost::asio::ip::address_v4::any();
    EXPECT_FALSE(detail::valid_single(request.single()));
  }

  TEST(PlatformUdpPrefix, PositiveShortBatchConfirmsOnlySuccessfulPrefix) {
    std::array<unsigned, 4> lengths {1234, 1234, 999, 888};
    auto result = detail::interpret_batch_prefix(2, 4, 1234, [&](size_t i) { return lengths[i]; });
    EXPECT_EQ(result.status, udp_send_status_e::partial);
    EXPECT_EQ(result.submitted_datagrams, 2u);
    EXPECT_EQ(result.submitted_payload_bytes, 2468u);
    EXPECT_EQ(result.reported_payload_bytes, 2468u);
    EXPECT_EQ(result.native_error, 0);  // No invented errno for a positive return.
    EXPECT_TRUE(result.submission_known);
    EXPECT_TRUE(result.retryable);
  }

  TEST(PlatformUdpPrefix, CompleteBatchAccountsEveryWholeDatagram) {
    std::array<unsigned, 3> lengths {1200, 1200, 1200};
    auto result = detail::interpret_batch_prefix(3, 3, 1200, [&](size_t i) { return lengths[i]; });
    EXPECT_EQ(result.status, udp_send_status_e::complete);
    EXPECT_EQ(result.submitted_datagrams, 3u);
    EXPECT_EQ(result.submitted_payload_bytes, 3600u);
    EXPECT_FALSE(result.retryable);
  }

  TEST(PlatformUdpPrefix, MalformedNativeLengthRetainsConfirmedPrefixAndMarksRestUnknown) {
    std::array<unsigned, 3> lengths {1200, 1, 1200};
    auto result = detail::interpret_batch_prefix(3, 3, 1200, [&](size_t i) { return lengths[i]; });
    EXPECT_EQ(result.status, udp_send_status_e::unknown_submission);
    EXPECT_FALSE(result.submission_known);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(result.submitted_datagrams, 1u);
    EXPECT_EQ(result.submitted_payload_bytes, 1200u);
    EXPECT_EQ(result.reported_payload_bytes, 2401u);
  }

  TEST(PlatformUdpPrefix, ImpossibleNativeCountDoesNotReadOutsideBoundedArray) {
    size_t reads = 0;
    auto result = detail::interpret_batch_prefix(33, 32, 1200, [&](size_t) {
      ++reads;
      return 1200;
    });
    EXPECT_EQ(result.status, udp_send_status_e::unknown_submission);
    EXPECT_FALSE(result.submission_known);
    EXPECT_EQ(reads, 0u);
  }

  TEST(PlatformUdpPrefix, NoProgressReturnsToOwnerWithoutInternalRetry) {
    size_t reads = 0;
    auto result = detail::interpret_batch_prefix(0, 32, 1200, [&](size_t) {
      ++reads;
      return 1200;
    });
    EXPECT_EQ(result.status, udp_send_status_e::partial);
    EXPECT_TRUE(result.submission_known);
    EXPECT_TRUE(result.retryable);
    EXPECT_EQ(result.submitted_datagrams, 0u);
    EXPECT_EQ(reads, 0u);
  }

#ifdef _WIN32
  struct fake_calls_t {
    int mode_result = 0;
    int send_result = 0;
    int error = WSAEWOULDBLOCK;
    std::optional<DWORD> report_bytes;
    int mode_calls = 0;
    int send_calls = 0;
    std::string captured;
    std::function<void(const WSAMSG &)> inspect;

    int
    nonblocking(SOCKET) {
      ++mode_calls;
      return mode_result;
    }

    int
    send_message(SOCKET, WSAMSG *message, DWORD *bytes) {
      ++send_calls;
      EXPECT_EQ(mode_calls, 1);
      EXPECT_EQ(message->dwFlags, 0u);
      DWORD total = 0;
      for (DWORD i = 0; i < message->dwBufferCount; ++i) {
        total += message->lpBuffers[i].len;
        if (message->lpBuffers[i].len) {
          captured.append(message->lpBuffers[i].buf, message->lpBuffers[i].len);
        }
      }
      if (inspect) {
        inspect(*message);
      }
      *bytes = report_bytes.value_or(send_result == SOCKET_ERROR ? 0 : total);
      return send_result;
    }

    int
    last_error() {
      return error;
    }
  };

  TEST(PlatformUdpSyscall, CompleteSingleCountsWholePrefixAndPayload) {
    request_t request;
    auto info = request.single();
    fake_calls_t calls;
    auto result = detail::try_send_impl(info, calls);
    EXPECT_EQ(result.status, udp_send_status_e::complete);
    EXPECT_TRUE(result.submission_known);
    EXPECT_EQ(result.submitted_datagrams, 1u);
    EXPECT_EQ(result.submitted_payload_bytes, 6u);
    EXPECT_EQ(result.reported_payload_bytes, 6u);
    EXPECT_EQ(calls.captured, "AB0000");
    EXPECT_EQ(calls.send_calls, 1);
  }

  TEST(PlatformUdpSyscall, CompleteOwnedPayloadAddsNoHeader) {
    request_t request;
    auto info = request.single();
    info.header = nullptr;
    info.header_size = 0;
    fake_calls_t calls;
    calls.inspect = [](const WSAMSG &msg) { EXPECT_EQ(msg.dwBufferCount, 1u); };
    auto result = detail::try_send_impl(info, calls);
    EXPECT_EQ(result.submitted_payload_bytes, 4u);
    EXPECT_EQ(calls.captured, "0000");
  }

  TEST(PlatformUdpSyscall, WouldBlockIsKnownZeroAndNeverWaitsOrRetries) {
    request_t request;
    auto info = request.single();
    fake_calls_t calls;
    calls.send_result = SOCKET_ERROR;
    auto result = detail::try_send_impl(info, calls);
    EXPECT_EQ(result.status, udp_send_status_e::would_block);
    EXPECT_EQ(result.native_error, WSAEWOULDBLOCK);
    EXPECT_TRUE(result.retryable);
    EXPECT_TRUE(result.submission_known);
    EXPECT_EQ(result.submitted_datagrams, 0u);
    EXPECT_EQ(calls.send_calls, 1);
  }

  TEST(PlatformUdpSyscall, InterruptedAndResourcePressureAreReturnedToOwner) {
    request_t request;
    auto info = request.single();
    fake_calls_t calls;
    calls.send_result = SOCKET_ERROR;
    calls.error = WSAEINTR;
    auto result = detail::try_send_impl(info, calls);
    EXPECT_EQ(result.status, udp_send_status_e::interrupted);
    EXPECT_TRUE(result.retryable);
    EXPECT_EQ(calls.send_calls, 1);
    calls = {};
    calls.send_result = SOCKET_ERROR;
    calls.error = WSAENOBUFS;
    result = detail::try_send_impl(info, calls);
    EXPECT_EQ(result.status, udp_send_status_e::failed);
    EXPECT_TRUE(result.retryable);
    EXPECT_TRUE(result.submission_known);
    EXPECT_EQ(calls.send_calls, 1);
  }

  TEST(PlatformUdpSyscall, AtomicMessageTooLargeIsKnownZeroAndTerminal) {
    request_t request;
    auto info = request.single();
    fake_calls_t calls;
    calls.send_result = SOCKET_ERROR;
    calls.error = WSAEMSGSIZE;
    auto result = detail::try_send_impl(info, calls);
    EXPECT_EQ(result.status, udp_send_status_e::failed);
    EXPECT_FALSE(result.retryable);
    EXPECT_TRUE(result.submission_known);
    EXPECT_EQ(result.submitted_payload_bytes, 0u);
  }

  TEST(PlatformUdpSyscall, NonblockingSetupFailureSubmitsNothing) {
    request_t request;
    auto info = request.single();
    fake_calls_t calls;
    calls.mode_result = SOCKET_ERROR;
    calls.error = WSA_IO_PENDING;
    auto result = detail::try_send_impl(info, calls);
    EXPECT_EQ(result.status, udp_send_status_e::failed);
    EXPECT_EQ(result.native_error, WSA_IO_PENDING);
    EXPECT_TRUE(result.submission_known);
    EXPECT_EQ(calls.send_calls, 0);
    EXPECT_EQ(calls.mode_calls, 1);
  }

  TEST(PlatformUdpSyscall, PendingAndInProgressSendCannotPretendZero) {
    for (int error : std::array<int, 2> {WSA_IO_PENDING, WSAEINPROGRESS}) {
      request_t request;
      auto info = request.single();
      fake_calls_t calls;
      calls.send_result = SOCKET_ERROR;
      calls.error = error;
      auto result = detail::try_send_impl(info, calls);
      EXPECT_EQ(result.status, udp_send_status_e::unknown_submission);
      EXPECT_FALSE(result.submission_known);
      EXPECT_FALSE(result.retryable);
      EXPECT_EQ(result.native_error, error);
      EXPECT_EQ(calls.send_calls, 1);
    }
  }

  TEST(PlatformUdpSyscall, ShortSingleSuccessIsUnknownAndPreservesRawBytes) {
    request_t request;
    auto info = request.single();
    fake_calls_t calls;
    calls.report_bytes = 5;
    auto result = detail::try_send_impl(info, calls);
    EXPECT_EQ(result.status, udp_send_status_e::unknown_submission);
    EXPECT_FALSE(result.submission_known);
    EXPECT_EQ(result.reported_payload_bytes, 5u);
    EXPECT_FALSE(result.retryable);
  }

  TEST(PlatformUdpSyscall, ShortUsoSuccessCannotBeRoundedToDatagramPrefix) {
    request_t request;
    auto info = request.batch();
    fake_calls_t calls;
    calls.report_bytes = 10;  // Two whole-sized segments of the requested three.
    auto result = detail::try_send_batch_impl(info, calls);
    EXPECT_EQ(result.status, udp_send_status_e::unknown_submission);
    EXPECT_FALSE(result.submission_known);
    EXPECT_EQ(result.reported_payload_bytes, 10u);
    EXPECT_EQ(info.submitted_blocks, 0u);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(calls.send_calls, 1);
  }

  TEST(PlatformUdpSyscall, NonzeroBytesWithErrorIsUnknown) {
    request_t request;
    auto info = request.batch();
    fake_calls_t calls;
    calls.send_result = SOCKET_ERROR;
    calls.error = WSAEOPNOTSUPP;
    calls.report_bytes = 5;
    auto result = detail::try_send_batch_impl(info, calls);
    EXPECT_EQ(result.status, udp_send_status_e::unknown_submission);
    EXPECT_FALSE(result.submission_known);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(result.reported_payload_bytes, 5u);
  }

  TEST(PlatformUdpSyscall, OversizedNativeByteCountIsUnknown) {
    request_t request;
    auto info = request.batch();
    fake_calls_t calls;
    calls.report_bytes = 100;
    auto result = detail::try_send_batch_impl(info, calls);
    EXPECT_EQ(result.status, udp_send_status_e::unknown_submission);
    EXPECT_EQ(result.reported_payload_bytes, 100u);
    EXPECT_FALSE(result.submission_known);
  }

  TEST(PlatformUdpSyscall, UsoUnsupportedIsKnownZeroWithoutInternalFallback) {
    for (int error : {WSAENOPROTOOPT, WSAEOPNOTSUPP, WSAEPROTONOSUPPORT}) {
      request_t request;
      auto info = request.batch();
      info.submitted_blocks = 100;
      fake_calls_t calls;
      calls.send_result = SOCKET_ERROR;
      calls.error = error;
      auto result = detail::try_send_batch_impl(info, calls);
      EXPECT_EQ(result.status, udp_send_status_e::unsupported);
      EXPECT_TRUE(result.submission_known);
      EXPECT_EQ(info.submitted_blocks, 0u);
      EXPECT_EQ(calls.send_calls, 1);
    }
  }

  TEST(PlatformUdpSyscall, InvalidSourceErrorDoesNotMasqueradeAsUsoUnsupported) {
    request_t request;
    request.source = boost::asio::ip::make_address("192.0.2.199");
    auto info = request.batch();
    fake_calls_t calls;
    calls.send_result = SOCKET_ERROR;
    calls.error = WSAEINVAL;
    auto result = detail::try_send_batch_impl(info, calls);
    EXPECT_EQ(result.status, udp_send_status_e::failed);
    EXPECT_TRUE(result.submission_known);
    EXPECT_FALSE(result.retryable);
    EXPECT_EQ(result.native_error, WSAEINVAL);
  }

  TEST(PlatformUdpSyscall, CompleteBatchUsesAlignedOffsetAndAllUdpBytes) {
    request_t request;
    request.buffers = {{request.payload.data(), 8}, {request.payload.data() + 8, 16}};
    auto info = request.batch(1, 3);
    fake_calls_t calls;
    auto result = detail::try_send_batch_impl(info, calls);
    EXPECT_EQ(result.status, udp_send_status_e::complete);
    EXPECT_EQ(result.submitted_datagrams, 3u);
    EXPECT_EQ(result.submitted_payload_bytes, 15u);
    EXPECT_EQ(info.submitted_blocks, 3u);
    EXPECT_EQ(calls.captured, "B1111C2222D3333");
    EXPECT_EQ(calls.send_calls, 1);
  }

  TEST(PlatformUdpSyscall, MaximumBatchRemainsOneCallWithFixedBuffers) {
    request_t request;
    request.payload.assign(platf::max_try_send_batch * 4, 'p');
    request.headers.assign(platf::max_try_send_batch, 'h');
    request.buffers = {{request.payload.data(), request.payload.size()}};
    auto info = request.batch(0, platf::max_try_send_batch);
    fake_calls_t calls;
    calls.inspect = [](const WSAMSG &msg) { EXPECT_EQ(msg.dwBufferCount, 64u); };
    auto result = detail::try_send_batch_impl(info, calls);
    EXPECT_EQ(result.submitted_datagrams, platf::max_try_send_batch);
    EXPECT_EQ(result.submitted_payload_bytes, 160u);
    EXPECT_EQ(calls.send_calls, 1);
  }

  TEST(PlatformUdpSyscall, InvalidBatchNeverTouchesSocket) {
    request_t request;
    auto info = request.batch();
    info.headers = nullptr;
    fake_calls_t calls;
    auto result = detail::try_send_batch_impl(info, calls);
    EXPECT_EQ(result.status, udp_send_status_e::invalid_request);
    EXPECT_TRUE(result.submission_known);
    EXPECT_EQ(calls.mode_calls, 0);
    EXPECT_EQ(calls.send_calls, 0);
  }

  TEST(PlatformUdpSyscall, EmptySingleRequestIsKnownZeroWithoutSyscall) {
    request_t request;
    auto info = request.single();
    info.header_size = 0;
    info.payload_size = 0;
    fake_calls_t calls;
    auto result = detail::try_send_impl(info, calls);
    EXPECT_EQ(result.status, udp_send_status_e::invalid_request);
    EXPECT_TRUE(result.submission_known);
    EXPECT_EQ(calls.mode_calls, 0);
    EXPECT_EQ(calls.send_calls, 0);
  }

  TEST(PlatformUdpSyscall, SourceIpv4PktinfoAndUsoRemainSeparateControlMessages) {
    request_t request;
    request.source = request.target;
    auto info = request.batch();
    fake_calls_t calls;
    calls.inspect = [](const WSAMSG &input) {
      auto *msg = const_cast<WSAMSG *>(&input);
      auto *cm = WSA_CMSG_FIRSTHDR(msg);
      ASSERT_NE(cm, nullptr);
      EXPECT_EQ(cm->cmsg_level, IPPROTO_IP);
      EXPECT_EQ(cm->cmsg_type, IP_PKTINFO);
      IN_PKTINFO source;
      std::memcpy(&source, WSA_CMSG_DATA(cm), sizeof(source));
      EXPECT_EQ(source.ipi_addr.s_addr, htonl(INADDR_LOOPBACK));
      EXPECT_EQ(source.ipi_ifindex, 0u);
      cm = WSA_CMSG_NXTHDR(msg, cm);
      ASSERT_NE(cm, nullptr);
      EXPECT_EQ(cm->cmsg_level, IPPROTO_UDP);
      EXPECT_EQ(cm->cmsg_type, UDP_SEND_MSG_SIZE);
      DWORD size;
      std::memcpy(&size, WSA_CMSG_DATA(cm), sizeof(size));
      EXPECT_EQ(size, 5u);
      EXPECT_EQ(WSA_CMSG_NXTHDR(msg, cm), nullptr);
    };
    EXPECT_EQ(detail::try_send_batch_impl(info, calls).status, udp_send_status_e::complete);
  }

  TEST(PlatformUdpSyscall, Ipv6ScopeAndSourcePktinfoArePreserved) {
    request_t request;
    request.target = boost::asio::ip::make_address("fe80::2%3");
    request.source = boost::asio::ip::make_address("fe80::1%3");
    auto info = request.single();
    fake_calls_t calls;
    calls.inspect = [](const WSAMSG &input) {
      auto *msg = const_cast<WSAMSG *>(&input);
      auto *address = reinterpret_cast<const SOCKADDR_IN6 *>(msg->name);
      EXPECT_EQ(address->sin6_family, AF_INET6);
      EXPECT_EQ(address->sin6_scope_id, 3u);
      EXPECT_EQ(ntohs(address->sin6_port), 12345);
      auto *cm = WSA_CMSG_FIRSTHDR(msg);
      ASSERT_NE(cm, nullptr);
      EXPECT_EQ(cm->cmsg_level, IPPROTO_IPV6);
      EXPECT_EQ(cm->cmsg_type, IPV6_PKTINFO);
      IN6_PKTINFO source;
      std::memcpy(&source, WSA_CMSG_DATA(cm), sizeof(source));
      EXPECT_EQ(source.ipi6_ifindex, 3u);
      EXPECT_EQ(source.ipi6_addr.u.Byte[15], 1u);
    };
    EXPECT_EQ(detail::try_send_impl(info, calls).status, udp_send_status_e::complete);
  }

  TEST(PlatformUdpSyscall, V4MappedDualStackUsesIpv4Pktinfo) {
    request_t request;
    request.target = boost::asio::ip::make_address("::ffff:127.0.0.1");
    request.source = request.target;
    auto info = request.single();
    fake_calls_t calls;
    calls.inspect = [](const WSAMSG &input) {
      auto *msg = const_cast<WSAMSG *>(&input);
      EXPECT_EQ(msg->name->sa_family, AF_INET6);
      auto *cm = WSA_CMSG_FIRSTHDR(msg);
      ASSERT_NE(cm, nullptr);
      EXPECT_EQ(cm->cmsg_level, IPPROTO_IP);
      EXPECT_EQ(cm->cmsg_type, IP_PKTINFO);
    };
    EXPECT_EQ(detail::try_send_impl(info, calls).status, udp_send_status_e::complete);
  }

  TEST(PlatformUdpSyscall, UnspecifiedSourceDoesNotPinAndSingleBatchOmitsUso) {
    request_t request;
    auto info = request.batch(0, 1);
    fake_calls_t calls;
    calls.inspect = [](const WSAMSG &msg) {
      EXPECT_EQ(msg.Control.len, 0u);
      EXPECT_EQ(msg.Control.buf, nullptr);
    };
    EXPECT_EQ(detail::try_send_batch_impl(info, calls).submitted_datagrams, 1u);
  }

  class PlatformUdpLoopback: public testing::Test {
  protected:
    static void
    SetUpTestSuite() {
      WSADATA data;
      ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
    }

    static void
    TearDownTestSuite() {
      WSACleanup();
    }

    struct socket_pair_t {
      SOCKET sender = INVALID_SOCKET;
      SOCKET receiver = INVALID_SOCKET;
      boost::asio::ip::address address;
      uint16_t port = 0;

      explicit socket_pair_t(bool ipv6 = false):
          address(boost::asio::ip::make_address(ipv6 ? "::1" : "127.0.0.1")) {
        const auto family = ipv6 ? AF_INET6 : AF_INET;
        sender = socket(family, SOCK_DGRAM, IPPROTO_UDP);
        receiver = socket(family, SOCK_DGRAM, IPPROTO_UDP);
        if (sender == INVALID_SOCKET || receiver == INVALID_SOCKET) {
          return;
        }
        detail::message_context_t context;
        context.initialize(address, 0, boost::asio::ip::address_v4::any());
        if (bind(receiver, context.msg.name, context.msg.namelen)) {
          return;
        }
        sockaddr_storage bound {};
        int length = sizeof(bound);
        if (getsockname(receiver, reinterpret_cast<sockaddr *>(&bound), &length)) {
          return;
        }
        port = ipv6 ? ntohs(reinterpret_cast<sockaddr_in6 *>(&bound)->sin6_port) : ntohs(reinterpret_cast<sockaddr_in *>(&bound)->sin_port);
        DWORD timeout = 500;
        setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
      }

      ~socket_pair_t() {
        if (sender != INVALID_SOCKET) {
          closesocket(sender);
        }
        if (receiver != INVALID_SOCKET) {
          closesocket(receiver);
        }
      }

      std::string
      receive() {
        std::array<char, 4096> buffer {};
        const auto count = recv(receiver, buffer.data(), static_cast<int>(buffer.size()), 0);
        EXPECT_GE(count, 0) << WSAGetLastError();
        return count >= 0 ? std::string(buffer.data(), count) : std::string();
      }
    };
  };

  TEST_F(PlatformUdpLoopback, Ipv4PrefixAndPinnedSourceArriveAsOneDatagram) {
    socket_pair_t sockets;
    ASSERT_NE(sockets.port, 0);
    std::string header = "PREFIX";
    std::string payload = "owned encrypted UDP bytes";
    auto source = sockets.address;
    platf::send_info_t info {header.data(), header.size(), payload.data(), payload.size(), sockets.sender, sockets.address, sockets.port, source};
    detail::native_calls_t calls;
    auto result = detail::try_send_impl(info, calls);
    ASSERT_EQ(result.status, udp_send_status_e::complete) << result.native_error;
    EXPECT_EQ(result.submitted_payload_bytes, header.size() + payload.size());
    EXPECT_EQ(sockets.receive(), header + payload);
    DWORD timeout = 50;
    setsockopt(sockets.sender, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    char unused;
    EXPECT_EQ(recv(sockets.sender, &unused, 1, 0), SOCKET_ERROR);
    EXPECT_EQ(WSAGetLastError(), WSAEWOULDBLOCK);  // The initially blocking socket was made nonblocking.
  }

  TEST_F(PlatformUdpLoopback, Ipv6OwnedDatagramArrivesWithoutAdditionalPrefix) {
    socket_pair_t sockets(true);
    ASSERT_NE(sockets.port, 0);
    std::string payload = "complete encrypted IPv6 datagram";
    auto source = sockets.address;
    platf::send_info_t info {nullptr, 0, payload.data(), payload.size(), sockets.sender, sockets.address, sockets.port, source};
    detail::native_calls_t calls;
    auto result = detail::try_send_impl(info, calls);
    ASSERT_EQ(result.status, udp_send_status_e::complete) << result.native_error;
    EXPECT_EQ(sockets.receive(), payload);
  }

  TEST_F(PlatformUdpLoopback, UsoBatchProducesDistinctHeaderAndPayloadDatagrams) {
    socket_pair_t sockets;
    ASSERT_NE(sockets.port, 0);
    request_t request;
    request.target = sockets.address;
    request.source = sockets.address;
    auto info = request.batch(1, 3);
    info.native_socket = sockets.sender;
    info.target_port = sockets.port;
    detail::native_calls_t calls;
    auto result = detail::try_send_batch_impl(info, calls);
    ASSERT_EQ(result.status, udp_send_status_e::complete) << result.native_error;
    EXPECT_EQ(result.submitted_datagrams, 3u);
    EXPECT_EQ(result.submitted_payload_bytes, 15u);
    EXPECT_EQ(sockets.receive(), "B1111");
    EXPECT_EQ(sockets.receive(), "C2222");
    EXPECT_EQ(sockets.receive(), "D3333");
  }

  TEST_F(PlatformUdpLoopback, UnpinnedIpv4OwnedPayloadArrivesWithoutAdditionalPrefix) {
    socket_pair_t sockets;
    ASSERT_NE(sockets.port, 0);
    std::string payload = "complete owned UDP payload";
    boost::asio::ip::address source_address = boost::asio::ip::address_v4::any();
    platf::send_info_t info {nullptr, 0, payload.data(), payload.size(), sockets.sender, sockets.address, sockets.port, source_address};
    detail::native_calls_t calls;
    auto result = detail::try_send_impl(info, calls);
    ASSERT_EQ(result.status, udp_send_status_e::complete) << result.native_error;
    EXPECT_EQ(result.submitted_datagrams, 1u);
    EXPECT_EQ(result.submitted_payload_bytes, payload.size());
    EXPECT_EQ(sockets.receive(), payload);
  }

  TEST_F(PlatformUdpLoopback, ClosedSocketIsKnownZeroWithoutSubmission) {
    socket_pair_t sockets;
    ASSERT_NE(sockets.port, 0);
    closesocket(sockets.sender);
    sockets.sender = INVALID_SOCKET;
    std::string payload = "data";
    auto source = sockets.address;
    platf::send_info_t info {nullptr, 0, payload.data(), payload.size(), INVALID_SOCKET, sockets.address, sockets.port, source};
    detail::native_calls_t calls;
    auto result = detail::try_send_impl(info, calls);
    EXPECT_EQ(result.status, udp_send_status_e::failed);
    EXPECT_EQ(result.native_error, WSAENOTSOCK);
    EXPECT_TRUE(result.submission_known);
    EXPECT_EQ(result.submitted_datagrams, 0u);
  }

  TEST_F(PlatformUdpLoopback, UnconfiguredPinnedSourceFailsWithoutSilentlyChangingSource) {
    socket_pair_t sockets;
    ASSERT_NE(sockets.port, 0);
    std::string payload = "data";
    auto source = boost::asio::ip::make_address("192.0.2.199");
    platf::send_info_t info {nullptr, 0, payload.data(), payload.size(), sockets.sender, sockets.address, sockets.port, source};
    detail::native_calls_t calls;
    auto result = detail::try_send_impl(info, calls);
    EXPECT_EQ(result.status, udp_send_status_e::failed);
    EXPECT_TRUE(result.submission_known);
    EXPECT_NE(result.native_error, 0);
    EXPECT_EQ(result.submitted_datagrams, 0u);
    EXPECT_EQ(source.to_string(), "192.0.2.199");
  }
#endif
}  // namespace

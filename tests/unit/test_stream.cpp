/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <src/stream.h>

namespace stream {
  std::vector<uint8_t>
  concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
}

#include "../tests_common.h"

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  char b1[] = { 'a', 'b' };
  char b2[] = { 'c', 'd', 'e' };
  auto res = stream::concat_and_insert(0, 2, std::string_view { b1, sizeof(b1) }, std::string_view { b2, sizeof(b2) });
  auto expected = std::vector<uint8_t> { 'a', 'b', 'c', 'd', 'e' };
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  char b1[] = { 'a', 'b' };
  char b2[] = { 'c', 'd', 'e' };
  auto res = stream::concat_and_insert(1, sizeof(b1) + sizeof(b2) + 1, std::string_view { b1, sizeof(b1) }, std::string_view { b2, sizeof(b2) });
  auto expected = std::vector<uint8_t> { 0, 'a', 'b', 'c', 'd', 'e' };
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  char b1[] = { 'a', 'b' };
  char b2[] = { 'c', 'd', 'e' };
  auto res = stream::concat_and_insert(1, 1, std::string_view { b1, sizeof(b1) }, std::string_view { b2, sizeof(b2) });
  auto expected = std::vector<uint8_t> { 0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e' };
  ASSERT_EQ(res, expected);
}

TEST(VideoRtpTimestampTests, UsesNinetyKilohertzClock) {
  const auto epoch = std::chrono::steady_clock::time_point {};

  EXPECT_EQ(stream::video_rtp_timestamp(epoch + std::chrono::seconds(1), epoch), 90'000u);
  EXPECT_EQ(stream::video_rtp_timestamp(epoch + std::chrono::milliseconds(20), epoch), 1'800u);
}

TEST(VideoRtpTimestampTests, RoundsBeforeApplyingRtpWraparound) {
  const auto epoch = std::chrono::steady_clock::time_point {};

  EXPECT_EQ(stream::video_rtp_timestamp(epoch - std::chrono::microseconds(1), epoch), 0u);
  EXPECT_EQ(
    stream::video_rtp_timestamp(epoch - std::chrono::milliseconds(20), epoch),
    std::numeric_limits<std::uint32_t>::max() - 1'799u);
}

TEST(VideoRtpTimestampTests, WrapsForwardPastUint32Max) {
  using rtp_tick = std::chrono::duration<std::int64_t, std::ratio<1, 90'000>>;
  const auto epoch = std::chrono::steady_clock::time_point {};
  const auto wrap_time = epoch +
                         std::chrono::round<std::chrono::steady_clock::duration>(
                           rtp_tick { std::int64_t { 1 } << 32 });

  EXPECT_EQ(stream::video_rtp_timestamp(wrap_time, epoch), 0u);
}

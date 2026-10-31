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

#include <src/config.h>
#include <src/display_device/parsed_config.h>
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

namespace {
  config::video_t
  client_resolution_change_config() {
    auto video = config::video;
    video.allow_client_resolution_change = true;
    video.dynamic_resolution_follow_display = true;
    video.resolution_change = static_cast<int>(display_device::parsed_config_t::resolution_change_e::automatic);
    return video;
  }
}  // namespace

TEST(ClientResolutionChangeTests, EnabledOnlyWithOptInFollowDisplayAutomaticResolutionChangeAndSops) {
#ifdef _WIN32
  EXPECT_TRUE(stream::client_resolution_change_enabled(client_resolution_change_config(), true));
#else
  // Live display mode switching is Windows-only, so the feature flag is never advertised elsewhere.
  EXPECT_FALSE(stream::client_resolution_change_enabled(client_resolution_change_config(), true));
#endif

  EXPECT_FALSE(stream::client_resolution_change_enabled(client_resolution_change_config(), false));

  auto video = client_resolution_change_config();
  video.allow_client_resolution_change = false;
  EXPECT_FALSE(stream::client_resolution_change_enabled(video, true));

  video = client_resolution_change_config();
  video.dynamic_resolution_follow_display = false;
  EXPECT_FALSE(stream::client_resolution_change_enabled(video, true));

  video = client_resolution_change_config();
  video.resolution_change = static_cast<int>(display_device::parsed_config_t::resolution_change_e::manual);
  EXPECT_FALSE(stream::client_resolution_change_enabled(video, true));
  video.resolution_change = static_cast<int>(display_device::parsed_config_t::resolution_change_e::no_operation);
  EXPECT_FALSE(stream::client_resolution_change_enabled(video, true));
}

TEST(ClientResolutionChangeTests, EnabledByDefault) {
  // Clients such as Moonlight V+ already send 0x5506 RESOLUTION without checking the flag
  EXPECT_TRUE(config::video.allow_client_resolution_change);
#ifdef _WIN32
  // The other defaults (follow display, automatic resolution change) allow it too
  EXPECT_TRUE(stream::client_resolution_change_enabled(config::video, true));
#else
  EXPECT_FALSE(stream::client_resolution_change_enabled(config::video, true));
#endif
  EXPECT_FALSE(stream::client_resolution_change_enabled(config::video, false));
}

TEST(ClientResolutionChangeTests, NormalizeKeepsValidEvenSizes) {
  EXPECT_EQ(stream::normalize_client_resolution_request(1920, 1080), std::make_pair(1920, 1080));
  EXPECT_EQ(stream::normalize_client_resolution_request(1080, 2400), std::make_pair(1080, 2400));
  EXPECT_EQ(stream::normalize_client_resolution_request(256, 256), std::make_pair(256, 256));
  EXPECT_EQ(stream::normalize_client_resolution_request(16384, 16384), std::make_pair(16384, 16384));
}

TEST(ClientResolutionChangeTests, NormalizeRoundsOddSizesDownToEven) {
  EXPECT_EQ(stream::normalize_client_resolution_request(1463, 911), std::make_pair(1462, 910));
  EXPECT_EQ(stream::normalize_client_resolution_request(257, 16385), std::make_pair(256, 16384));
}

TEST(ClientResolutionChangeTests, NormalizeRejectsOutOfRangeSizes) {
  EXPECT_FALSE(stream::normalize_client_resolution_request(0, 1080));
  EXPECT_FALSE(stream::normalize_client_resolution_request(1920, -1080));
  EXPECT_FALSE(stream::normalize_client_resolution_request(255, 1080));  // rounds to 254
  EXPECT_FALSE(stream::normalize_client_resolution_request(1920, 254));
  EXPECT_FALSE(stream::normalize_client_resolution_request(16386, 1080));
  EXPECT_FALSE(stream::normalize_client_resolution_request(std::numeric_limits<int>::max(), 1080));
  EXPECT_FALSE(stream::normalize_client_resolution_request(std::numeric_limits<int>::min(), 1080));
}
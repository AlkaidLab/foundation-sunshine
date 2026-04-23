/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace stream {
  std::vector<uint8_t>
  concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);

  bool
  clipboard_transfer_length_valid(uint8_t item_type, std::uint32_t total_length);

  bool
  clipboard_transfer_chunk_next_length(std::uint32_t received_length,
                                       std::uint32_t total_length,
                                       std::uint32_t chunk_offset,
                                       std::uint16_t chunk_length,
                                       std::uint32_t &next_received_length);
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

TEST(ClipboardTransferValidationTests, RejectsOversizedTextAndUnknownTypes) {
  constexpr std::uint32_t one_megabyte = 1024U * 1024U;

  EXPECT_TRUE(stream::clipboard_transfer_length_valid(LI_CLIPBOARD_ITEM_TYPE_NONE, 0));
  EXPECT_FALSE(stream::clipboard_transfer_length_valid(LI_CLIPBOARD_ITEM_TYPE_NONE, 1));
  EXPECT_TRUE(stream::clipboard_transfer_length_valid(LI_CLIPBOARD_ITEM_TYPE_IMAGE, LI_CLIPBOARD_MAX_IMAGE_SIZE));
  EXPECT_FALSE(stream::clipboard_transfer_length_valid(LI_CLIPBOARD_ITEM_TYPE_IMAGE, LI_CLIPBOARD_MAX_IMAGE_SIZE + 1));
  EXPECT_TRUE(stream::clipboard_transfer_length_valid(LI_CLIPBOARD_ITEM_TYPE_TEXT, one_megabyte));
  EXPECT_FALSE(stream::clipboard_transfer_length_valid(LI_CLIPBOARD_ITEM_TYPE_TEXT, one_megabyte + 1));
  EXPECT_FALSE(stream::clipboard_transfer_length_valid(0xFE, 16));
}

TEST(ClipboardTransferValidationTests, AcceptsSequentialChunksOnly) {
  std::uint32_t next_received_length = 0;

  EXPECT_TRUE(stream::clipboard_transfer_chunk_next_length(0, 10, 0, 4, next_received_length));
  EXPECT_EQ(next_received_length, 4U);
  EXPECT_TRUE(stream::clipboard_transfer_chunk_next_length(4, 10, 4, 6, next_received_length));
  EXPECT_EQ(next_received_length, 10U);
  EXPECT_TRUE(stream::clipboard_transfer_chunk_next_length(10, 10, 10, 0, next_received_length));
  EXPECT_EQ(next_received_length, 10U);

  EXPECT_FALSE(stream::clipboard_transfer_chunk_next_length(4, 10, 6, 2, next_received_length));
  EXPECT_FALSE(stream::clipboard_transfer_chunk_next_length(4, 10, 2, 2, next_received_length));
  EXPECT_FALSE(stream::clipboard_transfer_chunk_next_length(8, 10, 8, 3, next_received_length));
}

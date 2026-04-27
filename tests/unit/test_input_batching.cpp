/**
 * @file tests/unit/test_input_batching.cpp
 * @brief Tests for weak-network input coalescing.
 */

#include <cstdint>
#include <utility>

namespace input {
  std::pair<std::int16_t, std::int16_t>
  test_batch_relative_mouse_delta(std::int16_t first_x,
                                  std::int16_t first_y,
                                  std::int16_t second_x,
                                  std::int16_t second_y,
                                  bool &batched);

  std::int16_t
  test_batch_scroll_delta(std::int16_t first,
                          std::int16_t second,
                          bool &batched);
}  // namespace input

#include "../tests_common.h"

TEST(InputBatchingTests, CoalescesRelativeMouseDeltasWhenNoOverflow) {
  bool batched = false;

  auto result = input::test_batch_relative_mouse_delta(7, -3, 5, 9, batched);

  EXPECT_TRUE(batched);
  EXPECT_EQ(result.first, 12);
  EXPECT_EQ(result.second, 6);
}

TEST(InputBatchingTests, StopsRelativeMouseBatchingOnOverflow) {
  bool batched = true;

  auto result = input::test_batch_relative_mouse_delta(INT16_MAX, 1, 1, 2, batched);

  EXPECT_FALSE(batched);
  EXPECT_EQ(result.first, INT16_MAX);
  EXPECT_EQ(result.second, 1);
}

TEST(InputBatchingTests, CoalescesScrollDeltasWhenNoOverflow) {
  bool batched = false;

  auto result = input::test_batch_scroll_delta(30, 90, batched);

  EXPECT_TRUE(batched);
  EXPECT_EQ(result, 120);
}

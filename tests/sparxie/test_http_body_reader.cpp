#include "src/sparxie/http_body_reader.h"
#include <cstring>
#include <gtest/gtest.h>

TEST(HttpBodyReader, ReadsShortChunksAndEmptyResponses) {
  int calls = 0;
  auto body = sparxie::read_http_body(16,
    [&](std::uint32_t &available) { available = 4; if (calls == 2) { available = 0; } return true; },
    [&](char *buffer, std::uint32_t, std::uint32_t &received) {
      const char *chunk = "ab";
      if (calls == 1) { chunk = "cd"; }
      std::memcpy(buffer, chunk, 2);
      received = 2;
      ++calls;
      return true;
    });
  ASSERT_TRUE(body.has_value());
  EXPECT_EQ(*body, "abcd");
  auto empty = sparxie::read_http_body(16,
    [](std::uint32_t &available) { available = 0; return true; },
    [](char *, std::uint32_t, std::uint32_t &) { return false; });
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());
}

TEST(HttpBodyReader, RejectsIncompleteAndOversizedResponses) {
  int queries = 0;
  auto partial = sparxie::read_http_body(16,
    [&](std::uint32_t &available) { available = 2; return queries++ == 0; },
    [](char *buffer, std::uint32_t, std::uint32_t &received) {
      std::memcpy(buffer, "ok", 2);
      received = 2;
      return true;
    });
  EXPECT_FALSE(partial.has_value());
  for (int mode = 0; mode < 4; ++mode) {
    auto invalid = sparxie::read_http_body(3,
      [&](std::uint32_t &available) { available = 2; if (mode == 3) { available = 4; } return true; },
      [&](char *, std::uint32_t, std::uint32_t &received) {
        received = 0;
        if (mode == 2) { received = 3; }
        return mode != 0;
      });
    EXPECT_FALSE(invalid.has_value()) << "mode " << mode;
  }
}

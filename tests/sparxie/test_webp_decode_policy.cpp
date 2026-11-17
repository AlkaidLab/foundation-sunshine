#include "src/sparxie/webp_decode_policy.h"
#include <atomic>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

namespace {
  sparxie::Rgba
  pixel() {
    return { 1, 1, { 0, 0, 0, 255 } };
  }
}

TEST(WebpDecodePolicy, ConcurrentInitializationFailureIsCheckedOnce) {
  sparxie::WebpDecodePolicy policy;
  std::atomic<int> wic_calls { 0 }, decoded { 0 }, recovered { 0 };
  std::vector<std::thread> threads;
  for (int i = 0; i < 12; ++i) {
    threads.emplace_back([&] {
      std::string diagnostic;
      auto image = policy.decode(true, &diagnostic,
        [&](std::string *error, bool *init_failed) {
          ++wic_calls;
          *error = "component initialization failed";
          *init_failed = true;
          return sparxie::Rgba {};
        },
        [] { return pixel(); });
      if (image.w == 1) {
        ++decoded;
      }
      if (diagnostic.find("fallback succeeded") != std::string::npos) {
        ++recovered;
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(wic_calls, 1);
  EXPECT_EQ(decoded, 12);
  EXPECT_EQ(recovered, 1);

  std::string diagnostic;
  auto invalid = policy.decode(true, &diagnostic,
    [&](std::string *, bool *) { ++wic_calls; return pixel(); },
    [] { return sparxie::Rgba {}; });
  EXPECT_EQ(invalid.w, 0);
  EXPECT_NE(diagnostic.find("libwebp decode failed"), std::string::npos);
  EXPECT_EQ(wic_calls, 1);
  auto png = policy.decode(false, &diagnostic,
    [&](std::string *, bool *) { ++wic_calls; return pixel(); },
    [] { return sparxie::Rgba {}; });
  EXPECT_EQ(png.w, 1);
  EXPECT_EQ(wic_calls, 2);
}

TEST(WebpDecodePolicy, InvalidImageAndOtherErrorsDoNotDisableWic) {
  sparxie::WebpDecodePolicy policy;
  int wic_calls = 0;
  std::string diagnostic;
  auto broken = policy.decode(true, &diagnostic,
    [&](std::string *error, bool *init_failed) {
      ++wic_calls;
      *error = "component initialization failed";
      *init_failed = true;
      return sparxie::Rgba {};
    },
    [] { return sparxie::Rgba {}; });
  EXPECT_EQ(broken.w, 0);
  EXPECT_NE(diagnostic.find("libwebp decode failed"), std::string::npos);

  auto transient = policy.decode(true, &diagnostic,
    [&](std::string *error, bool *init_failed) {
      ++wic_calls;
      *error = "another WIC error";
      *init_failed = false;
      return sparxie::Rgba {};
    },
    [] { return pixel(); });
  EXPECT_EQ(transient.w, 1);

  auto valid = policy.decode(true, &diagnostic,
    [&](std::string *error, bool *) { ++wic_calls; error->clear(); return pixel(); },
    [] { return sparxie::Rgba {}; });
  EXPECT_EQ(valid.w, 1);
  EXPECT_TRUE(diagnostic.empty());
  EXPECT_EQ(wic_calls, 3);
}

TEST(WebpDecodePolicy, AvailableCodecCanBecomeUnavailable) {
  sparxie::WebpDecodePolicy policy;
  int wic_calls = 0;
  std::string diagnostic;
  auto first = policy.decode(true, &diagnostic,
    [&](std::string *error, bool *) { ++wic_calls; error->clear(); return pixel(); },
    [] { return sparxie::Rgba {}; });
  EXPECT_EQ(first.w, 1);
  auto recovered = policy.decode(true, &diagnostic,
    [&](std::string *error, bool *init_failed) {
      ++wic_calls;
      *error = "component initialization failed";
      *init_failed = true;
      return sparxie::Rgba {};
    },
    [] { return pixel(); });
  EXPECT_EQ(recovered.w, 1);
  EXPECT_NE(diagnostic.find("fallback succeeded"), std::string::npos);
  auto later = policy.decode(true, &diagnostic,
    [&](std::string *, bool *) { ++wic_calls; return sparxie::Rgba {}; },
    [] { return pixel(); });
  EXPECT_EQ(later.w, 1);
  EXPECT_TRUE(diagnostic.empty());
  EXPECT_EQ(wic_calls, 2);
}

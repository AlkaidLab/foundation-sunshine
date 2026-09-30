/**
 * @file tests/unit/test_nvenc_context_cache.cpp
 * @brief Context ownership tests that do not require an NVIDIA GPU.
 */
#include "src/nvenc/scoped_context_cache.h"

#include <array>
#include <atomic>
#include <barrier>
#include <memory>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

namespace {

  using cache_t = nvenc::scoped_context_cache<int, int>;

  TEST(NvencContextCacheTest, IdleCacheReleasesLastEncoderContext) {
    cache_t cache;
    auto context = cache.acquire(0, []() { return std::make_shared<int>(1); });
    std::weak_ptr<int> observed = context;
    context.reset();
    EXPECT_TRUE(observed.expired());

    auto next = cache.acquire(0, []() { return std::make_shared<int>(2); });
    ASSERT_TRUE(next);
    EXPECT_EQ(*next, 2);
  }

  TEST(NvencContextCacheTest, ProbeRetainsContextBetweenCandidatesOnly) {
    cache_t cache;
    int creations = 0;
    auto create = [&]() { return std::make_shared<int>(++creations); };
    std::weak_ptr<int> observed;
    {
      auto probe = cache.retain();
      for (int candidate = 0; candidate < 3; ++candidate) {
        auto context = cache.acquire(0, create);
        observed = context;
        ASSERT_TRUE(context);
        EXPECT_EQ(*context, 1);
      }
      EXPECT_FALSE(observed.expired());
      EXPECT_EQ(creations, 1);
    }
    EXPECT_TRUE(observed.expired());
  }

  TEST(NvencContextCacheTest, ActiveEncoderOutlivesProbeAndOtherEncoders) {
    cache_t cache;
    auto probe = cache.retain();
    auto first = cache.acquire(0, []() { return std::make_shared<int>(1); });
    auto second = cache.acquire(0, []() { return std::make_shared<int>(2); });
    EXPECT_EQ(first, second);
    std::weak_ptr<int> observed = first;
    probe.reset();
    first.reset();
    EXPECT_FALSE(observed.expired());
    second.reset();
    EXPECT_TRUE(observed.expired());
  }

  TEST(NvencContextCacheTest, ProbeRetainsReusedEncoderContext) {
    cache_t cache;
    auto encoder = cache.acquire(0, []() { return std::make_shared<int>(1); });
    std::weak_ptr<int> observed = encoder;
    auto probe = cache.retain();
    auto candidate = cache.acquire(0, []() { return std::make_shared<int>(2); });
    EXPECT_EQ(candidate, encoder);
    candidate.reset();
    encoder.reset();
    EXPECT_FALSE(observed.expired());
    probe.reset();
    EXPECT_TRUE(observed.expired());
  }

  TEST(NvencContextCacheTest, OverlappingProbesRetainUntilLastExit) {
    cache_t cache;
    auto first_probe = cache.retain();
    auto second_probe = cache.retain();
    auto context = cache.acquire(0, []() { return std::make_shared<int>(1); });
    std::weak_ptr<int> observed = context;
    context.reset();
    first_probe.reset();
    EXPECT_FALSE(observed.expired());
    second_probe.reset();
    EXPECT_TRUE(observed.expired());
  }

  TEST(NvencContextCacheTest, AdaptersDoNotShareContexts) {
    cache_t cache;
    auto first = cache.acquire(0, []() { return std::make_shared<int>(1); });
    auto second = cache.acquire(1, []() { return std::make_shared<int>(2); });
    EXPECT_NE(first, second);
    std::weak_ptr<int> observed = first;
    first.reset();
    EXPECT_TRUE(observed.expired());
    EXPECT_EQ(*second, 2);
  }

  TEST(NvencContextCacheTest, FailedCreationCanBeRetriedWithinProbe) {
    cache_t cache;
    auto probe = cache.retain();
    EXPECT_FALSE(cache.acquire(0, []() { return std::shared_ptr<int> {}; }));
    auto context = cache.acquire(0, []() { return std::make_shared<int>(1); });
    ASSERT_TRUE(context);
    EXPECT_EQ(*context, 1);
  }

  TEST(NvencContextCacheTest, EvictionReleasesProbeOwnershipButPreservesEncoderCleanup) {
    cache_t cache;
    auto probe = cache.retain();
    auto dead = cache.acquire(0, []() { return std::make_shared<int>(1); });
    std::weak_ptr<int> observed = dead;
    cache.erase(dead);
    EXPECT_FALSE(observed.expired());
    dead.reset();
    EXPECT_TRUE(observed.expired());
    auto replacement = cache.acquire(0, []() { return std::make_shared<int>(2); });
    ASSERT_TRUE(replacement);
    EXPECT_EQ(*replacement, 2);
  }

  TEST(NvencContextCacheTest, LateEvictionDoesNotEraseReplacement) {
    cache_t cache;
    auto probe = cache.retain();
    auto dead = cache.acquire(0, []() { return std::make_shared<int>(1); });
    cache.erase(dead);
    auto replacement = cache.acquire(0, []() { return std::make_shared<int>(2); });
    std::weak_ptr<int> observed = replacement;
    replacement.reset();
    cache.erase(dead);
    auto reused = cache.acquire(0, []() { return std::make_shared<int>(3); });
    EXPECT_EQ(reused, observed.lock());
    EXPECT_EQ(*reused, 2);
  }

  TEST(NvencContextCacheTest, FailedProbeUnwindsRetention) {
    cache_t cache;
    std::weak_ptr<int> observed;
    auto probe = [&]() {
      auto retention = cache.retain();
      observed = cache.acquire(0, []() { return std::make_shared<int>(1); });
      throw std::runtime_error("failed probe");
    };
    EXPECT_THROW(probe(), std::runtime_error);
    EXPECT_TRUE(observed.expired());
  }

  TEST(NvencContextCacheTest, ConcurrentEncodersCreateOneSharedContext) {
    cache_t cache;
    std::atomic<int> creations = 0;
    std::array<std::shared_ptr<int>, 8> contexts;
    std::barrier start(static_cast<std::ptrdiff_t>(contexts.size()));
    {
      std::array<std::jthread, 8> threads;
      for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i] = std::jthread([&, i]() {
          start.arrive_and_wait();
          contexts[i] = cache.acquire(0, [&]() { return std::make_shared<int>(++creations); });
        });
      }
    }
    EXPECT_EQ(creations, 1);
    for (const auto &context : contexts) EXPECT_EQ(context, contexts.front());
    std::weak_ptr<int> observed = contexts.front();
    contexts = {};
    EXPECT_TRUE(observed.expired());
  }

}  // namespace

#include "src/thread_safe.h"
#include <gtest/gtest.h>

TEST(ThreadSafeMail, RecreatesExpiredEventEntry) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  {
    auto first = mail->event<bool>("shutdown");
    ASSERT_NE(first, nullptr);
    first->raise(true);
  }

  auto second = mail->event<bool>("shutdown");
  ASSERT_NE(second, nullptr);
  EXPECT_FALSE(second->peek());
}

TEST(ThreadSafeMail, RecreatesExpiredQueueEntry) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  {
    auto first = mail->queue<int>("messages");
    ASSERT_NE(first, nullptr);
    first->raise(7);
  }

  auto second = mail->queue<int>("messages");
  ASSERT_NE(second, nullptr);
  EXPECT_FALSE(second->pop(std::chrono::milliseconds(1)).has_value());
}

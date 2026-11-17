/**
 * @file tests/unit/test_session_store.cpp
 * @brief Session lifecycle, rotation, CSRF binding and login backoff.
 */

#include <gtest/gtest.h>

#include "src/confighttp/login_throttle.h"
#include "src/confighttp/session_store.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
  using namespace std::chrono_literals;

  struct fake_clock_t {
    confighttp::sessions::time_point_t value { std::chrono::seconds { 1'700'000'000 } };

    confighttp::sessions::time_point_t
    operator()() const { return value; }

    void
    advance(const std::chrono::seconds duration) { value += duration; }
  };

  confighttp::sessions::policy_t
  short_policy() {
    return {
      .idle = 10min,
      .absolute = 60min,
      .remember_idle = 2h,
      .remember_absolute = 3h,
    };
  }

  confighttp::sessions::created_session_t
  create_one(confighttp::sessions::store_t &store, const std::string &username = "alice") {
    auto created = store.create(username, false, { "10.0.5.77", "Mozilla/5.0" }, "fingerprint");
    EXPECT_TRUE(created);
    return *created;
  }
}  // namespace

TEST(SessionStore, CreatesOpaqueIdentifiersAndOnlyStoresVerifiers) {
  fake_clock_t clock;
  confighttp::sessions::store_t store { short_policy(), [&clock] { return clock(); } };

  const auto created = create_one(store);
  EXPECT_EQ(created.token.size(), 64u);
  EXPECT_EQ(created.csrf_token.size(), 64u);
  EXPECT_EQ(created.public_id.size(), 32u);
  EXPECT_NE(created.public_id, created.token);

  const auto found = store.lookup(created.token, "fingerprint");
  ASSERT_TRUE(found);
  EXPECT_EQ(found->username, "alice");
  EXPECT_EQ(found->public_id, created.public_id);

  EXPECT_FALSE(store.lookup(created.csrf_token, "fingerprint"));
  EXPECT_FALSE(store.lookup("not-a-token", "fingerprint"));
}

TEST(SessionStore, ExpiresOnIdleTimeout) {
  fake_clock_t clock;
  confighttp::sessions::store_t store { short_policy(), [&clock] { return clock(); } };
  const auto created = create_one(store);

  clock.advance(9min);
  EXPECT_TRUE(store.lookup(created.token, "fingerprint"));

  clock.advance(11min);
  EXPECT_FALSE(store.lookup(created.token, "fingerprint"));
  EXPECT_EQ(store.size(), 0u);
}

TEST(SessionStore, TouchesIdleDeadlineButNotAbsoluteDeadline) {
  fake_clock_t clock;
  confighttp::sessions::store_t store { short_policy(), [&clock] { return clock(); } };
  const auto created = create_one(store);

  clock.advance(9min);
  EXPECT_TRUE(store.lookup(created.token, "fingerprint"));
  clock.advance(9min);
  EXPECT_TRUE(store.lookup(created.token, "fingerprint"));

  clock.advance(43min);
  EXPECT_FALSE(store.lookup(created.token, "fingerprint"));
}

TEST(SessionStore, RememberMeUsesTheLongerPolicy) {
  fake_clock_t clock;
  confighttp::sessions::store_t store { short_policy(), [&clock] { return clock(); } };
  auto created = store.create("alice", true, { "127.0.0.1", "browser" }, "fingerprint");
  ASSERT_TRUE(created);
  EXPECT_TRUE(created->remember);

  clock.advance(90min);
  const auto found = store.lookup(created->token, "fingerprint");
  ASSERT_TRUE(found);
  EXPECT_TRUE(found->remember);
}

TEST(SessionStore, RememberMeSurvivesReloadWithOnlyVerifierHashesAndRotatesOnResume) {
  fake_clock_t clock;
  const auto path = std::filesystem::temp_directory_path() / "sunshine-session-store-test.json";
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  const auto writer = [](const std::filesystem::path &destination, const std::string_view contents) {
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output << contents;
    return output.good();
  };

  confighttp::sessions::store_t first { short_policy(), [&clock] { return clock(); } };
  first.configure(short_policy(), path, writer);
  const auto created = first.create("alice", true, { "10.0.5.77", "Mozilla/5.0" }, std::string(64, 'a'));
  ASSERT_TRUE(created);

  std::ifstream persisted(path, std::ios::binary);
  const std::string contents((std::istreambuf_iterator<char>(persisted)), std::istreambuf_iterator<char>());
  EXPECT_EQ(contents.find(created->token), std::string::npos);
  EXPECT_EQ(contents.find(created->csrf_token), std::string::npos);

  confighttp::sessions::store_t second { short_policy(), [&clock] { return clock(); } };
  second.configure(short_policy(), path, writer);
  ASSERT_TRUE(second.lookup(created->token, std::string(64, 'a')));

  const auto resumed = second.resume(created->token, std::string(64, 'a'));
  ASSERT_TRUE(resumed);
  EXPECT_NE(resumed->token, created->token);
  EXPECT_NE(resumed->csrf_token, created->csrf_token);
  EXPECT_FALSE(second.lookup(created->token, "fingerprint"));
  EXPECT_TRUE(second.lookup(resumed->token, std::string(64, 'a')));

  std::filesystem::remove(path, ignored);
}

TEST(SessionStore, BrowserSessionsAreNotWrittenToTheDurableStore) {
  fake_clock_t clock;
  const auto path = std::filesystem::temp_directory_path() / "sunshine-browser-session-test.json";
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  const auto writer = [](const std::filesystem::path &destination, const std::string_view contents) {
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output << contents;
    return output.good();
  };

  confighttp::sessions::store_t first { short_policy(), [&clock] { return clock(); } };
  first.configure(short_policy(), path, writer);
  const auto created = create_one(first);

  confighttp::sessions::store_t second { short_policy(), [&clock] { return clock(); } };
  second.configure(short_policy(), path, writer);
  EXPECT_FALSE(second.lookup(created.token, "fingerprint"));
  EXPECT_FALSE(std::filesystem::exists(path));
  std::filesystem::remove(path, ignored);
}

TEST(SessionStore, LoginRotationRevokesThePreviousIdentifier) {
  fake_clock_t clock;
  confighttp::sessions::store_t store { short_policy(), [&clock] { return clock(); } };
  const auto first = create_one(store);
  const auto second = store.create("alice", false, { "10.0.5.77", "Mozilla/5.0" }, "fingerprint", first.token);
  ASSERT_TRUE(second);

  EXPECT_NE(first.token, second->token);
  EXPECT_NE(first.public_id, second->public_id);
  EXPECT_FALSE(store.lookup(first.token, "fingerprint"));
  EXPECT_TRUE(store.lookup(second->token, "fingerprint"));
}

TEST(SessionStore, CsrfRequiresTheBoundHeaderAndCookie) {
  fake_clock_t clock;
  confighttp::sessions::store_t store { short_policy(), [&clock] { return clock(); } };
  const auto created = create_one(store);

  EXPECT_TRUE(store.csrf_matches(created.token, created.csrf_token, created.csrf_token));
  EXPECT_FALSE(store.csrf_matches(created.token, "", created.csrf_token));
  EXPECT_FALSE(store.csrf_matches(created.token, created.csrf_token, ""));
  EXPECT_FALSE(store.csrf_matches(created.token, std::string(64, 'a'), std::string(64, 'a')));
  EXPECT_FALSE(store.csrf_matches(created.token, created.csrf_token, std::string(64, 'b')));
  EXPECT_FALSE(store.csrf_matches(created.csrf_token, created.csrf_token, created.csrf_token));
}

TEST(SessionStore, CredentialChangeInvalidatesSessionsUntilRekey) {
  fake_clock_t clock;
  confighttp::sessions::store_t store { short_policy(), [&clock] { return clock(); } };
  const auto first = create_one(store);
  const auto second = create_one(store, "bob");

  EXPECT_FALSE(store.lookup(first.token, "new-fingerprint"));
  EXPECT_FALSE(store.lookup(second.token, "new-fingerprint"));
  EXPECT_FALSE(store.rekey(second.token, "new-fingerprint"));

  const auto again = create_one(store);
  EXPECT_TRUE(store.rekey(again.token, "new-fingerprint"));
  EXPECT_TRUE(store.lookup(again.token, "new-fingerprint"));
}

TEST(SessionStore, RevokesOneOrEveryOtherSession) {
  fake_clock_t clock;
  confighttp::sessions::store_t store { short_policy(), [&clock] { return clock(); } };
  const auto first = create_one(store);
  const auto second = create_one(store, "bob");
  const auto third = create_one(store, "carol");

  EXPECT_TRUE(store.revoke_public(second.public_id));
  EXPECT_FALSE(store.lookup(second.token, "fingerprint"));
  EXPECT_TRUE(store.lookup(first.token, "fingerprint"));
  EXPECT_TRUE(store.lookup(third.token, "fingerprint"));

  EXPECT_EQ(store.revoke_all(first.token), 1u);
  EXPECT_TRUE(store.lookup(first.token, "fingerprint"));
  EXPECT_FALSE(store.lookup(third.token, "fingerprint"));
}

TEST(SessionStore, ListingMasksAddressesAndShortensUserAgents) {
  fake_clock_t clock;
  confighttp::sessions::store_t store { short_policy(), [&clock] { return clock(); } };
  const std::string long_agent(300, 'A');
  auto first = store.create("alice", false, { "2001:db8:85a3:1234:5678:90ab:cdef:1234", long_agent }, "fingerprint");
  ASSERT_TRUE(first);
  clock.advance(1min);
  auto second = store.create("bob", false, { "10.20.30.40", "curl/8" }, "fingerprint");
  ASSERT_TRUE(second);
  clock.advance(1min);

  const auto summaries = store.list(second->token);
  ASSERT_EQ(summaries.size(), 2u);
  EXPECT_EQ(summaries[0].public_id, second->public_id);
  EXPECT_TRUE(summaries[0].current);
  EXPECT_EQ(summaries[0].masked_address, "10.20.30.x");
  EXPECT_FALSE(summaries[1].current);
  EXPECT_EQ(summaries[1].masked_address, "2001:db8:85a3:1234::x");
  EXPECT_EQ(summaries[1].short_user_agent.size(), 99u);
  EXPECT_EQ(summaries[1].short_user_agent.substr(96), "...");
}

TEST(LoginThrottle, AllowsFiveFailuresThenBacksOffExponentially) {
  confighttp::auth::throttle_policy_t policy;
  policy.free_attempts = 5;
  policy.base_delay = 10s;
  policy.maximum_delay = 100s;
  policy.window = 1h;
  fake_clock_t clock;
  confighttp::auth::login_throttle_t throttle { policy, [&clock] { return clock(); } };

  for (int attempt = 0; attempt != 5; ++attempt) {
    EXPECT_TRUE(throttle.check("192.0.2.10", "alice").allowed);
    throttle.record_failure("192.0.2.10", "alice");
  }

  auto decision = throttle.check("192.0.2.10", "alice");
  EXPECT_FALSE(decision.allowed);
  EXPECT_EQ(decision.retry_after, 10s);

  clock.advance(10s);
  EXPECT_TRUE(throttle.check("192.0.2.10", "alice").allowed);
  throttle.record_failure("192.0.2.10", "alice");
  decision = throttle.check("192.0.2.10", "alice");
  EXPECT_FALSE(decision.allowed);
  EXPECT_EQ(decision.retry_after, 20s);
}

TEST(LoginThrottle, AppliesPerAddressAndPerUsername) {
  confighttp::auth::throttle_policy_t policy;
  policy.free_attempts = 2;
  policy.base_delay = 30s;
  policy.maximum_delay = 30s;
  fake_clock_t clock;
  confighttp::auth::login_throttle_t throttle { policy, [&clock] { return clock(); } };

  throttle.record_failure("198.51.100.1", "alice");
  throttle.record_failure("198.51.100.1", "alice");

  EXPECT_FALSE(throttle.check("198.51.100.1", "bob").allowed);
  EXPECT_FALSE(throttle.check("198.51.100.2", "alice").allowed);
  EXPECT_TRUE(throttle.check("198.51.100.2", "bob").allowed);
}

TEST(LoginThrottle, SuccessClearsBothKeys) {
  confighttp::auth::throttle_policy_t policy;
  policy.free_attempts = 2;
  fake_clock_t clock;
  confighttp::auth::login_throttle_t throttle { policy, [&clock] { return clock(); } };

  throttle.record_failure("198.51.100.1", "alice");
  throttle.record_failure("198.51.100.1", "alice");
  EXPECT_FALSE(throttle.check("198.51.100.1", "alice").allowed);

  throttle.record_success("198.51.100.1", "alice");
  EXPECT_TRUE(throttle.check("198.51.100.1", "alice").allowed);
}

TEST(LoginThrottle, OneHourWindowCapsRepeatedFailures) {
  confighttp::auth::throttle_policy_t policy;
  policy.free_attempts = 1000;
  policy.window_limit = 100;
  policy.window = 1h;
  fake_clock_t clock;
  confighttp::auth::login_throttle_t throttle { policy, [&clock] { return clock(); } };

  for (int attempt = 0; attempt != 100; ++attempt) {
    throttle.record_failure("203.0.113.9", "alice");
  }
  EXPECT_FALSE(throttle.check("203.0.113.9", "alice").allowed);

  clock.advance(1h);
  EXPECT_TRUE(throttle.check("203.0.113.9", "alice").allowed);
}

TEST(LoginThrottle, ConsecutiveFailuresResetAfterWindowExpiry) {
  confighttp::auth::throttle_policy_t policy;
  policy.free_attempts = 5;
  policy.base_delay = 30s;
  policy.maximum_delay = 900s;
  policy.window = 1h;
  fake_clock_t clock;
  confighttp::auth::login_throttle_t throttle { policy, [&clock] { return clock(); } };

  // 5 failures trigger rate limit
  for (int attempt = 0; attempt != 5; ++attempt) {
    throttle.record_failure("192.0.2.1", "alice");
  }
  EXPECT_FALSE(throttle.check("192.0.2.1", "alice").allowed);

  // Advance time past the 1-hour window (e.g. 2 hours later)
  clock.advance(2h);

  // After window expiration, a single new typo should NOT trigger immediate rate-limiting.
  // The consecutive failure counter should reset with the window, giving the user their free attempts back.
  throttle.record_failure("192.0.2.1", "alice");
  EXPECT_TRUE(throttle.check("192.0.2.1", "alice").allowed);
}

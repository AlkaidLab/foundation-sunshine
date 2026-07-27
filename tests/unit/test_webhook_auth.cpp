/**
 * @file tests/unit/test_webhook_auth.cpp
 * @brief Tests for the standalone webhook_auth.json store.
 */

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../tests_common.h"
#include <src/webhook/webhook_auth.h>

namespace {
  namespace fs = std::filesystem;

  class WebhookAuthTest : public testing::Test {
  protected:
    void SetUp() override {
      root_ = fs::temp_directory_path() /
              ("sunshine_webhook_auth_test_" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)));
      std::error_code ignored;
      fs::remove_all(root_, ignored);
      fs::create_directories(root_);
      auth_path_ = root_ / "webhook_auth.json";
    }

    void TearDown() override {
      std::error_code ignored;
      fs::remove_all(root_, ignored);
    }

    void write_json(const nlohmann::json &value) {
      std::ofstream file(auth_path_, std::ios::binary | std::ios::trunc);
      file << value.dump(2) << '\n';
    }

    void write_text(std::string_view value) {
      std::ofstream file(auth_path_, std::ios::binary | std::ios::trunc);
      file << value;
    }

    std::string read_text(const fs::path &path) {
      std::ifstream file(path, std::ios::binary);
      return std::string(
        std::istreambuf_iterator<char> {file},
        std::istreambuf_iterator<char> {}
      );
    }

    fs::path root_;
    fs::path auth_path_;
  };
}  // namespace

TEST_F(WebhookAuthTest, ResolvesBesideSelectedSunshineConfig) {
  const auto config_directory = root_ / "config";
  EXPECT_EQ(
    webhook::auth::path_for(config_directory / "sunshine.conf"),
    config_directory / "webhook_auth.json"
  );
  EXPECT_TRUE(webhook::auth::path_for(fs::path {}).empty());
  EXPECT_TRUE(webhook::auth::backup_path_for(fs::path {}).empty());
}

TEST_F(WebhookAuthTest, MissingFileReturnsDisabledDefaults) {
  const auto result = webhook::auth::load(auth_path_);

  EXPECT_EQ(result.status, webhook::auth::load_status_t::MISSING);
  EXPECT_FALSE(result.settings.enabled);
  EXPECT_TRUE(result.settings.url.empty());
  EXPECT_FALSE(result.settings.skip_ssl_verify);
  EXPECT_EQ(result.settings.timeout.count(), 5000);
  EXPECT_EQ(result.settings.events, (std::vector<int> {0, 1, 2, 3, 4, 5, 6}));
}

TEST_F(WebhookAuthTest, EmptyPathIsInvalid) {
  EXPECT_EQ(
    webhook::auth::load(fs::path {}).status,
    webhook::auth::load_status_t::INVALID
  );
  EXPECT_FALSE(webhook::auth::save(fs::path {}, webhook::auth::settings_t {}));
}

TEST_F(WebhookAuthTest, EventIdsUseStrictStableCommaSeparatedValues) {
  std::vector<int> events;
  EXPECT_TRUE(webhook::auth::parse_event_ids("6, 2,0", events));
  EXPECT_EQ(events, (std::vector<int> {0, 2, 6}));
  EXPECT_EQ(webhook::auth::serialize_event_ids(events), "0,2,6");

  EXPECT_TRUE(webhook::auth::parse_event_ids("-1", events));
  EXPECT_TRUE(events.empty());
  EXPECT_EQ(webhook::auth::serialize_event_ids(events), "-1");

  EXPECT_FALSE(webhook::auth::parse_event_ids("", events));
  EXPECT_FALSE(webhook::auth::parse_event_ids("1,", events));
  EXPECT_FALSE(webhook::auth::parse_event_ids("1,1", events));
  EXPECT_FALSE(webhook::auth::parse_event_ids("1,7", events));
  EXPECT_FALSE(webhook::auth::parse_event_ids("1,unknown", events));
}

TEST_F(WebhookAuthTest, LoadsNativeJsonFields) {
  write_json({
    {"webhook_enabled", true},
    {"webhook_events", "1,4,6"},
    {"webhook_skip_ssl_verify", true},
    {"webhook_timeout", 5000},
    {"webhook_url", "https://example.invalid/webhook"}
  });

  const auto result = webhook::auth::load(auth_path_);

  EXPECT_EQ(result.status, webhook::auth::load_status_t::LOADED);
  EXPECT_TRUE(result.settings.enabled);
  EXPECT_EQ(result.settings.url, "https://example.invalid/webhook");
  EXPECT_TRUE(result.settings.skip_ssl_verify);
  EXPECT_EQ(result.settings.timeout.count(), 5000);
  EXPECT_EQ(result.settings.events, (std::vector<int> {1, 4, 6}));
}

TEST_F(WebhookAuthTest, RejectsLegacyStringBooleanValues) {
  write_json({
    {"webhook_enabled", "enabled"},
    {"webhook_events", "0,1,2,3,4,5,6"},
    {"webhook_skip_ssl_verify", "disabled"},
    {"webhook_timeout", 5000},
    {"webhook_url", "https://example.invalid/webhook"}
  });

  const auto result = webhook::auth::load(auth_path_);

  EXPECT_EQ(result.status, webhook::auth::load_status_t::INVALID);
  EXPECT_FALSE(result.settings.enabled);
}

TEST_F(WebhookAuthTest, RejectsMalformedOrInvalidSchema) {
  write_text("{");
  EXPECT_EQ(
    webhook::auth::load(auth_path_).status,
    webhook::auth::load_status_t::INVALID
  );

  // The former four-field file is intentionally unsupported. Event
  // selection belongs to the same independent configuration now.
  write_json({
    {"webhook_enabled", true},
    {"webhook_skip_ssl_verify", false},
    {"webhook_timeout", 5000},
    {"webhook_url", "https://example.invalid/webhook"}
  });
  EXPECT_EQ(
    webhook::auth::load(auth_path_).status,
    webhook::auth::load_status_t::INVALID
  );

  write_json({
    {"webhook_enabled", true},
    {"webhook_events", "0,1,2,3,4,5,6"},
    {"webhook_skip_ssl_verify", false},
    {"webhook_timeout", 5000}
  });
  EXPECT_EQ(
    webhook::auth::load(auth_path_).status,
    webhook::auth::load_status_t::INVALID
  );

  write_json({
    {"webhook_enabled", true},
    {"webhook_events", "0,1,2,3,4,5,6"},
    {"webhook_skip_ssl_verify", false},
    {"webhook_timeout", 5000},
    {"webhook_url", "https://example.invalid/webhook"},
    {"unexpected_field", true}
  });
  EXPECT_EQ(
    webhook::auth::load(auth_path_).status,
    webhook::auth::load_status_t::INVALID
  );
}

TEST_F(WebhookAuthTest, RejectsTimeoutOutsideOneToFifteenSeconds) {
  for (const auto timeout : {999, 15001}) {
    write_json({
      {"webhook_enabled", true},
      {"webhook_events", "0,1,2,3,4,5,6"},
      {"webhook_skip_ssl_verify", false},
      {"webhook_timeout", timeout},
      {"webhook_url", "https://example.invalid/webhook"}
    });

    const auto result = webhook::auth::load(auth_path_);
    EXPECT_EQ(result.status, webhook::auth::load_status_t::INVALID);
  }
}

TEST_F(WebhookAuthTest, AcceptsTimeoutBoundaryValues) {
  for (const auto timeout : {1000, 15000}) {
    write_json({
      {"webhook_enabled", true},
      {"webhook_events", "0,1,2,3,4,5,6"},
      {"webhook_skip_ssl_verify", false},
      {"webhook_timeout", timeout},
      {"webhook_url", "https://example.invalid/webhook"}
    });

    const auto result = webhook::auth::load(auth_path_);
    ASSERT_EQ(result.status, webhook::auth::load_status_t::LOADED);
    EXPECT_EQ(result.settings.timeout.count(), timeout);
  }
}

TEST_F(WebhookAuthTest, RejectsOversizedUrl) {
  std::string oversized_url = "https://example.invalid/";
  oversized_url.append(webhook::MAX_URL_SIZE + 1 - oversized_url.size(), 'x');
  write_json({
    {"webhook_enabled", true},
    {"webhook_events", "0,1,2,3,4,5,6"},
    {"webhook_skip_ssl_verify", false},
    {"webhook_timeout", 5000},
    {"webhook_url", std::move(oversized_url)}
  });

  EXPECT_EQ(
    webhook::auth::load(auth_path_).status,
    webhook::auth::load_status_t::INVALID
  );
}

TEST_F(WebhookAuthTest, SavesAndReloadsCompleteSettings) {
  const webhook::auth::settings_t settings {
    true,
    "https://example.invalid/webhook",
    false,
    std::chrono::milliseconds {15000},
    {0, 2, 6}
  };

  ASSERT_TRUE(webhook::auth::save(auth_path_, settings));
  const auto result = webhook::auth::load(auth_path_);

  ASSERT_EQ(result.status, webhook::auth::load_status_t::LOADED);
  EXPECT_EQ(result.settings.enabled, settings.enabled);
  EXPECT_EQ(result.settings.url, settings.url);
  EXPECT_EQ(result.settings.skip_ssl_verify, settings.skip_ssl_verify);
  EXPECT_EQ(result.settings.timeout, settings.timeout);
  EXPECT_EQ(result.settings.events, settings.events);

  std::ifstream file(auth_path_, std::ios::binary);
  const auto persisted = nlohmann::json::parse(file);
  EXPECT_EQ(persisted.size(), 5);
  EXPECT_TRUE(persisted["webhook_enabled"].is_boolean());
  EXPECT_TRUE(persisted["webhook_skip_ssl_verify"].is_boolean());
  EXPECT_TRUE(persisted["webhook_timeout"].is_number_integer());
  EXPECT_TRUE(persisted["webhook_url"].is_string());
  EXPECT_TRUE(persisted["webhook_events"].is_string());
  EXPECT_EQ(persisted["webhook_events"], "0,2,6");

#ifndef _WIN32
  const auto permissions = fs::status(auth_path_).permissions();
  const auto non_owner_permissions =
    fs::perms::group_read |
    fs::perms::group_write |
    fs::perms::group_exec |
    fs::perms::others_read |
    fs::perms::others_write |
    fs::perms::others_exec;
  EXPECT_EQ(permissions & non_owner_permissions, fs::perms::none);
#endif
}

TEST_F(WebhookAuthTest, SubsequentSaveBacksUpTheExactPreviousFile) {
  const webhook::auth::settings_t previous {
    true,
    "https://example.invalid/previous",
    false,
    std::chrono::milliseconds {5000},
    {0, 2, 6}
  };
  const webhook::auth::settings_t replacement {
    true,
    "https://example.invalid/replacement",
    true,
    std::chrono::milliseconds {15000},
    {1, 4}
  };

  ASSERT_TRUE(webhook::auth::save(auth_path_, previous));
  const auto exact_previous_contents = read_text(auth_path_);
  ASSERT_TRUE(webhook::auth::save(auth_path_, replacement));

  const auto backup_path = webhook::auth::backup_path_for(auth_path_);
  EXPECT_EQ(read_text(backup_path), exact_previous_contents);

  const auto active = webhook::auth::load(auth_path_);
  ASSERT_EQ(active.status, webhook::auth::load_status_t::LOADED);
  EXPECT_EQ(active.settings.url, replacement.url);
  EXPECT_EQ(active.settings.events, replacement.events);

  const auto backup = webhook::auth::load(backup_path);
  ASSERT_EQ(backup.status, webhook::auth::load_status_t::LOADED);
  EXPECT_EQ(backup.settings.url, previous.url);
  EXPECT_EQ(backup.settings.events, previous.events);
}

TEST_F(WebhookAuthTest, InvalidSaveDoesNotReplaceExistingFile) {
  const webhook::auth::settings_t valid {
    true,
    "https://example.invalid/original",
    false,
    std::chrono::milliseconds {5000},
    {0, 1, 2, 3, 4, 5, 6}
  };
  ASSERT_TRUE(webhook::auth::save(auth_path_, valid));

  auto invalid = valid;
  invalid.timeout = std::chrono::milliseconds {16000};
  EXPECT_FALSE(webhook::auth::save(auth_path_, invalid));

  const auto result = webhook::auth::load(auth_path_);
  ASSERT_EQ(result.status, webhook::auth::load_status_t::LOADED);
  EXPECT_EQ(result.settings.url, valid.url);
  EXPECT_EQ(result.settings.timeout, valid.timeout);
}

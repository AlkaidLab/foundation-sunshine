/**
 * @file tests/unit/test_webhook_config.cpp
 * @brief Test webhook configuration parsing.
 */

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>

#include "../tests_common.h"
#include <src/config.h>

namespace {
  class temporary_sunshine_config_t {
  public:
    temporary_sunshine_config_t():
        original_path_(config::sunshine.config_file),
        path_(
          std::filesystem::temp_directory_path() /
          ("sunshine_webhook_config_test_" +
           std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
           ".conf")
        ) {
      config::sunshine.config_file = path_.string();
    }

    ~temporary_sunshine_config_t() {
      config::sunshine.config_file = original_path_;
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }

    void write(std::string_view content) const {
      std::ofstream file(path_, std::ios::binary | std::ios::trunc);
      file << content;
    }

    std::string read() const {
      std::ifstream file(path_, std::ios::binary);
      return {
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
      };
    }

  private:
    std::string original_path_;
    std::filesystem::path path_;
  };
}  // namespace

TEST(WebhookConfigTest, LegacyFieldsAreOrdinaryParsedValues) {
  const auto parsed = config::parse_config(
    "webhook_enabled = enabled\n"
    "webhook_url = https://example.invalid/webhook\n"
    "webhook_skip_ssl_verify = enabled\n"
    "webhook_timeout = 5000\n"
    "webhook_events = 1,4\n"
  );

  EXPECT_EQ(parsed.at("webhook_enabled"), "enabled");
  EXPECT_EQ(parsed.at("webhook_url"), "https://example.invalid/webhook");
  EXPECT_EQ(parsed.at("webhook_skip_ssl_verify"), "enabled");
  EXPECT_EQ(parsed.at("webhook_timeout"), "5000");
  EXPECT_EQ(parsed.at("webhook_events"), "1,4");
}

TEST(WebhookConfigTest, GeneralConfigCanOverwriteLegacyWebhookFields) {
  temporary_sunshine_config_t temporary_config;
  temporary_config.write(
    "webhook_enabled = enabled\n"
    "webhook_url = https://example.invalid/legacy\n"
    "webhook_skip_ssl_verify = enabled\n"
    "webhook_timeout = 5000\n"
    "webhook_events = 1,4\n"
    "sunshine_name = old-name\n"
  );

  const std::map<std::string, std::string> incoming {
    {"sunshine_name", "new-name"},
    {"webhook_enabled", "disabled"},
    {"webhook_url", "https://example.invalid/replacement"},
    {"webhook_skip_ssl_verify", "disabled"},
    {"webhook_timeout", "15000"},
    {"webhook_events", "0,2,6"},
  };
  ASSERT_TRUE(config::update_full_config(incoming));
  ASSERT_TRUE(config::update_config({
    {"webhook_url", "https://example.invalid/second-replacement"},
    {"sunshine_name", "final-name"},
  }));

  const auto persisted = config::parse_config(temporary_config.read());
  EXPECT_EQ(persisted.at("sunshine_name"), "final-name");
  EXPECT_EQ(persisted.at("webhook_enabled"), "disabled");
  EXPECT_EQ(persisted.at("webhook_url"), "https://example.invalid/second-replacement");
  EXPECT_EQ(persisted.at("webhook_skip_ssl_verify"), "disabled");
  EXPECT_EQ(persisted.at("webhook_timeout"), "15000");
  EXPECT_EQ(persisted.at("webhook_events"), "0,2,6");
}

TEST(WebhookConfigTest, EmptyLegacyValueFollowsNormalConfigRules) {
  temporary_sunshine_config_t temporary_config;
  temporary_config.write(
    "webhook_url = \n"
    "sunshine_name = old-name\n"
  );

  ASSERT_TRUE(config::update_config({
    {"sunshine_name", "new-name"},
  }));
  EXPECT_EQ(temporary_config.read().find("webhook_url = \n"), std::string::npos);
}

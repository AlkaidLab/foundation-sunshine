/**
 * @file tests/unit/test_webhook_config.cpp
 * @brief Test webhook configuration parsing.
 */

#include <string>
#include <unordered_map>

#include "../tests_common.h"
#include "config.h"

using namespace std::literals;

struct WebhookConfigTest : testing::Test {
  void SetUp() override {
    // Reset webhook config to defaults
    config::webhook.enabled = false;
    config::webhook.url = "";
    config::webhook.skip_ssl_verify = false;
    config::webhook.timeout = std::chrono::milliseconds(1000);
  }
};

TEST_F(WebhookConfigTest, DefaultValues) {
  EXPECT_FALSE(config::webhook.enabled);
  EXPECT_EQ(config::webhook.url, "");
  EXPECT_FALSE(config::webhook.skip_ssl_verify);
  EXPECT_EQ(config::webhook.timeout.count(), 1000);
}

TEST_F(WebhookConfigTest, ParseWebhookEnabled) {
  std::unordered_map<std::string, std::string> vars;
  vars["webhook_enabled"] = "true";
  
  config::apply_config(std::move(vars));
  
  EXPECT_TRUE(config::webhook.enabled);
}

TEST_F(WebhookConfigTest, ParseWebhookUrl) {
  std::unordered_map<std::string, std::string> vars;
  vars["webhook_url"] = "https://example.com/webhook";
  
  config::apply_config(std::move(vars));
  
  EXPECT_EQ(config::webhook.url, "https://example.com/webhook");
}

TEST_F(WebhookConfigTest, ParseWebhookSkipSslVerify) {
  std::unordered_map<std::string, std::string> vars;
  vars["webhook_skip_ssl_verify"] = "true";
  
  config::apply_config(std::move(vars));
  
  EXPECT_TRUE(config::webhook.skip_ssl_verify);
}

TEST_F(WebhookConfigTest, ParseWebhookTimeout) {
  std::unordered_map<std::string, std::string> vars;
  vars["webhook_timeout"] = "2000";
  
  config::apply_config(std::move(vars));
  
  EXPECT_EQ(config::webhook.timeout.count(), 2000);
}

TEST_F(WebhookConfigTest, ParseWebhookTimeoutOutOfRange) {
  std::unordered_map<std::string, std::string> vars;
  vars["webhook_timeout"] = "10000";  // Out of range (100-5000)
  
  config::apply_config(std::move(vars));
  
  // Should remain at default value
  EXPECT_EQ(config::webhook.timeout.count(), 1000);
}

TEST_F(WebhookConfigTest, ParseWebhookTimeoutTooLow) {
  std::unordered_map<std::string, std::string> vars;
  vars["webhook_timeout"] = "50";  // Too low (100-5000)
  
  config::apply_config(std::move(vars));
  
  // Should remain at default value
  EXPECT_EQ(config::webhook.timeout.count(), 1000);
}

TEST_F(WebhookConfigTest, ParseAllWebhookSettings) {
  std::unordered_map<std::string, std::string> vars;
  vars["webhook_enabled"] = "true";
  vars["webhook_url"] = "https://test.com/hook";
  vars["webhook_skip_ssl_verify"] = "true";
  vars["webhook_timeout"] = "3000";
  
  config::apply_config(std::move(vars));
  
  EXPECT_TRUE(config::webhook.enabled);
  EXPECT_EQ(config::webhook.url, "https://test.com/hook");
  EXPECT_TRUE(config::webhook.skip_ssl_verify);
  EXPECT_EQ(config::webhook.timeout.count(), 3000);
}

TEST_F(WebhookConfigTest, ParseWebhookTimeoutBoundaryValues) {
  // Test minimum valid value
  {
    std::unordered_map<std::string, std::string> vars;
    vars["webhook_timeout"] = "100";  // Minimum valid value
    
    config::apply_config(std::move(vars));
    
    EXPECT_EQ(config::webhook.timeout.count(), 100);
  }
  
  // Test maximum valid value
  {
    std::unordered_map<std::string, std::string> vars;
    vars["webhook_timeout"] = "5000";  // Maximum valid value
    
    config::apply_config(std::move(vars));
    
    EXPECT_EQ(config::webhook.timeout.count(), 5000);
  }
}

TEST_F(WebhookConfigTest, ParseWebhookBooleanVariations) {
  // Test various boolean representations for enabled
  std::vector<std::string> true_values = {"true", "True", "TRUE", "1", "yes", "Yes", "YES", "enable", "enabled", "on"};
  std::vector<std::string> false_values = {"false", "False", "FALSE", "0", "no", "No", "NO", "disable", "disabled", "off"};
  
  for (const auto& value : true_values) {
    std::unordered_map<std::string, std::string> vars;
    vars["webhook_enabled"] = value;
    
    config::apply_config(std::move(vars));
    
    EXPECT_TRUE(config::webhook.enabled) << "Failed for value: " << value;
  }
  
  for (const auto& value : false_values) {
    std::unordered_map<std::string, std::string> vars;
    vars["webhook_enabled"] = value;
    
    config::apply_config(std::move(vars));
    
    EXPECT_FALSE(config::webhook.enabled) << "Failed for value: " << value;
  }
}

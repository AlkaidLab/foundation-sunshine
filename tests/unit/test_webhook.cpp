/**
 * @file tests/unit/test_webhook.cpp
 * @brief Webhook formatting and transport-policy tests.
 */

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../tests_common.h"
#include <src/webhook/webhook.h>
#include <src/webhook/webhook_format.h>

using namespace std::literals;

struct WebhookTest: testing::Test {
  void SetUp() override {
    ASSERT_TRUE(webhook::configure({}));
    webhook::configure_webhook_format(true);
  }
};

TEST_F(WebhookTest, IsEnabledRequiresFlagAndUrl) {
  EXPECT_FALSE(webhook::is_enabled());
  auto configuration = webhook::current_configuration();
  configuration.enabled = true;
  EXPECT_FALSE(webhook::configure(configuration));
  EXPECT_FALSE(webhook::is_enabled());

  configuration.url = "https://example.invalid/webhook";
  ASSERT_TRUE(webhook::configure(std::move(configuration)));
  EXPECT_TRUE(webhook::is_enabled());
}

TEST_F(WebhookTest, AlertMessageLocalization) {
  EXPECT_EQ(webhook::get_alert_message(webhook::event_type_t::CONFIG_PIN_SUCCESS, true), "🔗 配置配对成功");
  EXPECT_EQ(webhook::get_alert_message(webhook::event_type_t::NV_APP_LAUNCH, true), "🚀 应用启动");
  EXPECT_EQ(webhook::get_alert_message(webhook::event_type_t::NV_APP_RESUME, false), "▶️ application resumed");
}

TEST_F(WebhookTest, EventIdentifiersAndFilterAreStable) {
  EXPECT_EQ(webhook::event_type_id(webhook::event_type_t::CONFIG_PIN_SUCCESS), 0);
  EXPECT_EQ(webhook::event_type_id(webhook::event_type_t::NV_SESSION_END), 6);
  EXPECT_STREQ(webhook::event_type_name(webhook::event_type_t::NV_APP_TERMINATE), "nv_app_terminate");

  EXPECT_TRUE(webhook::is_event_enabled(webhook::event_type_t::NV_APP_LAUNCH));
  auto configuration = webhook::current_configuration();
  configuration.events = {1, 6};
  ASSERT_TRUE(webhook::configure(std::move(configuration)));
  EXPECT_FALSE(webhook::is_event_enabled(webhook::event_type_t::NV_APP_LAUNCH));
  EXPECT_TRUE(webhook::is_event_enabled(webhook::event_type_t::NV_SESSION_END));
}

TEST_F(WebhookTest, ConfigurationUpdateIsCoherentAndRejectsInvalidValues) {
  webhook::configuration_t configuration {
    true,
    "https://example.invalid/webhook",
    true,
    15000ms,
    {4, 1}
  };
  ASSERT_TRUE(webhook::configure(configuration));

  const auto active = webhook::current_configuration();
  EXPECT_TRUE(active.enabled);
  EXPECT_EQ(active.url, configuration.url);
  EXPECT_TRUE(active.skip_ssl_verify);
  EXPECT_EQ(active.timeout, 15000ms);
  EXPECT_EQ(active.events, (std::vector<int> {1, 4}));

  configuration.events = {1, 1};
  EXPECT_FALSE(webhook::configure(configuration));
  EXPECT_EQ(webhook::current_configuration().events, (std::vector<int> {1, 4}));
}

TEST_F(WebhookTest, JsonStringSanitization) {
  EXPECT_EQ(webhook::sanitize_json_string("Hello \"World\""), "Hello \\\"World\\\"");
  EXPECT_EQ(webhook::sanitize_json_string("Line1\nLine2"), "Line1\\nLine2");
  EXPECT_EQ(webhook::sanitize_json_string("Tab\tHere"), "Tab\\tHere");
  EXPECT_EQ(webhook::sanitize_json_string("Back\\slash"), "Back\\\\slash");
  EXPECT_EQ(webhook::sanitize_json_string("Text\x01\x02\x03"), "Text");
  EXPECT_EQ(webhook::sanitize_json_string("Hello 世界 🚀"), "Hello 世界 🚀");
}

TEST_F(WebhookTest, TimestampIsRfc3339Utc) {
  const auto timestamp = webhook::get_current_timestamp();
  ASSERT_EQ(timestamp.size(), 24);
  EXPECT_EQ(timestamp[4], '-');
  EXPECT_EQ(timestamp[7], '-');
  EXPECT_EQ(timestamp[10], 'T');
  EXPECT_EQ(timestamp[13], ':');
  EXPECT_EQ(timestamp[16], ':');
  EXPECT_EQ(timestamp[19], '.');
  EXPECT_EQ(timestamp.back(), 'Z');
}

TEST_F(WebhookTest, ProductionPayloadIsValidJsonAndEscapesMarkup) {
  webhook::event_t event {
    .type = webhook::event_type_t::NV_APP_LAUNCH,
    .timestamp = "2026-01-01T00:00:00.000Z",
    .client_name = "Client **admin** <admin>",
    .client_ip = "127.0.0.10",
    .server_ip = "127.0.0.20",
    .app_name = "Game \"One\" & <script>",
    .app_id = 123,
    .extra_data = {{"resolution", "1920x1080"}, {"fps", "60"}}
  };

  const auto payload_text = webhook::generate_webhook_json(event, false);
  ASSERT_TRUE(nlohmann::json::accept(payload_text));
  const auto payload = nlohmann::json::parse(payload_text);
  ASSERT_EQ(payload.at("msgtype"), "markdown");
  EXPECT_EQ(payload.at("event_id"), 2);
  EXPECT_EQ(payload.at("event_type"), "nv_app_launch");
  const auto content = payload.at("markdown").at("content").get<std::string>();
  EXPECT_NE(content.find("Game &quot;One&quot; &amp; &lt;script&gt;"), std::string::npos);
  EXPECT_NE(content.find("Client \\*\\*admin\\*\\* &lt;admin&gt;"), std::string::npos);
  EXPECT_EQ(content.find("<script>"), std::string::npos);
  EXPECT_NE(content.find("127.0.0.20"), std::string::npos);
}

TEST_F(WebhookTest, TestPayloadUsesTheProductionEnvelope) {
  const auto payload_text = webhook::g_webhook_format.generate_test_json_payload();
  ASSERT_TRUE(nlohmann::json::accept(payload_text));

  const auto payload = nlohmann::json::parse(payload_text);
  EXPECT_EQ(payload.at("event_id"), -1);
  EXPECT_EQ(payload.at("event_type"), "webhook_test");
  EXPECT_EQ(payload.at("msgtype"), "markdown");
  EXPECT_EQ(payload.at("markdown").at("content"), "**Sunshine Webhook Test**");
}

TEST_F(WebhookTest, TestRetryCountRejectsValuesAboveThree) {
  const webhook::settings_t settings {
    "https://example.invalid/webhook",
    false,
    5000ms
  };
  bool completed = false;
  webhook::delivery_result_t result;

  EXPECT_FALSE(webhook::send_test_async(
    settings,
    webhook::MAX_TEST_RETRIES + 1,
    [&](const webhook::delivery_result_t value) {
      completed = true;
      result = value;
    }
  ));
  EXPECT_TRUE(completed);
  EXPECT_EQ(result.attempts, 0);
  EXPECT_EQ(result.error, webhook::delivery_error_t::INTERNAL);
}

TEST_F(WebhookTest, TestDeliveryRejectsTimeoutOutsidePublicContract) {
  webhook::settings_t settings {
    "https://example.invalid/webhook",
    false,
    webhook::MIN_TIMEOUT - 1ms
  };
  bool completed = false;
  webhook::delivery_result_t result;

  EXPECT_FALSE(webhook::send_test_async(
    settings,
    0,
    [&](const webhook::delivery_result_t value) {
      completed = true;
      result = value;
    }
  ));
  EXPECT_TRUE(completed);
  EXPECT_EQ(result.attempts, 0);
  EXPECT_EQ(result.error, webhook::delivery_error_t::INTERNAL);

  completed = false;
  settings.timeout = webhook::MAX_TIMEOUT + 1ms;
  EXPECT_FALSE(webhook::send_test_async(
    settings,
    0,
    [&](const webhook::delivery_result_t value) {
      completed = true;
      result = value;
    }
  ));
  EXPECT_TRUE(completed);
  EXPECT_EQ(result.attempts, 0);
  EXPECT_EQ(result.error, webhook::delivery_error_t::INTERNAL);
}

TEST_F(WebhookTest, PayloadTruncationPreservesUtf8AndJson) {
  webhook::event_t event {
    .type = webhook::event_type_t::NV_APP_LAUNCH,
    .timestamp = "2026-01-01T00:00:00.000Z",
    .app_name = std::string(2000, 'a') + "世界世界世界"
  };
  for (int i = 0; i < 2000; ++i) {
    event.extra_data["error"] += "界";
  }

  const auto payload_text = webhook::generate_webhook_json(event, false);
  ASSERT_TRUE(nlohmann::json::accept(payload_text));
  const auto content = nlohmann::json::parse(payload_text).at("markdown").at("content").get<std::string>();
  EXPECT_LE(content.size(), 4096);
  EXPECT_TRUE(content.ends_with("..."));
}

TEST_F(WebhookTest, UrlParserPreservesQueryAndRemovesFragment) {
  webhook::test_support::parsed_url_t parsed;
  ASSERT_TRUE(webhook::test_support::parse_url(
    "https://example.invalid:8443/hooks/event?kind=launch#local-view",
    parsed
  ));
  EXPECT_TRUE(parsed.https);
  EXPECT_EQ(parsed.server, "example.invalid:8443");
  EXPECT_EQ(parsed.target, "/hooks/event?kind=launch");

  ASSERT_TRUE(webhook::test_support::parse_url("http://example.invalid?kind=test", parsed));
  EXPECT_FALSE(parsed.https);
  EXPECT_EQ(parsed.target, "/?kind=test");
}

TEST_F(WebhookTest, UrlParserRejectsUnsupportedOrCredentialedUrls) {
  webhook::test_support::parsed_url_t parsed;
  EXPECT_FALSE(webhook::test_support::parse_url("ftp://example.invalid/hook", parsed));
  EXPECT_FALSE(webhook::test_support::parse_url("https://user:password@example.invalid/hook", parsed));
  EXPECT_FALSE(webhook::test_support::parse_url("https:///missing-host", parsed));
  EXPECT_FALSE(webhook::test_support::parse_url("https://example.invalid/has space", parsed));
  EXPECT_FALSE(webhook::test_support::parse_url("https://example.invalid/hook\nInjected", parsed));

  auto embedded_nul = std::string {"https://example.invalid/hook"};
  embedded_nul.push_back('\0');
  embedded_nul += "hidden";
  EXPECT_FALSE(webhook::test_support::parse_url(embedded_nul, parsed));
}

TEST_F(WebhookTest, DynamicHeaderValuesCannotInjectAnotherHeader) {
  EXPECT_EQ(
    webhook::test_support::sanitize_header_value("host\r\nInjected: yes\t"),
    "host__Injected: yes_"
  );
  EXPECT_EQ(
    webhook::test_support::sanitize_header_value(std::string(600, 'a')).size(),
    512
  );
}

TEST_F(WebhookTest, TimeoutMillisecondsRoundUpToWholeSeconds) {
  EXPECT_EQ(webhook::test_support::timeout_seconds(1ms), 1);
  EXPECT_EQ(webhook::test_support::timeout_seconds(1000ms), 1);
  EXPECT_EQ(webhook::test_support::timeout_seconds(1001ms), 2);
  EXPECT_EQ(webhook::test_support::timeout_seconds(1999ms), 2);
  EXPECT_EQ(webhook::test_support::timeout_seconds(15000ms), 15);
}

TEST_F(WebhookTest, HttpStatusPolicyAcceptsAll2xxAndRetriesOnlyAllowlist) {
  EXPECT_TRUE(webhook::test_support::is_success_status(200));
  EXPECT_TRUE(webhook::test_support::is_success_status(202));
  EXPECT_TRUE(webhook::test_support::is_success_status(204));
  EXPECT_FALSE(webhook::test_support::is_success_status(300));

  for (const int status : {408, 429, 500, 502, 503, 504}) {
    EXPECT_TRUE(webhook::test_support::is_retryable_status(status)) << status;
  }
  for (const int status : {400, 401, 403, 404, 409, 501}) {
    EXPECT_FALSE(webhook::test_support::is_retryable_status(status)) << status;
  }
}

TEST_F(WebhookTest, RetryAfterAcceptsDelaySecondsAndAppliesCap) {
  ASSERT_TRUE(webhook::test_support::retry_after_seconds("0"));
  EXPECT_EQ(*webhook::test_support::retry_after_seconds("0"), 0);
  ASSERT_TRUE(webhook::test_support::retry_after_seconds("15"));
  EXPECT_EQ(*webhook::test_support::retry_after_seconds("15"), 15);
  ASSERT_TRUE(webhook::test_support::retry_after_seconds("120"));
  EXPECT_EQ(*webhook::test_support::retry_after_seconds("120"), 60);
  EXPECT_FALSE(webhook::test_support::retry_after_seconds("not-a-delay"));
}

TEST_F(WebhookTest, RetryAfterAcceptsHttpDateForms) {
  ASSERT_TRUE(webhook::test_support::retry_after_seconds("Sun, 06 Nov 2094 08:49:37 GMT"));
  EXPECT_EQ(*webhook::test_support::retry_after_seconds("Sun, 06 Nov 2094 08:49:37 GMT"), 60);

  ASSERT_TRUE(webhook::test_support::retry_after_seconds("Sunday, 06-Nov-94 08:49:37 GMT"));
  EXPECT_EQ(*webhook::test_support::retry_after_seconds("Sunday, 06-Nov-94 08:49:37 GMT"), 0);

  ASSERT_TRUE(webhook::test_support::retry_after_seconds("Sun Nov  6 08:49:37 1994"));
  EXPECT_EQ(*webhook::test_support::retry_after_seconds("Sun Nov  6 08:49:37 1994"), 0);
}

TEST_F(WebhookTest, DeliveryErrorsHaveStableApiNames) {
  EXPECT_STREQ(webhook::delivery_error_name(webhook::delivery_error_t::NONE), "none");
  EXPECT_STREQ(webhook::delivery_error_name(webhook::delivery_error_t::INVALID_URL), "invalid_url");
  EXPECT_STREQ(webhook::delivery_error_name(webhook::delivery_error_t::TRANSPORT), "transport");
  EXPECT_STREQ(webhook::delivery_error_name(webhook::delivery_error_t::CANCELLED), "cancelled");
}

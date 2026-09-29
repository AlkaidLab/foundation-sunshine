/**
 * @file src/ai/codex_responses.h
 * @brief Stateless Chat Completions to Codex Responses conversion and SSE decoding.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace codex_responses {

  struct request_result_t {
    bool success = false;
    std::string body;
    std::string model;
    std::string error;
  };

  namespace detail {
    using json = nlohmann::json;

    inline bool
    is_data_image(std::string_view uri) {
      constexpr std::string_view prefix = "data:image/";
      if (!uri.starts_with(prefix)) return false;
      const auto delimiter = uri.find(";base64,", prefix.size());
      if (delimiter == std::string_view::npos || delimiter + 8 >= uri.size()) return false;
      const auto subtype = uri.substr(prefix.size(), delimiter - prefix.size());
      if (subtype != "png" && subtype != "jpeg" && subtype != "gif" && subtype != "webp") return false;
      const auto encoded = uri.substr(delimiter + 8);
      return std::all_of(encoded.begin(), encoded.end(), [](char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
               || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/' || ch == '=';
      });
    }

    inline bool
    append_content(const json &content, std::string_view role, json &parts, std::string &error) {
      const bool assistant = role == "assistant";
      if (content.is_string()) {
        const auto text = content.get<std::string>();
        if (text.empty()) {
          error = "Message content must not be empty.";
          return false;
        }
        parts.push_back({ { "type", assistant ? "output_text" : "input_text" }, { "text", text } });
        return true;
      }
      if (!content.is_array() || content.empty()) {
        error = "Message content must be a non-empty string or an array of text/image parts.";
        return false;
      }
      for (const auto &part : content) {
        if (!part.is_object() || !part.contains("type") || !part["type"].is_string()) {
          error = "Each message content part needs a string type.";
          return false;
        }
        const auto type = part["type"].get<std::string>();
        if (type == "text") {
          if (!part.contains("text") || !part["text"].is_string() || part["text"].get<std::string>().empty()) {
            error = "A text content part needs non-empty text.";
            return false;
          }
          parts.push_back({ { "type", assistant ? "output_text" : "input_text" }, { "text", part["text"] } });
        }
        else if (type == "image_url" && role == "user") {
          if (!part.contains("image_url") || !part["image_url"].is_object()
              || !part["image_url"].contains("url") || !part["image_url"]["url"].is_string()) {
            error = "An image_url content part needs image_url.url.";
            return false;
          }
          const auto &image = part["image_url"];
          const auto url = image["url"].get<std::string>();
          if (!is_data_image(url)) {
            error = "Codex images need a base64 data URI for PNG, JPEG, GIF, or WebP.";
            return false;
          }
          json converted = { { "type", "input_image" }, { "image_url", url } };
          if (image.contains("detail")) {
            if (!image["detail"].is_string()) {
              error = "Image detail must be auto, low, or high.";
              return false;
            }
            const auto detail = image["detail"].get<std::string>();
            if (detail != "auto" && detail != "low" && detail != "high") {
              error = "Image detail must be auto, low, or high.";
              return false;
            }
            converted["detail"] = detail;
          }
          parts.push_back(std::move(converted));
        }
        else {
          error = "Unsupported content part '" + type + "' for role '" + std::string(role) + "'.";
          return false;
        }
      }
      return true;
    }

    inline std::string
    event_error(const json &event, std::string fallback) {
      if (event.contains("response") && event["response"].is_object()) {
        const auto &response = event["response"];
        if (response.contains("error") && response["error"].is_object()) {
          const auto &error = response["error"];
          if (error.contains("message") && error["message"].is_string()) return error["message"].get<std::string>();
        }
        if (response.contains("incomplete_details") && response["incomplete_details"].is_object()) {
          const auto &details = response["incomplete_details"];
          if (details.contains("reason") && details["reason"].is_string()) {
            return "Codex response incomplete: " + details["reason"].get<std::string>();
          }
        }
      }
      if (event.contains("error") && event["error"].is_object()) {
        const auto &error = event["error"];
        if (error.contains("message") && error["message"].is_string()) return error["message"].get<std::string>();
      }
      if (event.contains("message") && event["message"].is_string()) return event["message"].get<std::string>();
      return fallback;
    }
  }  // namespace detail

  inline request_result_t
  make_request(const std::string &chat_json) {
    request_result_t result;
    try {
      const auto chat = detail::json::parse(chat_json);
      if (!chat.is_object()) {
        result.error = "Chat Completions request must be a JSON object.";
        return result;
      }
      for (auto it = chat.begin(); it != chat.end(); ++it) {
        if (it.key() != "model" && it.key() != "messages" && it.key() != "temperature"
            && it.key() != "max_tokens" && it.key() != "stream") {
          result.error = "Unsupported Chat Completions field '" + it.key() + "' for Codex.";
          return result;
        }
      }
      if (!chat.contains("model") || !chat["model"].is_string()
          || chat["model"].get<std::string>().empty()) {
        result.error = "Codex request needs a non-empty model.";
        return result;
      }
      if (!chat.contains("messages") || !chat["messages"].is_array() || chat["messages"].empty()) {
        result.error = "Codex request needs a non-empty messages array.";
        return result;
      }

      result.model = chat["model"].get<std::string>();
      detail::json input = detail::json::array();
      std::string instructions;
      std::size_t message_index = 0;
      for (const auto &message : chat["messages"]) {
        if (!message.is_object() || !message.contains("role") || !message["role"].is_string()
            || !message.contains("content")) {
          result.error = "Each message needs a role and content.";
          return result;
        }
        for (auto it = message.begin(); it != message.end(); ++it) {
          if (it.key() != "role" && it.key() != "content") {
            result.error = "Unsupported message field '" + it.key() + "' for Codex.";
            return result;
          }
        }
        const auto role = message["role"].get<std::string>();
        if (role != "system" && role != "developer" && role != "user" && role != "assistant") {
          result.error = "Unsupported message role '" + role + "'; only system, developer, user, and assistant are supported.";
          return result;
        }
        if ((role == "system" || role == "developer") && !input.empty()) {
          result.error = "System and developer messages must precede conversation history for Codex.";
          return result;
        }
        detail::json parts = detail::json::array();
        if (!detail::append_content(message["content"], role, parts, result.error)) return result;
        if (role == "system" || role == "developer") {
          for (const auto &part : parts) {
            if (!instructions.empty()) instructions += "\n\n";
            instructions += part["text"].get<std::string>();
          }
        }
        else if (role == "assistant") {
          for (auto &part : parts) part["annotations"] = detail::json::array();
          input.push_back({
            { "type", "message" },
            { "id", "msg_sunshine_" + std::to_string(message_index) },
            { "role", "assistant" },
            { "status", "completed" },
            { "content", std::move(parts) },
          });
        }
        else {
          input.push_back({ { "role", role }, { "content", std::move(parts) } });
        }
        ++message_index;
      }
      if (input.empty()) {
        result.error = "Codex request needs at least one user or assistant message.";
        return result;
      }
      // The ChatGPT Codex endpoint does not accept Chat Completions sampling/token fields.
      result.body = detail::json {
        { "model", result.model },
        { "instructions", instructions.empty() ? "You are a helpful assistant." : instructions },
        { "input", std::move(input) },
        { "store", false },
        { "stream", true },
        { "text", { { "verbosity", "low" } } },
        { "include", detail::json::array({ "reasoning.encrypted_content" }) },
        { "tool_choice", "auto" },
        { "parallel_tool_calls", true },
      }.dump();
      result.success = true;
    }
    catch (const detail::json::exception &e) {
      result.error = std::string("Invalid Chat Completions JSON: ") + e.what();
    }
    return result;
  }

  class sse_decoder_t {
  public:
    void
    feed(std::string_view chunk, std::function<void(const std::string &)> on_delta = {}) {
      if (!error_.empty() || completed_) return;
      for (const char ch : chunk) {
        if (pending_cr_) {
          process_line(on_delta);
          pending_cr_ = false;
          if (ch == '\n') continue;
        }
        if (ch == '\r') pending_cr_ = true;
        else if (ch == '\n') process_line(on_delta);
        else {
          line_ += ch;
          if (line_.size() + data_.size() > max_event_bytes) {
            error_ = "Codex response event exceeds 1 MiB.";
          }
        }
        if (!error_.empty() || completed_) return;
      }
    }

    void
    finish() {
      if (!error_.empty() || completed_) return;
      if (pending_cr_ || !line_.empty()) {
        process_line({});
        pending_cr_ = false;
      }
      if (!data_.empty() && error_.empty() && !completed_) dispatch({});
      if (!completed_ && error_.empty()) error_ = "Codex response stream ended before response.completed.";
    }

    [[nodiscard]] const std::string &text() const { return text_; }
    [[nodiscard]] const std::string &error() const { return error_; }
    [[nodiscard]] bool completed() const { return completed_; }

  private:
    static constexpr std::size_t max_event_bytes = 1024 * 1024;
    std::string line_;
    std::string data_;
    std::string event_name_;
    std::string text_;
    std::string error_;
    bool pending_cr_ = false;
    bool data_seen_ = false;
    bool completed_ = false;

    void
    append_text(const std::string &delta, const std::function<void(const std::string &)> &on_delta) {
      text_ += delta;
      if (on_delta && !delta.empty()) on_delta(delta);
    }

    void
    process_line(const std::function<void(const std::string &)> &on_delta) {
      if (line_.empty()) {
        dispatch(on_delta);
        return;
      }
      if (line_[0] != ':') {
        const auto colon = line_.find(':');
        const auto field = line_.substr(0, colon);
        const auto value = colon == std::string::npos ? std::string {} :
          line_.substr(colon + 1 + (colon + 1 < line_.size() && line_[colon + 1] == ' '));
        if (field == "data") {
          if (data_seen_) data_ += '\n';
          data_ += value;
          data_seen_ = true;
        }
        else if (field == "event") event_name_ = value;
      }
      line_.clear();
      if (line_.size() + data_.size() > max_event_bytes) error_ = "Codex response event exceeds 1 MiB.";
    }

    void
    dispatch(const std::function<void(const std::string &)> &on_delta) {
      data_seen_ = false;
      if (data_.empty()) {
        event_name_.clear();
        return;
      }
      std::string data = std::move(data_);
      data_.clear();
      std::string name = std::move(event_name_);
      event_name_.clear();
      if (data == "[DONE]") return;
      try {
        const auto event = detail::json::parse(data);
        if (!event.is_object()) {
          error_ = "Codex response event must be a JSON object.";
          return;
        }
        if (event.contains("type")) {
          if (!event["type"].is_string()) {
            error_ = "Codex response event type must be a string.";
            return;
          }
          name = event["type"].get<std::string>();
        }
        if (name == "response.output_text.delta" || name == "response.refusal.delta") {
          if (!event.contains("delta") || !event["delta"].is_string()) {
            error_ = "Codex text or refusal delta event is missing its delta string.";
            return;
          }
          append_text(event["delta"].get<std::string>(), on_delta);
        }
        else if (name == "response.refusal.done") {
          if (text_.empty() && event.contains("refusal") && event["refusal"].is_string()) {
            append_text(event["refusal"].get<std::string>(), on_delta);
          }
        }
        else if (name == "response.completed") {
          if (text_.empty() && event.contains("response") && event["response"].is_object()) {
            const auto &response = event["response"];
            if (response.contains("output") && response["output"].is_array()) {
              for (const auto &item : response["output"]) {
                if (!item.is_object() || item.value("type", "") != "message"
                    || !item.contains("content") || !item["content"].is_array()) continue;
                for (const auto &part : item["content"]) {
                  if (!part.is_object()) continue;
                  const auto type = part.value("type", std::string {});
                  const char *field = type == "output_text" ? "text" :
                                      type == "refusal" ? "refusal" : nullptr;
                  if (!field || !part.contains(field) || !part[field].is_string()) continue;
                  append_text(part[field].get<std::string>(), on_delta);
                }
              }
            }
          }
          completed_ = true;
        }
        else if (name == "response.failed" || name == "response.incomplete" || name == "error") {
          error_ = detail::event_error(event, "Codex response " + name + ".");
        }
      }
      catch (const detail::json::exception &) {
        error_ = "Malformed Codex response event JSON.";
      }
    }
  };

}  // namespace codex_responses

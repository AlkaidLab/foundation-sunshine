/**
 * @file src/ai/config_store.cpp
 * @brief AI configuration cache, migration and credential transactions.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "config_store.h"
#include "credential_store.h"
#include "src/config.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include <filesystem>
#include <fstream>
#include <mutex>
#ifdef _WIN32
  #include <windows.h>
#endif

using namespace std::literals;
using json = nlohmann::json;

namespace ai {
  namespace fs = std::filesystem;

  namespace {
    std::mutex ai_config_mutex;
    nlohmann::json ai_config_cache;
    bool ai_config_loaded = false;

    /**
     * @brief 获取 AI 配置文件路径（与 sunshine.conf 同目录）
     */
    static fs::path
    getAiConfigPath() {
      auto config_dir = file_handler::path_from_utf8(config::sunshine.config_file).parent_path();
      return config_dir / "ai_config.json";
    }

    static fs::path
    getAiCredentialPath() {
      auto config_dir = file_handler::path_from_utf8(config::sunshine.config_file).parent_path();
      return config_dir / "ai_llm_credential.bin";
    }

    static void
    applyStoredCredentialLocked(nlohmann::json &cfg) {
      auto credential = credential_store::read_llm_api_key(getAiCredentialPath());
      if (credential.status == credential_store::read_status_e::success) {
        cfg["apiKey"] = std::move(credential.secret);
      }
      else {
        cfg["apiKey"] = "";
        if (credential.status == credential_store::read_status_e::error) {
          BOOST_LOG(error) << "Could not load the LLM API key from secure storage: " << credential.error;
        }
      }
      cfg["apiKeyConfigured"] = !cfg.value("apiKey", "").empty();
    }

    static bool
    writeAiConfigFile(const nlohmann::json &cfg) {
      const fs::path path = getAiConfigPath();
      auto temp = path;
      temp += ".tmp";
      try {
        {
          std::ofstream file(temp, std::ios::trunc);
          if (!file.is_open()) return false;
          file << cfg.dump(2);
          file.flush();
          if (!file.good()) throw std::runtime_error("Failed to flush AI config");
        }
#ifdef _WIN32
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
          std::error_code ignored;
          fs::remove(temp, ignored);
          return false;
        }
#else
        std::error_code ec;
        fs::rename(temp, path, ec);
        if (ec) {
          fs::remove(temp, ec);
          return false;
        }
#endif
        return true;
      }
      catch (const std::exception &e) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        BOOST_LOG(error) << "Failed to save AI config: " << e.what();
        return false;
      }
    }

    /**
     * @brief 从文件或缓存读取 AI 配置（调用方需持有 ai_config_mutex）
     */
    static nlohmann::json
    loadAiConfigLocked() {
      if (ai_config_loaded) {
        return ai_config_cache;
      }

      auto path = getAiConfigPath();
      try {
        const auto utf8_path = file_handler::path_to_utf8(path);
        std::string content = file_handler::read_file(utf8_path.c_str());
        if (!content.empty()) {
          auto persisted = nlohmann::json::parse(content);
          ai_config_cache = persisted;

          // Migrate legacy plaintext credentials before exposing the runtime
          // configuration. Never remove the old value until both secure storage
          // and the sanitized config rewrite have succeeded.
          const std::string legacy_key = persisted.value("apiKey", "");
          if (!legacy_key.empty()) {
            auto migration = credential_store::write_llm_api_key(getAiCredentialPath(), legacy_key);
            if (migration.success) {
              auto sanitized = persisted;
              sanitized.erase("apiKey");
              if (writeAiConfigFile(sanitized)) {
                ai_config_cache = std::move(sanitized);
                BOOST_LOG(info) << "Migrated the LLM API key out of ai_config.json";
              }
              else {
                BOOST_LOG(error) << "The LLM API key was secured, but ai_config.json could not be sanitized; migration will retry";
              }
            }
            else {
              BOOST_LOG(error) << "Could not migrate the plaintext LLM API key: " << migration.error;
            }
            ai_config_cache["apiKey"] = legacy_key;
          }
          else {
            applyStoredCredentialLocked(ai_config_cache);
          }
          ai_config_cache["apiKeyConfigured"] = !ai_config_cache.value("apiKey", "").empty();
          ai_config_loaded = true;
          return ai_config_cache;
        }
      }
      catch (...) {
      }

      ai_config_cache = nlohmann::json {
        { "enabled", false },
        { "provider", "openai" },
        { "apiBase", "https://api.openai.com/v1" },
        { "apiKey", "" },
        { "apiKeyConfigured", false },
        { "model", "gpt-4.1-mini" },
        { "compatibility", "openai-chat" },
        { "temperature", 0.3 },
        { "max_tokens", 2048 }
      };
      applyStoredCredentialLocked(ai_config_cache);
      ai_config_loaded = true;
      return ai_config_cache;
    }

    /**
     * @brief 保存 AI 配置并刷新缓存（调用方需持有 ai_config_mutex）
     */
    static bool
    saveAiConfigLocked(const nlohmann::json &cfg) {
      auto persisted = cfg;
      persisted.erase("apiKey");
      persisted.erase("apiKeyConfigured");
      persisted.erase("apiKeyHint");
      if (!writeAiConfigFile(persisted)) return false;
      ai_config_cache = cfg;
      ai_config_loaded = true;
      return true;
    }

  }  // namespace

  /**
   * @brief 从文件或缓存读取 AI 配置（线程安全）
   */
  nlohmann::json
  loadAiConfig() {
    std::lock_guard<std::mutex> lock(ai_config_mutex);
    return loadAiConfigLocked();
  }

  nlohmann::json
  save_config(const std::string &request_body) {
    nlohmann::json output;
    try {
      auto input = nlohmann::json::parse(request_body);

      // 用同一把锁包住 load-modify-save，防止并发写入丢失
      std::lock_guard<std::mutex> lock(ai_config_mutex);
      auto current = loadAiConfigLocked();
      if (input.contains("enabled")) current["enabled"] = input["enabled"].get<bool>();
      if (input.contains("provider")) current["provider"] = input["provider"].get<std::string>();
      if (input.contains("apiBase")) current["apiBase"] = input["apiBase"].get<std::string>();
      if (input.contains("model")) current["model"] = input["model"].get<std::string>();
      if (input.contains("compatibility")) current["compatibility"] = input["compatibility"].get<std::string>();
      if (input.contains("system_prompt")) current["system_prompt"] = input["system_prompt"].get<std::string>();
      if (input.contains("temperature")) current["temperature"] = input["temperature"].get<double>();
      if (input.contains("max_tokens")) current["max_tokens"] = input["max_tokens"].get<int>();
      std::string key_action = input.value("apiKeyAction", "keep");
      // Backward compatibility for older clients that only send apiKey.
      if (input.contains("apiKey") && key_action == "keep") {
        const std::string candidate = input["apiKey"].get<std::string>();
        if (!candidate.empty() && candidate.find("****") == std::string::npos) {
          key_action = "replace";
        }
      }

      if (key_action == "replace") {
        if (!input.contains("apiKey") || !input["apiKey"].is_string() || input["apiKey"].get_ref<const std::string &>().empty()) {
          throw std::invalid_argument("apiKeyAction=replace requires a non-empty apiKey");
        }
        const std::string key = input["apiKey"].get<std::string>();
        auto stored = credential_store::write_llm_api_key(getAiCredentialPath(), key);
        if (!stored.success) throw std::runtime_error("Failed to secure API key: " + stored.error);
        current["apiKey"] = key;
      }
      else if (key_action == "clear") {
        auto erased = credential_store::erase_llm_api_key(getAiCredentialPath());
        if (!erased.success) throw std::runtime_error("Failed to clear API key: " + erased.error);
        current["apiKey"] = "";
      }
      else if (key_action != "keep") {
        throw std::invalid_argument("apiKeyAction must be keep, replace, or clear");
      }
      current["apiKeyConfigured"] = !current.value("apiKey", "").empty();

      if (saveAiConfigLocked(current)) {
        output["status"] = "ok";
        output["apiKeyConfigured"] = current["apiKeyConfigured"];
      }
      else {
        output["status"] = "error";
        output["error"] = "Failed to write config file";
        // Credential mutations have already completed. Keep runtime behavior
        // consistent with secure storage even if the non-secret JSON write
        // failed; the caller still receives an error and can retry.
        ai_config_cache = current;
        ai_config_loaded = true;
      }
    }
    catch (const std::exception &e) {
      output["status"] = "error";
      output["error"] = e.what();
    }

    return output;
  }

}  // namespace ai

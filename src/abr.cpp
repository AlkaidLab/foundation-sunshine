/**
 * @file src/abr.cpp
 * @brief Adaptive Bitrate (ABR) decision engine using LLM AI.
 *
 * Uses the configured LLM API to make intelligent bitrate decisions
 * based on client network feedback and the running game type.
 * Falls back to simple threshold logic when the LLM is unavailable.
 */

#include "abr.h"
#include "config.h"
#include "confighttp.h"
#include "logging.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

#ifdef _WIN32
  #include <Windows.h>
  #include <Psapi.h>
#endif

using json = nlohmann::json;

namespace abr {

  static std::mutex sessions_mutex;
  static std::unordered_map<std::string, session_state_t> sessions;

  /**
   * @brief Sanitize client-provided network feedback values.
   */
  static network_feedback_t
  sanitize_feedback(const network_feedback_t &raw) {
    return {
      std::clamp(raw.packet_loss, 0.0, 100.0),
      std::max(raw.rtt_ms, 0.0),
      std::max(raw.decode_fps, 0.0),
      std::max(raw.dropped_frames, 0),
      std::max(raw.current_bitrate_kbps, 0),
    };
  }

  /**
   * @brief Detect the actual foreground window title and process name.
   *
   * When users launch games through Steam/Epic/etc., the app_name from config
   * is just "Steam Big Picture". This function gets the real active window info.
   *
   * @return Pair of (window_title, exe_name), or empty strings on failure.
   */
  struct foreground_info_t {
    std::string window_title;
    std::string exe_name;
    uint32_t pid = 0;
  };

  static foreground_info_t
  detect_foreground_app() {
#ifdef _WIN32
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return {};

    // Get window title
    wchar_t title_buf[256] = {};
    int len = GetWindowTextW(hwnd, title_buf, sizeof(title_buf) / sizeof(title_buf[0]));
    std::string window_title;
    if (len > 0) {
      // Convert wide string to UTF-8
      int utf8_len = WideCharToMultiByte(CP_UTF8, 0, title_buf, len, nullptr, 0, nullptr, nullptr);
      if (utf8_len > 0) {
        window_title.resize(utf8_len);
        WideCharToMultiByte(CP_UTF8, 0, title_buf, len, window_title.data(), utf8_len, nullptr, nullptr);
      }
    }

    // Get process ID and executable name
    std::string exe_name;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid > 0) {
      HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (hProcess) {
        wchar_t exe_buf[MAX_PATH] = {};
        DWORD buf_size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProcess, 0, exe_buf, &buf_size)) {
          // Extract just the filename
          std::wstring full_path(exe_buf, buf_size);
          auto last_sep = full_path.find_last_of(L"\\/");
          std::wstring name = (last_sep != std::wstring::npos) ? full_path.substr(last_sep + 1) : full_path;

          int utf8_len = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
          if (utf8_len > 0) {
            exe_name.resize(utf8_len);
            WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), exe_name.data(), utf8_len, nullptr, nullptr);
          }
        }
        CloseHandle(hProcess);
      }
    }

    return { window_title, exe_name, pid };
#else
    return {};
#endif
  }

  /// Convert mode enum to string
  static std::string
  mode_to_string(mode_e mode) {
    switch (mode) {
      case mode_e::QUALITY:
        return "quality";
      case mode_e::LOW_LATENCY:
        return "lowLatency";
      case mode_e::BALANCED:
      default:
        return "balanced";
    }
  }

  /**
   * @brief Load prompt template from external file.
   * File location: same directory as ai_config.json (config dir / abr_prompt.md).
   * Cached after first successful load. Returns empty string if file not found.
   */
  static const std::string &
  load_prompt_template() {
    static std::string cached;
    static bool loaded = false;
    if (loaded) return cached;

    try {
      auto config_dir = std::filesystem::path(config::sunshine.config_file).parent_path();
      auto path = config_dir / "abr_prompt.md";
      std::ifstream file(path);
      if (file.is_open()) {
        cached.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        BOOST_LOG(info) << "ABR: loaded prompt template from " << path;
      }
      else {
        BOOST_LOG(warning) << "ABR: prompt template not found at " << path << ", LLM decisions will be unavailable";
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "ABR: failed to load prompt template: " << e.what();
    }

    loaded = true;
    return cached;
  }

  /**
   * @brief Replace all occurrences of {{key}} with value in a string.
   */
  static std::string
  replace_placeholders(std::string tmpl, const std::vector<std::pair<std::string, std::string>> &vars) {
    for (const auto &[key, value] : vars) {
      std::string placeholder = "{{" + key + "}}";
      size_t pos = 0;
      while ((pos = tmpl.find(placeholder, pos)) != std::string::npos) {
        tmpl.replace(pos, placeholder.size(), value);
        pos += value.size();
      }
    }
    return tmpl;
  }

  /**
   * @brief Build the LLM prompt by filling the template with session state.
   */
  static std::string
  build_llm_prompt(const session_state_t &state) {
    // Format recent feedback
    std::ostringstream feedback_ss;
    for (auto it = state.recent_feedback.rbegin(); it != state.recent_feedback.rend(); ++it) {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - it->timestamp)
                       .count();
      feedback_ss << "- [" << elapsed << "s ago] "
                  << "loss=" << it->feedback.packet_loss << "%, "
                  << "rtt=" << it->feedback.rtt_ms << "ms, "
                  << "fps=" << it->feedback.decode_fps << ", "
                  << "dropped=" << it->feedback.dropped_frames << ", "
                  << "bitrate=" << it->feedback.current_bitrate_kbps << "Kbps\n";
    }

    int max_br = state.config.max_bitrate_kbps;

    return replace_placeholders(load_prompt_template(), {
      { "FOREGROUND_TITLE",  !state.foreground_title.empty() ? state.foreground_title
                             : !state.app_name.empty()       ? state.app_name
                             : "Unknown" },
      { "FOREGROUND_EXE",    !state.foreground_exe.empty() ? state.foreground_exe
                             : !state.app_name.empty()     ? state.app_name
                             : "Unknown" },
      { "MODE",              mode_to_string(state.config.mode) },
      { "CURRENT_BITRATE",   std::to_string(state.current_bitrate_kbps) },
      { "MIN_BITRATE",       std::to_string(state.config.min_bitrate_kbps) },
      { "MAX_BITRATE",       std::to_string(max_br) },
      { "RECENT_FEEDBACK",   feedback_ss.str() },
      { "FPS_RANGE",         std::to_string(int(max_br * 0.8)) + "-" + std::to_string(max_br) },
      { "ACTION_RANGE",      std::to_string(int(max_br * 0.6)) + "-" + std::to_string(int(max_br * 0.8)) },
      { "STRATEGY_RANGE",    std::to_string(int(max_br * 0.4)) + "-" + std::to_string(int(max_br * 0.6)) },
      { "DESKTOP_RANGE",     std::to_string(int(max_br * 0.2)) + "-" + std::to_string(int(max_br * 0.3)) },
    });
  }

  /**
   * @brief Load AI model parameters from ai_config.json (with defaults).
   * Reads: system_prompt, temperature, max_tokens.
   */
  struct llm_params_t {
    std::string system_prompt = "You are a streaming bitrate optimizer. Always respond with valid JSON only.";
    double temperature = 0.1;
    int max_tokens = 150;
  };

  static const llm_params_t &
  load_llm_params() {
    static llm_params_t cached;
    static bool loaded = false;
    if (loaded) return cached;

    try {
      auto config_dir = std::filesystem::path(config::sunshine.config_file).parent_path();
      auto path = config_dir / "ai_config.json";
      std::ifstream file(path);
      if (file.is_open()) {
        auto cfg = json::parse(file);
        if (cfg.contains("system_prompt"))  cached.system_prompt = cfg["system_prompt"].get<std::string>();
        if (cfg.contains("temperature"))    cached.temperature = cfg["temperature"].get<double>();
        if (cfg.contains("max_tokens"))     cached.max_tokens = cfg["max_tokens"].get<int>();
      }
    }
    catch (...) {}

    loaded = true;
    return cached;
  }

  /**
   * @brief Build the OpenAI-compatible request body for the LLM.
   */
  static std::string
  build_llm_request(const std::string &prompt) {
    const auto &params = load_llm_params();

    json request;
    request["messages"] = json::array({
      { { "role", "system" }, { "content", params.system_prompt } },
      { { "role", "user" }, { "content", prompt } },
    });
    request["temperature"] = params.temperature;
    request["max_tokens"] = params.max_tokens;
    request["stream"] = false;
    return request.dump();
  }

  /**
   * @brief Parse the LLM response to extract bitrate decision.
   * @return action_t with new_bitrate_kbps and reason.
   */
  static action_t
  parse_llm_response(const std::string &response_body, const session_state_t &state) {
    action_t action;
    try {
      auto resp = json::parse(response_body);

      // Extract the assistant's message content
      std::string content;
      if (resp.contains("choices") && !resp["choices"].empty()) {
        content = resp["choices"][0]["message"]["content"].get<std::string>();
      }
      else {
        action.reason = "llm_parse_error: no choices in response";
        return action;
      }

      // Strip markdown code fence if present
      if (content.find("```") != std::string::npos) {
        auto start = content.find('{');
        auto end = content.rfind('}');
        if (start != std::string::npos && end != std::string::npos) {
          content = content.substr(start, end - start + 1);
        }
      }

      auto decision = json::parse(content);

      int bitrate = decision.value("bitrate", 0);
      action.reason = decision.value("reason", "llm_decision");

      if (bitrate > 0) {
        // Clamp to configured range
        bitrate = std::clamp(bitrate, state.config.min_bitrate_kbps, state.config.max_bitrate_kbps);

        // Ignore negligible changes (< 2%)
        if (std::abs(bitrate - state.current_bitrate_kbps) < state.current_bitrate_kbps / 50) {
          action.new_bitrate_kbps = 0;
          action.reason = "no_change: delta too small";
        }
        else {
          action.new_bitrate_kbps = bitrate;
        }
      }
    }
    catch (const json::exception &e) {
      action.reason = std::string("llm_parse_error: ") + e.what();
      BOOST_LOG(warning) << "ABR LLM parse error: " << e.what() << " body: " << response_body.substr(0, 200);
    }

    return action;
  }

  /**
   * @brief Simple fallback when LLM is unavailable.
   */
  static action_t
  fallback_decision(session_state_t &state, const network_feedback_t &feedback) {
    action_t action;

    if (feedback.packet_loss > 5.0) {
      state.consecutive_high_loss++;
      state.stable_ticks = 0;
      int new_bitrate = static_cast<int>(state.current_bitrate_kbps * 0.70);
      new_bitrate = std::clamp(new_bitrate, state.config.min_bitrate_kbps, state.config.max_bitrate_kbps);
      action.new_bitrate_kbps = new_bitrate;
      action.reason = "fallback: emergency_drop";
    }
    else if (feedback.packet_loss > 2.0) {
      state.consecutive_high_loss = 0;
      state.stable_ticks = 0;
      int new_bitrate = static_cast<int>(state.current_bitrate_kbps * 0.90);
      new_bitrate = std::clamp(new_bitrate, state.config.min_bitrate_kbps, state.config.max_bitrate_kbps);
      action.new_bitrate_kbps = new_bitrate;
      action.reason = "fallback: moderate_drop";
    }
    else if (feedback.packet_loss < 0.5) {
      state.consecutive_high_loss = 0;
      state.stable_ticks++;
      if (state.stable_ticks >= 5) {
        int new_bitrate = static_cast<int>(state.current_bitrate_kbps * 1.05);
        new_bitrate = std::clamp(new_bitrate, state.config.min_bitrate_kbps, state.config.max_bitrate_kbps);
        if (new_bitrate != state.current_bitrate_kbps) {
          action.new_bitrate_kbps = new_bitrate;
          action.reason = "fallback: probe_up";
        }
      }
    }

    return action;
  }

  void
  enable(const std::string &client_name, const config_t &cfg, int initial_bitrate_kbps, const std::string &app_name) {
    std::lock_guard lock(sessions_mutex);

    auto &state = sessions[client_name];
    state.config = cfg;
    state.config.enabled = true;
    state.initial_bitrate_kbps = initial_bitrate_kbps;
    state.current_bitrate_kbps = initial_bitrate_kbps;
    state.app_name = app_name;
    state.recent_feedback.clear();
    state.consecutive_high_loss = 0;
    state.stable_ticks = 0;
    state.last_llm_call = std::chrono::steady_clock::time_point {};
    state.last_fg_detect = std::chrono::steady_clock::time_point {};
    state.last_fg_pid = 0;
    state.cached_action = {};
    state.llm_in_flight = false;
    static uint64_t generation_counter = 0;
    state.generation = ++generation_counter;
    state.created_time = std::chrono::steady_clock::now();

    // Initial foreground detection
    auto fg = detect_foreground_app();
    if (!fg.window_title.empty()) {
      state.foreground_title = fg.window_title;
      state.foreground_exe = fg.exe_name;
      state.last_fg_pid = fg.pid;
      state.last_fg_detect = std::chrono::steady_clock::now();
    }

    // Apply mode presets if min/max not explicitly configured
    if (cfg.min_bitrate_kbps <= 0 || cfg.max_bitrate_kbps <= 0) {
      switch (cfg.mode) {
        case mode_e::QUALITY:
          state.config.min_bitrate_kbps = std::max(5000, initial_bitrate_kbps / 2);
          state.config.max_bitrate_kbps = std::min(150000, initial_bitrate_kbps * 3 / 2);
          break;
        case mode_e::LOW_LATENCY:
          state.config.min_bitrate_kbps = 2000;
          state.config.max_bitrate_kbps = initial_bitrate_kbps * 6 / 5;
          break;
        case mode_e::BALANCED:
        default:
          state.config.min_bitrate_kbps = std::max(3000, initial_bitrate_kbps * 3 / 10);
          state.config.max_bitrate_kbps = std::min(150000, initial_bitrate_kbps * 2);
          break;
      }
      // Guard against inverted range when initial_bitrate is very low
      if (state.config.min_bitrate_kbps > state.config.max_bitrate_kbps) {
        state.config.min_bitrate_kbps = state.config.max_bitrate_kbps;
      }
    }

    BOOST_LOG(info) << "ABR enabled for client '" << client_name
                    << "': app=" << app_name
                    << " mode=" << mode_to_string(cfg.mode)
                    << " bitrate=" << initial_bitrate_kbps
                    << " range=[" << state.config.min_bitrate_kbps
                    << "," << state.config.max_bitrate_kbps << "] Kbps";
  }

  void
  disable(const std::string &client_name) {
    std::lock_guard lock(sessions_mutex);
    sessions.erase(client_name);
    BOOST_LOG(info) << "ABR disabled for client '" << client_name << "'";
  }

  bool
  is_enabled(const std::string &client_name) {
    std::lock_guard lock(sessions_mutex);
    auto it = sessions.find(client_name);
    return it != sessions.end() && it->second.config.enabled;
  }

  /**
   * @brief Background worker: calls LLM and stores result.
   * Spawned as detached thread; communicates via sessions map.
   */
  static void
  llm_worker(const std::string &client_name, uint64_t generation, std::string request_body, network_feedback_t last_feedback) {
    auto result = confighttp::processAiChat(request_body);

    std::lock_guard lock(sessions_mutex);
    auto it = sessions.find(client_name);
    if (it == sessions.end() || it->second.generation != generation) {
      return;  // Session was cleaned up or re-created while LLM was in flight
    }
    auto &state = it->second;
    state.llm_in_flight = false;

    if (result.httpCode != 200) {
      BOOST_LOG(warning) << "ABR LLM call failed (HTTP " << result.httpCode << "), using fallback";
      state.cached_action = fallback_decision(state, last_feedback);
    }
    else {
      state.cached_action = parse_llm_response(result.body, state);
    }

    if (state.cached_action.new_bitrate_kbps > 0) {
      state.current_bitrate_kbps = state.cached_action.new_bitrate_kbps;
      state.stable_ticks = 0;
      BOOST_LOG(info) << "ABR LLM decision for '" << client_name
                      << "': " << state.cached_action.new_bitrate_kbps << " Kbps"
                      << " (" << state.cached_action.reason << ")";
    }
  }

  action_t
  process_feedback(const std::string &client_name, const network_feedback_t &raw_feedback) {
    auto feedback = sanitize_feedback(raw_feedback);

    std::lock_guard lock(sessions_mutex);

    auto it = sessions.find(client_name);
    if (it == sessions.end() || !it->second.config.enabled) {
      return { 0, "ABR not enabled" };
    }

    auto &state = it->second;
    auto now = std::chrono::steady_clock::now();

    // Update current bitrate from client report, clamped to session range
    if (feedback.current_bitrate_kbps > 0) {
      state.current_bitrate_kbps = std::clamp(
        feedback.current_bitrate_kbps,
        state.config.min_bitrate_kbps,
        state.config.max_bitrate_kbps);
    }

    // Add to feedback history
    state.recent_feedback.push_back({ feedback, now });
    while (state.recent_feedback.size() > session_state_t::MAX_FEEDBACK_HISTORY) {
      state.recent_feedback.pop_front();
    }

    // Consume cached action from completed LLM call (if any)
    action_t result_action;
    if (state.cached_action.new_bitrate_kbps > 0) {
      result_action = std::exchange(state.cached_action, {});
    }

    // Emergency: high packet loss — immediate fallback regardless of LLM state
    if (feedback.packet_loss > 5.0) {
      auto action = fallback_decision(state, feedback);
      if (action.new_bitrate_kbps > 0) {
        state.current_bitrate_kbps = action.new_bitrate_kbps;
        result_action = action;
      }
      return result_action;
    }

    // Decide whether to launch a new LLM call
    auto since_last_llm = std::chrono::duration_cast<std::chrono::seconds>(now - state.last_llm_call).count();
    bool should_call_llm = since_last_llm >= session_state_t::LLM_CALL_INTERVAL_SECONDS;

    if (!should_call_llm || state.llm_in_flight) {
      return result_action;
    }

    // LLM not available or prompt template missing → use fallback synchronously
    if (!confighttp::isAiEnabled() || load_prompt_template().empty()) {
      auto action = fallback_decision(state, feedback);
      if (action.new_bitrate_kbps > 0) {
        state.current_bitrate_kbps = action.new_bitrate_kbps;
        BOOST_LOG(info) << "ABR fallback for '" << client_name
                        << "': " << action.new_bitrate_kbps << " Kbps"
                        << " (" << action.reason << ")";
      }
      // Merge: prefer new fallback over stale cached
      if (action.new_bitrate_kbps > 0) {
        result_action = action;
      }
      return result_action;
    }

    // Detect foreground window/process (rate limited)
    auto since_last_fg = std::chrono::duration_cast<std::chrono::seconds>(now - state.last_fg_detect).count();
    if (since_last_fg >= session_state_t::FG_DETECT_INTERVAL_SECONDS) {
      auto fg = detect_foreground_app();
      state.last_fg_detect = now;
      if (fg.pid > 0 && fg.pid != state.last_fg_pid) {
        state.foreground_title = fg.window_title;
        state.foreground_exe = fg.exe_name;
        state.last_fg_pid = fg.pid;
        BOOST_LOG(debug) << "ABR: foreground changed to '" << fg.window_title
                         << "' (" << fg.exe_name << ") pid=" << fg.pid;
      }
      else if (fg.pid == state.last_fg_pid && !fg.window_title.empty()) {
        state.foreground_title = fg.window_title;
      }
    }

    // Build prompt and launch async LLM call
    state.last_llm_call = now;
    state.llm_in_flight = true;

    auto prompt = build_llm_prompt(state);
    auto request_body = build_llm_request(prompt);

    std::thread(llm_worker, client_name, state.generation, std::move(request_body), feedback).detach();

    return result_action;
  }

  capabilities_t
  get_capabilities() {
    return { true, 1 };
  }

  void
  cleanup(const std::string &client_name) {
    std::lock_guard lock(sessions_mutex);
    sessions.erase(client_name);
    BOOST_LOG(debug) << "ABR state cleaned up for client '" << client_name << "'";
  }

}  // namespace abr

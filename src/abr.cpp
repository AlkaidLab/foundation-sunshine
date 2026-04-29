/**
 * @file src/abr.cpp
 * @brief Adaptive Bitrate (ABR) decision engine using LLM AI.
 *
 * Architecture: two-tier bitrate control —
 *   1. Real-time fallback: threshold-based reactions to network conditions
 *      (always active, rate-limited to every 3 seconds).
 *   2. Event-driven LLM: queries the configured LLM API for optimal target
 *      bitrate on app switches and network recovery events.
 *
 * The fallback layer handles immediate network degradation, while the LLM
 * provides intelligent per-game bitrate targets that guide probe-up behavior.
 */

#include "abr.h"
#include "config.h"
#include "confighttp.h"
#include "logging.h"

#include <algorithm>
#include <cctype>
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
   * Search order: config dir (user override) → assets dir (bundled default).
   * Cached after first successful load. Returns empty string if not found.
   */
  static const std::string &
  load_prompt_template() {
    static std::string cached;
    static bool loaded = false;
    if (loaded) return cached;

    // Search paths: config dir first (user override), then assets dir (bundled default)
    std::vector<std::filesystem::path> search_paths;
    try {
      search_paths.push_back(std::filesystem::path(config::sunshine.config_file).parent_path() / "abr_prompt.md");
    }
    catch (...) {}
    search_paths.push_back(std::filesystem::path(SUNSHINE_ASSETS_DIR) / "abr_prompt.md");

    for (const auto &path : search_paths) {
      try {
        std::ifstream file(path);
        if (file.is_open()) {
          cached.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
          BOOST_LOG(info) << "ABR: loaded prompt template from " << path;
          loaded = true;
          return cached;
        }
      }
      catch (...) {}
    }

    BOOST_LOG(warning) << "ABR: prompt template not found, LLM decisions will be unavailable";
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

  static std::string
  trim_copy(std::string text) {
    auto is_not_space = [](unsigned char ch) {
      return !std::isspace(ch);
    };

    auto begin = std::find_if(text.begin(), text.end(), is_not_space);
    if (begin == text.end()) {
      return {};
    }

    auto end = std::find_if(text.rbegin(), text.rend(), is_not_space).base();
    return std::string(begin, end);
  }

  static size_t
  find_case_insensitive(const std::string &haystack, const std::string &needle, size_t start_pos = 0) {
    if (needle.empty() || start_pos >= haystack.size()) {
      return std::string::npos;
    }

    auto it = std::search(
      haystack.begin() + static_cast<std::ptrdiff_t>(start_pos),
      haystack.end(),
      needle.begin(),
      needle.end(),
      [](unsigned char lhs, unsigned char rhs) {
        return std::tolower(lhs) == std::tolower(rhs);
      });

    return it == haystack.end() ? std::string::npos : static_cast<size_t>(it - haystack.begin());
  }

  static void
  strip_reasoning_blocks(std::string &text) {
    static constexpr auto think_open = "<think";
    static constexpr auto think_close = "</think>";

    size_t open = find_case_insensitive(text, think_open);
    while (open != std::string::npos) {
      size_t open_end = text.find('>', open);
      if (open_end == std::string::npos) {
        break;
      }

      size_t close = find_case_insensitive(text, think_close, open_end + 1);
      if (close == std::string::npos) {
        text.erase(open, open_end - open + 1);
        break;
      }

      text.erase(open, close + std::char_traits<char>::length(think_close) - open);
      open = find_case_insensitive(text, think_open, open);
    }
  }

  static std::string
  extract_first_json_object(const std::string &text) {
    size_t start = std::string::npos;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;

    for (size_t i = 0; i < text.size(); ++i) {
      char ch = text[i];

      if (start == std::string::npos) {
        if (ch == '{') {
          start = i;
          depth = 1;
          in_string = false;
          escaped = false;
        }
        continue;
      }

      if (in_string) {
        if (escaped) {
          escaped = false;
        }
        else if (ch == '\\') {
          escaped = true;
        }
        else if (ch == '"') {
          in_string = false;
        }
        continue;
      }

      if (ch == '"') {
        in_string = true;
      }
      else if (ch == '{') {
        ++depth;
      }
      else if (ch == '}') {
        --depth;
        if (depth == 0) {
          return text.substr(start, i - start + 1);
        }
      }
    }

    return {};
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
    std::string system_prompt = "You are a streaming bitrate optimizer. Respond with a single valid JSON object only. Do not include markdown, code fences, or reasoning tags such as <think>.";
    double temperature = 0.1;
    // Reasoning models (e.g. "Coder", DeepSeek-R1, QwQ) emit long <think> blocks before the JSON.
    // 150 tokens almost always truncates them mid-thought, leaving no JSON to parse.
    // 1024 gives enough headroom for ~600-800 reasoning tokens + the small JSON answer.
    int max_tokens = 1024;
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
    std::string parse_target;
    try {
      auto resp = json::parse(response_body);

      // Extract the assistant's message content
      std::string content;
      std::string finish_reason;
      if (resp.contains("choices") && !resp["choices"].empty()) {
        content = resp["choices"][0]["message"]["content"].get<std::string>();
        if (resp["choices"][0].contains("finish_reason") && resp["choices"][0]["finish_reason"].is_string()) {
          finish_reason = resp["choices"][0]["finish_reason"].get<std::string>();
        }
      }
      else {
        action.reason = "llm_parse_error: no choices in response";
        return action;
      }

      parse_target = trim_copy(content);
      if (!json::accept(parse_target)) {
        auto normalized = trim_copy(content);
        strip_reasoning_blocks(normalized);
        normalized = trim_copy(normalized);

        if (json::accept(normalized)) {
          parse_target = normalized;
        }
        else {
          parse_target = extract_first_json_object(normalized);
        }
      }

      if (parse_target.empty()) {
        // Detect the common case: a reasoning model (e.g. "Coder") emitted a <think>
        // block but ran out of tokens before producing the JSON answer.
        bool truncated_reasoning =
          finish_reason == "length" &&
          find_case_insensitive(content, "<think") != std::string::npos;

        if (truncated_reasoning) {
          action.reason = "llm_truncated: reasoning exceeded max_tokens";
          BOOST_LOG(info) << "ABR LLM response truncated mid-reasoning (finish_reason=length); "
                          << "increase ai_config.json max_tokens (current default 1024) for this model. "
                          << "content: " << content.substr(0, 160);
        }
        else {
          action.reason = "llm_parse_error: no JSON object in content";
          BOOST_LOG(warning) << "ABR LLM parse error: no JSON object in content. "
                             << "finish_reason=" << (finish_reason.empty() ? "<none>" : finish_reason)
                             << " body: " << response_body.substr(0, 200)
                             << " content: " << content.substr(0, 200);
        }
        return action;
      }

      auto decision = json::parse(parse_target);

      int bitrate = decision.value("bitrate", 0);
      action.reason = decision.value("reason", "llm_decision");

      if (bitrate > 0) {
        // Clamp to configured range
        bitrate = std::clamp(bitrate, state.config.min_bitrate_kbps, state.config.max_bitrate_kbps);

        // Always record the LLM's recommended target
        action.target_bitrate_kbps = bitrate;

        // Only signal an immediate action if the change is significant (>= 2%)
        if (std::abs(bitrate - state.current_bitrate_kbps) >= state.current_bitrate_kbps / 50) {
          action.new_bitrate_kbps = bitrate;
        }
        else {
          action.reason = "no_change: delta too small";
        }
      }
    }
    catch (const json::exception &e) {
      action.reason = std::string("llm_parse_error: ") + e.what();
      BOOST_LOG(warning) << "ABR LLM parse error: " << e.what()
                         << " body: " << response_body.substr(0, 200)
                         << " extracted: " << parse_target.substr(0, 200);
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
    state.llm_target_bitrate_kbps = 0;
    state.app_changed = true;  // Trigger initial LLM call for app classification
    state.network_recovered = false;
    state.last_llm_call = std::chrono::steady_clock::time_point {};
    state.last_fallback_time = std::chrono::steady_clock::time_point {};
    state.last_fg_detect = std::chrono::steady_clock::time_point {};
    state.last_fg_pid = 0;
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

    // Clamp current bitrate to the computed range
    state.current_bitrate_kbps = std::clamp(
      state.current_bitrate_kbps,
      state.config.min_bitrate_kbps,
      state.config.max_bitrate_kbps);

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
   * @brief Background worker: calls LLM and stores target bitrate recommendation.
   * Spawned as detached thread; communicates via sessions map.
   * Unlike old design, the LLM sets a target (not an immediate action).
   */
  static void
  llm_worker(const std::string &client_name, uint64_t generation, std::string request_body) {
    auto result = confighttp::processAiChat(request_body);

    std::lock_guard lock(sessions_mutex);
    auto it = sessions.find(client_name);
    if (it == sessions.end() || it->second.generation != generation) {
      return;  // Session was cleaned up or re-created while LLM was in flight
    }
    auto &state = it->second;
    state.llm_in_flight = false;

    if (result.httpCode != 200) {
      BOOST_LOG(warning) << "ABR LLM call failed (HTTP " << result.httpCode << ")";
      return;
    }

    auto action = parse_llm_response(result.body, state);
    if (action.target_bitrate_kbps > 0) {
      state.llm_target_bitrate_kbps = action.target_bitrate_kbps;
      BOOST_LOG(info) << "ABR LLM target for '" << client_name
                      << "': " << action.target_bitrate_kbps << " Kbps"
                      << " (" << action.reason << ")";
    }
  }

  action_t
  process_feedback(const std::string &client_name, const network_feedback_t &raw_feedback) {
    auto feedback = sanitize_feedback(raw_feedback);

    std::lock_guard lock(sessions_mutex);

    auto it = sessions.find(client_name);
    if (it == sessions.end() || !it->second.config.enabled) {
      return { .reason = "ABR not enabled" };
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

    // ── Phase 1: Foreground detection (rate limited) ──
    auto since_last_fg = std::chrono::duration_cast<std::chrono::seconds>(now - state.last_fg_detect).count();
    if (since_last_fg >= session_state_t::FG_DETECT_INTERVAL_SECONDS) {
      auto fg = detect_foreground_app();
      state.last_fg_detect = now;
      if (fg.pid > 0 && fg.pid != state.last_fg_pid) {
        state.foreground_title = fg.window_title;
        state.foreground_exe = fg.exe_name;
        state.last_fg_pid = fg.pid;
        state.app_changed = true;
        BOOST_LOG(info) << "ABR: foreground changed to '" << fg.window_title
                        << "' (" << fg.exe_name << ") pid=" << fg.pid;
      }
      else if (fg.pid == state.last_fg_pid && !fg.window_title.empty()) {
        state.foreground_title = fg.window_title;
      }
    }

    // ── Phase 2: Fallback decisions (real-time, rate-limited to FALLBACK_INTERVAL_SECONDS) ──
    action_t result_action;
    auto since_last_fallback = std::chrono::duration_cast<std::chrono::seconds>(now - state.last_fallback_time).count();
    bool can_fallback = since_last_fallback >= session_state_t::FALLBACK_INTERVAL_SECONDS;

    // Emergency: high packet loss — immediate, no rate limit
    if (feedback.packet_loss > 5.0) {
      auto action = fallback_decision(state, feedback);
      if (action.new_bitrate_kbps > 0) {
        state.current_bitrate_kbps = action.new_bitrate_kbps;
        state.last_fallback_time = now;
        result_action = action;
      }
      return result_action;
    }

    // Regular fallback: moderate loss, probe-up, etc.
    if (can_fallback) {
      state.last_fallback_time = now;
      auto action = fallback_decision(state, feedback);
      if (action.new_bitrate_kbps > 0) {
        // When probing up with LLM target, don't exceed the target
        if (state.llm_target_bitrate_kbps > 0 && action.reason.find("probe_up") != std::string::npos) {
          action.new_bitrate_kbps = std::min(action.new_bitrate_kbps, state.llm_target_bitrate_kbps);
          if (action.new_bitrate_kbps <= state.current_bitrate_kbps) {
            action.new_bitrate_kbps = 0;  // Already at or above LLM target, don't probe further
          }
        }
        if (action.new_bitrate_kbps > 0) {
          state.current_bitrate_kbps = action.new_bitrate_kbps;
          BOOST_LOG(info) << "ABR fallback for '" << client_name
                          << "': " << action.new_bitrate_kbps << " Kbps"
                          << " (" << action.reason << ")";
          result_action = action;
        }
      }
    }

    // Track network recovery: edge-trigger when stable_ticks crosses threshold
    if (state.stable_ticks == 5 && state.consecutive_high_loss == 0) {
      state.network_recovered = true;  // Signal for LLM (set once on transition)
    }

    // ── Phase 3: LLM trigger (event-driven, NOT periodic) ──
    bool should_trigger_llm = (state.app_changed || state.network_recovered)
                              && !state.llm_in_flight
                              && confighttp::isAiEnabled()
                              && !load_prompt_template().empty();

    if (should_trigger_llm) {
      auto since_last_llm = std::chrono::duration_cast<std::chrono::seconds>(now - state.last_llm_call).count();
      if (since_last_llm >= session_state_t::LLM_MIN_INTERVAL_SECONDS) {
        state.app_changed = false;
        state.network_recovered = false;
        state.last_llm_call = now;
        state.llm_in_flight = true;

        auto prompt = build_llm_prompt(state);
        auto request_body = build_llm_request(prompt);

        std::thread(llm_worker, client_name, state.generation, std::move(request_body)).detach();
      }
    }

    return result_action;
  }

  capabilities_t
  get_capabilities() {
    return { true, 1 };
  }

  void
  cleanup(const std::string &client_name) {
    disable(client_name);
  }

}  // namespace abr

#ifdef SUNSHINE_TESTS
  #include <gtest/gtest.h>

namespace {

  abr::session_state_t
  make_test_session_state(int current_bitrate_kbps = 20000, int min_bitrate_kbps = 10000, int max_bitrate_kbps = 50000) {
    abr::session_state_t state;
    state.current_bitrate_kbps = current_bitrate_kbps;
    state.config.min_bitrate_kbps = min_bitrate_kbps;
    state.config.max_bitrate_kbps = max_bitrate_kbps;
    return state;
  }

}  // namespace

TEST(AbrLlmParseTests, ExtractsJsonAfterThinkBlock) {
  auto response = nlohmann::json {
    {"choices", nlohmann::json::array({
      {
        {"message", {
          {"role", "assistant"},
          {"content", "<think>Need to compare packet loss and app type.</think>\n{\"bitrate\":42000,\"reason\":\"fps_game_upper_range\"}"},
        }},
      },
    })},
  };

  auto state = make_test_session_state(20000, 10000, 45000);
  auto action = abr::parse_llm_response(response.dump(), state);

  EXPECT_EQ(action.new_bitrate_kbps, 42000);
  EXPECT_EQ(action.target_bitrate_kbps, 42000);
  EXPECT_EQ(action.reason, "fps_game_upper_range");
}

TEST(AbrLlmParseTests, ExtractsFirstJsonObjectFromMixedContent) {
  auto response = nlohmann::json {
    {"choices", nlohmann::json::array({
      {
        {"message", {
          {"role", "assistant"},
          {"content", "Here is the result you asked for:\n```json\n{\"bitrate\":52000,\"reason\":\"quality_probe\"}\n```\nUse it carefully."},
        }},
      },
    })},
  };

  auto state = make_test_session_state(30000, 10000, 50000);
  auto action = abr::parse_llm_response(response.dump(), state);

  EXPECT_EQ(action.new_bitrate_kbps, 50000);
  EXPECT_EQ(action.target_bitrate_kbps, 50000);
  EXPECT_EQ(action.reason, "quality_probe");
}

TEST(AbrLlmParseTests, ReportsMissingJsonWhenContentHasOnlyReasoning) {
  auto response = nlohmann::json {
    {"choices", nlohmann::json::array({
      {
        {"message", {
          {"role", "assistant"},
          {"content", "<think>I should answer with JSON, but I forgot.</think>Still thinking..."},
        }},
      },
    })},
  };

  auto state = make_test_session_state();
  auto action = abr::parse_llm_response(response.dump(), state);

  EXPECT_EQ(action.new_bitrate_kbps, 0);
  EXPECT_EQ(action.target_bitrate_kbps, 0);
  EXPECT_EQ(action.reason, "llm_parse_error: no JSON object in content");
}

// Regression for the real-world log:
//   content starts with "<think>Let me analyze this step by step..." and the
//   response was cut off by max_tokens before the closing tag / JSON appeared.
TEST(AbrLlmParseTests, DetectsTruncatedReasoningWhenFinishReasonLength) {
  auto response = nlohmann::json {
    {"choices", nlohmann::json::array({
      {
        {"finish_reason", "length"},
        {"message", {
          {"role", "assistant"},
          {"content", "<think>Let me analyze this step by step:\n\n1. **Active Window/Process**: Sunshine Desktop / sunshine-gui.exe\n   - This is a desktop streaming application, not a game\n   - Category: Desktop/Productivity"},
        }},
      },
    })},
  };

  auto state = make_test_session_state();
  auto action = abr::parse_llm_response(response.dump(), state);

  EXPECT_EQ(action.new_bitrate_kbps, 0);
  EXPECT_EQ(action.target_bitrate_kbps, 0);
  EXPECT_EQ(action.reason, "llm_truncated: reasoning exceeded max_tokens");
}

#endif

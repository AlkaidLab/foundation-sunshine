/**
 * @file src/doctor/doctor.cpp
 * @brief Health check runner, JSON, redaction and the pure evaluators of every group.
 */
#include "doctor.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "src/network_reachability.h"

// The build identity of host feat/build-identity (GET /api/version, sunshine --version --json); absent until that branch is merged.
#if __has_include("src/build_info.h")
  #include "src/build_info.h"
  #define DOCTOR_HAS_BUILD_INFO 1
#endif

using namespace std::literals;

namespace doctor {
  namespace {
    std::string
    tr(lang_e lang, std::string_view en, std::string_view zh) {
      return std::string { lang == lang_e::zh ? zh : en };
    }

    item_t
    make(std::string id, std::string group, status_e status, std::string title, std::string detail = {}, fix_t fix = {}) {
      return item_t { std::move(id), std::move(group), status, std::move(title), std::move(detail), std::move(fix) };
    }

    fix_t
    fix(std::string text, std::string kind = {}, std::string target = {}, bool confirm = false) {
      fix_t result { std::move(text), std::nullopt };
      if (!kind.empty()) result.action = action_t { std::move(kind), std::move(target), confirm };
      return result;
    }

    fix_t
    open_setting(std::string text, std::string_view key) {
      return fix(std::move(text), "open", "setting:" + std::string { key });
    }

    fix_t
    open_page(std::string text, std::string_view page) {
      return fix(std::move(text), "open", "page:" + std::string { page });
    }

    fix_t
    repair(std::string text, std::string_view driver, lang_e lang) {
      fix_t result { std::move(text), action_t { "repair", "driver:" + std::string { driver }, true, 3, driver_effect(driver, lang) } };
      return result;
    }

    fix_t
    link(std::string text, std::string_view url) {
      return fix(std::move(text), "link", std::string { url });
    }

    std::string
    join(const std::vector<std::string> &parts, std::string_view separator) {
      std::string out;
      for (const auto &part : parts) {
        if (!out.empty()) out += separator;
        out += part;
      }
      return out;
    }

    std::string
    lower(std::string_view text) {
      std::string out { text };
      std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return out;
    }

    std::string
    format_bytes(std::uint64_t bytes) {
      char buffer[32];
      const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
      if (gib >= 10) std::snprintf(buffer, sizeof(buffer), "%.0f GB", gib);
      else std::snprintf(buffer, sizeof(buffer), "%.1f GB", gib);
      return buffer;
    }

    std::string
    format_duration(std::int64_t seconds, lang_e lang) {
      const auto days = seconds / 86400;
      const auto hours = (seconds % 86400) / 3600;
      const auto minutes = (seconds % 3600) / 60;
      std::ostringstream out;
      if (lang == lang_e::zh) {
        if (days) out << days << " 天 ";
        if (days || hours) out << hours << " 小时 ";
        out << minutes << " 分钟";
      }
      else {
        if (days) out << days << " d ";
        if (days || hours) out << hours << " h ";
        out << minutes << " min";
      }
      return out.str();
    }

    std::string
    encoder_label(std::string_view name) {
      if (name == "nvenc") return "NVENC";
      if (name == "quicksync") return "Intel QSV";
      if (name == "amdvce") return "AMD AMF";
      if (name == "vaapi") return "VA-API";
      if (name == "vulkan") return "Vulkan";
      if (name == "software") return "Software (x264/x265/SVT-AV1)";
      if (name == "videotoolbox") return "VideoToolbox";
      return std::string { name };
    }
  }  // namespace

  const std::vector<std::string_view> &
  group_ids() {
    static const std::vector<std::string_view> ids {
      "gpu", "encoder", "capture", "vdd", "audio", "input", "network", "system", "host"
    };
    return ids;
  }

  bool
  is_group(std::string_view id) {
    return std::ranges::find(group_ids(), id) != group_ids().end();
  }

  std::vector<std::string>
  parse_groups(std::string_view value, std::vector<std::string> &unknown) {
    std::vector<std::string> groups;
    std::size_t start = 0;
    while (start <= value.size()) {
      const auto end = value.find(',', start);
      auto part = value.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
      while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
      while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
      if (!part.empty()) {
        if (!is_group(part)) unknown.emplace_back(part);
        else if (std::ranges::find(groups, part) == groups.end()) groups.emplace_back(part);
      }
      if (end == std::string_view::npos) break;
      start = end + 1;
    }
    return groups;
  }

  std::string
  group_title(std::string_view id, lang_e lang) {
    if (id == "gpu") return tr(lang, "Graphics and driver", "显卡与驱动");
    if (id == "encoder") return tr(lang, "Encoders", "编码器");
    if (id == "capture") return tr(lang, "Screen capture", "画面采集");
    if (id == "vdd") return tr(lang, "Virtual display", "虚拟显示器");
    if (id == "audio") return tr(lang, "Audio", "音频");
    if (id == "input") return tr(lang, "Input and gamepads", "输入与手柄");
    if (id == "network") return tr(lang, "Network", "网络");
    if (id == "system") return tr(lang, "System", "系统");
    if (id == "host") return tr(lang, "Host status", "主机状态");
    return std::string { id };
  }

  std::string_view
  to_string(status_e status) {
    switch (status) {
      case status_e::pass: return "pass";
      case status_e::warn: return "warn";
      case status_e::fail: return "fail";
      case status_e::skip: return "skip";
    }
    return "skip";
  }

  lang_e
  parse_lang(std::string_view value) {
    return value.starts_with("zh") ? lang_e::zh : lang_e::en;
  }

  status_e
  worst(const std::vector<item_t> &items) {
    auto rank = [](status_e s) {
      switch (s) {
        case status_e::fail: return 3;
        case status_e::warn: return 2;
        case status_e::pass: return 1;
        case status_e::skip: return 0;
      }
      return 0;
    };
    status_e out = status_e::skip;
    for (const auto &item : items) {
      if (rank(item.status) > rank(out)) out = item.status;
    }
    return out;
  }

  std::string
  redact(std::string_view text) {
    std::string out { text };
    // Home folders: the user name is personal; keep the rest of the path.
    static const std::regex windows_home { R"(([A-Za-z]:\\Users\\)[^\\/\s"']+)", std::regex::icase };
    static const std::regex unix_home { R"((/home/|/Users/)[^/\s"']+)" };
    out = std::regex_replace(out, windows_home, "$1…");
    out = std::regex_replace(out, unix_home, "$1…");
    // MAC addresses: keep the vendor half, the same as the network inventory.
    static const std::regex mac { R"(\b([0-9A-Fa-f]{2}[:-][0-9A-Fa-f]{2}[:-][0-9A-Fa-f]{2})[:-][0-9A-Fa-f]{2}[:-][0-9A-Fa-f]{2}[:-][0-9A-Fa-f]{2}\b)" };
    out = std::regex_replace(out, mac, "$1:…");
    // Addresses go through the reachability mask (IPv4 /24, IPv6 /64), so the doctor masks the same way as the 网络 page.
    static const std::regex ipv4 { R"((^|[^\w.])((?:\d{1,3}\.){3}\d{1,3})(?![\w.]*\d))" };
    static const std::regex ipv6 { R"((^|[^\w:])((?:[0-9A-Fa-f]{1,4}:){2,7}[0-9A-Fa-f]{0,4}|(?:[0-9A-Fa-f]{1,4}:){1,6}:[0-9A-Fa-f]{1,4}))" };
    for (const auto *pattern : { &ipv4, &ipv6 }) {
      std::string result;
      auto begin = std::sregex_iterator(out.begin(), out.end(), *pattern);
      std::size_t last = 0;
      for (auto it = begin; it != std::sregex_iterator(); ++it) {
        const auto &match = *it;
        const auto address = match[2].str();
        const auto masked = reachability::mask_address(address);
        const auto start = static_cast<std::size_t>(match.position(2));
        result.append(out, last, start - last);
        result += masked.empty() ? address : masked;
        last = start + address.size();
      }
      result.append(out, last, std::string::npos);
      out = std::move(result);
    }
    return out;
  }

  std::string
  zh_punctuation(std::string_view text) {
    // UTF-8 code points, so a mark can look at its neighbours
    std::vector<char32_t> cps;
    for (std::size_t i = 0; i < text.size();) {
      const auto c = static_cast<unsigned char>(text[i]);
      const int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
      char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
      for (int k = 1; k < n && i + k < text.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
      cps.push_back(cp);
      i += n;
    }
    auto wide = [](char32_t cp) { return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF00 && cp <= 0xFFEF) || cp == 0x201C || cp == 0x201D; };
    for (std::size_t i = 0; i < cps.size(); ++i) {
      const bool before = i > 0 && wide(cps[i - 1]);
      const bool after = i + 1 < cps.size() && wide(cps[i + 1]);
      switch (cps[i]) {
        case U',': if (before || after) cps[i] = U'，'; break;
        case U';': if (before || after) cps[i] = U'；'; break;
        case U':': if (before || after) cps[i] = U'：'; break;
        case U'?': if (before) cps[i] = U'？'; break;
        case U'(': if (before || after) cps[i] = U'（'; break;
        case U')': if (before) cps[i] = U'）'; break;
        default: break;
      }
    }
    // a full-width opening bracket pairs with the next ")"
    bool open = false;
    for (auto &cp : cps) {
      if (cp == U'（') open = true;
      else if (cp == U')' && open) { cp = U'）'; open = false; }
      else if (cp == U'）') open = false;
    }
    std::string out;
    for (const auto cp : cps) {
      if (cp < 0x80) out += static_cast<char>(cp);
      else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
      else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
      else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    return out;
  }

  void
  redact_item(item_t &item) {
    item.title = redact(item.title);
    item.detail = redact(item.detail);
    item.fix.text = redact(item.fix.text);
    if (item.fix.action) item.fix.action->effect = redact(item.fix.action->effect);
  }

  void
  complete_action(item_t &item, lang_e lang) {
    if (!item.fix.action) return;
    auto &a = *item.fix.action;
    if (a.level == 0) a.level = a.kind == "restart" ? 2 : a.kind == "repair" ? 3 : 1;
    if (a.level >= 2) a.confirm = true;
    if (a.kind == "restart" && a.effect.empty()) {
      a.effect = tr(lang, "Restarts the Sunshine service; it is back in about 10 seconds. A stream that is live ends; the settings stay.", "重启 Sunshine 服务,大约 10 秒后回来;正在进行的串流会断开,配置不变。");
    }
  }

  std::string
  driver_effect(std::string_view id, lang_e lang) {
    if (id == "vdd") return tr(lang, "Removes and recreates the virtual display device, then installs its driver. On a host without a monitor the picture is gone until it is back; the display a stream uses drops.", "删除并重建虚拟显示器设备,再安装驱动。没有物理显示器的主机在恢复前没有画面;正在串流用的显示器会掉。");
    if (id == "vigem") return tr(lang, "Installs ViGEmBus again from the package that ships with Sunshine (SHA-256 checked). Other programs that use it, such as DS4Windows, lose their gamepads for a moment; a restart may be needed.", "用随产品附带的安装包(已校验 SHA-256)重新安装 ViGEmBus。DS4Windows 等也用它的软件会短暂失去手柄,可能需要重启电脑。");
    if (id == "usbip") return tr(lang, "Installs the usbip-win2 transport again through the control panel's elevated helper. Every attached USB device is detached, remote keyboards, mice and gamepads included.", "通过控制面板的加固提权助手重新安装 usbip-win2。所有已挂载的 USB 设备都会断开,包括远程键鼠和手柄。");
    return {};
  }

  nlohmann::json
  to_json(const item_t &item) {
    nlohmann::json fix_json { { "text", item.fix.text } };
    if (item.fix.action) {
      fix_json["action"] = {
        { "kind", item.fix.action->kind },
        { "target", item.fix.action->target },
        { "confirm", item.fix.action->confirm },
        { "level", item.fix.action->level },
      };
      if (!item.fix.action->effect.empty()) fix_json["action"]["effect"] = item.fix.action->effect;
    }
    return {
      { "id", item.id },
      { "group", item.group },
      { "status", to_string(item.status) },
      { "title", item.title },
      { "detail", item.detail },
      { "fix", fix_json },
    };
  }

  nlohmann::json
  to_json(const report_t &report) {
    nlohmann::json groups = nlohmann::json::array();
    nlohmann::json items = nlohmann::json::array();
    nlohmann::json summary { { "pass", 0 }, { "warn", 0 }, { "fail", 0 }, { "skip", 0 } };
    for (const auto &group : report.groups) {
      nlohmann::json ids = nlohmann::json::array();
      for (const auto &item : group.items) {
        items.push_back(to_json(item));
        ids.push_back(item.id);
        summary[std::string { to_string(item.status) }] = summary[std::string { to_string(item.status) }].get<int>() + 1;
      }
      groups.push_back({
        { "id", group.id },
        { "title", group.title },
        { "status", to_string(group.status) },
        { "elapsed_ms", group.elapsed_ms },
        { "timed_out", group.timed_out },
        { "items", ids },
      });
    }
    return {
      { "version", report.version },
      { "source", "host" },
      { "streaming", report.streaming },
      { "checked_at", report.checked_at },
      { "elapsed_ms", report.elapsed_ms },
      { "summary", summary },
      { "groups", groups },
      { "items", items },
    };
  }

  // ---- runner ----

  report_t
  run(const std::vector<check_t> &checks, const std::vector<std::string> &only, lang_e lang) {
    struct slot_t {
      std::mutex mutex;
      std::condition_variable cv;
      bool done = false;
      std::vector<item_t> items;
      std::int64_t elapsed_ms = 0;
    };

    const auto started = std::chrono::steady_clock::now();
    report_t report;
    report.checked_at = reachability::format_utc_iso(reachability::now_epoch_seconds());

    std::vector<const check_t *> selected;
    for (const auto group : group_ids()) {
      if (!only.empty() && std::ranges::find(only, group) == only.end()) continue;
      for (const auto &check : checks) {
        if (check.group == group) selected.push_back(&check);
      }
    }

    // Each check gets its own detached thread: a probe that hangs in a driver must not hold the request.
    std::vector<std::shared_ptr<slot_t>> slots;
    for (const auto *check : selected) {
      auto slot = std::make_shared<slot_t>();
      slots.push_back(slot);
      std::thread([slot, run = check->run, group = check->group, lang]() {
        const auto begin = std::chrono::steady_clock::now();
        std::vector<item_t> items;
        try {
          items = run(lang);
        }
        catch (const std::exception &error) {
          items.push_back(make(group + ".error", group, status_e::fail, tr(lang, "The check stopped with an error", "检查出错中止"), error.what(), fix(tr(lang, "Run the check again; if it repeats, export the report.", "重新检查;反复出现时导出报告。"), "rerun", group)));
        }
        catch (...) {
          items.push_back(make(group + ".error", group, status_e::fail, tr(lang, "The check stopped with an error", "检查出错中止"), {}, fix(tr(lang, "Run the check again.", "重新检查。"), "rerun", group)));
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
        std::lock_guard lock { slot->mutex };
        slot->items = std::move(items);
        slot->elapsed_ms = elapsed;
        slot->done = true;
        slot->cv.notify_all();
      }).detach();
    }

    for (const auto group : group_ids()) {
      if (!only.empty() && std::ranges::find(only, group) == only.end()) continue;
      if (std::ranges::none_of(selected, [&](const check_t *check) { return check->group == group; })) continue;
      group_result_t result;
      result.id = std::string { group };
      result.title = group_title(group, lang);
      for (std::size_t i = 0; i < selected.size(); ++i) {
        if (selected[i]->group != group) continue;
        auto &slot = *slots[i];
        const auto deadline = started + selected[i]->timeout;
        std::unique_lock lock { slot.mutex };
        if (!slot.cv.wait_until(lock, deadline, [&] { return slot.done; })) {
          result.timed_out = true;
          const auto seconds = std::to_string(selected[i]->timeout.count() / 1000);
          result.items.push_back(make(result.id + ".timeout", result.id, status_e::warn,
            tr(lang, "The check did not finish in time", "检查超时"),
            tr(lang, "No result within " + seconds + " s; it keeps running in the background and its result is dropped.", "超过 " + seconds + " 秒没有结果;它在后台跑完,结果不计入。"),
            fix(tr(lang, "Run this group again in a minute.", "过一分钟重新检查这一组。"), "rerun", result.id)));
          result.elapsed_ms = std::max<std::int64_t>(result.elapsed_ms, selected[i]->timeout.count());
          continue;
        }
        for (auto &item : slot.items) {
          complete_action(item, lang);
          redact_item(item);
          if (lang == lang_e::zh) {
            item.title = zh_punctuation(item.title);
            item.detail = zh_punctuation(item.detail);
            item.fix.text = zh_punctuation(item.fix.text);
            if (item.fix.action) item.fix.action->effect = zh_punctuation(item.fix.action->effect);
          }
          result.items.push_back(item);
        }
        result.elapsed_ms = std::max(result.elapsed_ms, slot.elapsed_ms);
      }
      result.status = worst(result.items);
      report.groups.push_back(std::move(result));
    }
    report.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    return report;
  }

  // ---- versions ----

  int
  compare_versions(std::string_view a, std::string_view b) {
    auto parts = [](std::string_view text) {
      std::vector<long> out;
      long value = 0;
      bool any = false;
      for (const char c : text) {
        if (c >= '0' && c <= '9') {
          value = value * 10 + (c - '0');
          any = true;
        }
        else if (c == '.') {
          out.push_back(any ? value : 0);
          value = 0;
          any = false;
        }
        else {
          break;
        }
      }
      if (any) out.push_back(value);
      return out;
    };
    const auto left = parts(a);
    const auto right = parts(b);
    for (std::size_t i = 0; i < std::max(left.size(), right.size()); ++i) {
      const long l = i < left.size() ? left[i] : 0;
      const long r = i < right.size() ? right[i] : 0;
      if (l != r) return l < r ? -1 : 1;
    }
    return 0;
  }

  std::optional<std::string>
  nvidia_driver_version(std::string_view platform_version, bool windows) {
    if (platform_version.empty()) return std::nullopt;
    if (!windows) {
      if (platform_version.front() < '0' || platform_version.front() > '9') return std::nullopt;
      return std::string { platform_version };
    }
    // Windows reports the WDDM form "32.0.15.6094": the public version is the last five digits of the
    // last two fields, "56094" -> "560.94" (NVIDIA's own reading of the DriverVersion value).
    std::string digits;
    int dots = 0;
    for (auto it = platform_version.rbegin(); it != platform_version.rend(); ++it) {
      if (*it == '.') {
        if (++dots == 2) break;
        continue;
      }
      if (*it < '0' || *it > '9') return std::nullopt;
      digits.insert(digits.begin(), *it);
    }
    if (dots < 1) return std::nullopt;
    // the last field is zero-padded to four digits ("15.0612" -> "150612")
    if (digits.size() < 5) return std::nullopt;
    digits = digits.substr(digits.size() - 5);
    return digits.substr(0, 3) + "." + digits.substr(3);
  }

  const std::vector<nvenc_tier_t> &
  nvenc_tiers() {
    // Minimum drivers from third-party/nvenc-headers/<tier>/README ("Minimum required driver versions").
    static const std::vector<nvenc_tier_t> tiers {
      { "11.0", "456.71", "455.28", "H.264 / HEVC" },
      { "12.0", "522.25", "520.56.06", "AV1 (RTX 40 and newer)" },
      { "13.0", "570.0", "570.0", "AV1 / HEVC 4:2:2, RTX 50" },
      { "13.1", "610.0", "610.0", "SDK 13.1" },
    };
    return tiers;
  }

  // ---- 显卡与驱动 ----

  std::vector<item_t>
  evaluate_gpu(const gpu_facts_t &facts, lang_e lang) {
    std::vector<item_t> items;
    const std::string group = "gpu";
    if (facts.gpus.empty()) {
      items.push_back(make("gpu.present", group, status_e::fail, tr(lang, "No graphics adapter found", "没有找到显卡"),
        tr(lang, "The system reported no display adapter, so hardware encoding cannot work.", "系统没有报告任何显示适配器,硬件编码无法工作。"),
        fix(tr(lang, "Install the graphics driver from the GPU vendor, then restart Sunshine.", "从显卡厂商官网安装显卡驱动,然后重启 Sunshine。"), "restart", "sunshine", true)));
      return items;
    }

    for (std::size_t i = 0; i < facts.gpus.size(); ++i) {
      const auto &gpu = facts.gpus[i];
      const auto id = "gpu.driver." + std::to_string(i);
      const auto title = gpu.name.empty() ? tr(lang, "Graphics adapter", "显卡") : gpu.name;
      if (gpu.vendor == "nvidia") {
        const auto version = nvidia_driver_version(gpu.driver_version, facts.windows);
        if (!version) {
          items.push_back(make(id, group, status_e::skip, title,
            tr(lang, "Not checked: the driver version could not be read", "未检查:读不到驱动版本") + (gpu.driver_version.empty() ? "" : " (" + gpu.driver_version + ")")));
          continue;
        }
        const nvenc_tier_t *best = nullptr;
        for (const auto &tier : nvenc_tiers()) {
          if (compare_versions(*version, facts.windows ? tier.windows_min : tier.linux_min) >= 0) best = &tier;
        }
        const auto &oldest = nvenc_tiers().front();
        const auto &av1 = nvenc_tiers()[1];
        if (!best) {
          const auto min = std::string { facts.windows ? oldest.windows_min : oldest.linux_min };
          items.push_back(make(id, group, status_e::fail, title,
            tr(lang, "Driver " + *version + " is older than " + min + ", the minimum for NVENC", "驱动 " + *version + ",低于 NVENC 的最低要求 " + min),
            link(tr(lang, "Update the NVIDIA driver to " + min + " or newer.", "把 NVIDIA 驱动更新到 " + min + " 或更新。"), "https://www.nvidia.com/Download/index.aspx")));
        }
        else if (compare_versions(best->sdk, av1.sdk) < 0) {
          const auto min = std::string { facts.windows ? av1.windows_min : av1.linux_min };
          items.push_back(make(id, group, status_e::warn, title,
            tr(lang, "Driver " + *version + ": NVENC SDK " + std::string { best->sdk } + "; AV1 needs " + min + " or newer", "驱动 " + *version + "，NVENC SDK " + std::string { best->sdk } + "；AV1 需要 " + min + " 或更新"),
            link(tr(lang, "Update the NVIDIA driver to " + min + " or newer for AV1.", "要用 AV1,把 NVIDIA 驱动更新到 " + min + " 或更新。"), "https://www.nvidia.com/Download/index.aspx")));
        }
        else {
          items.push_back(make(id, group, status_e::pass, title,
            tr(lang, "Driver " + *version + ", NVENC SDK " + std::string { best->sdk }, "驱动 " + *version + ",NVENC SDK " + std::string { best->sdk })));
        }
        continue;
      }
      if (gpu.vendor == "amd" || gpu.vendor == "intel") {
        const auto vendor = gpu.vendor == "amd" ? "AMD"s : "Intel"s;
        items.push_back(make(id, group, status_e::pass, title,
          (gpu.driver_version.empty() ? tr(lang, "Driver version not reported", "驱动版本未报告") : tr(lang, "Driver ", "驱动 ") + gpu.driver_version) +
            tr(lang, "; the host has no minimum " + vendor + " driver table, the encoder probe decides", ";主机没有 " + vendor + " 最低驱动表,以编码器探测为准")));
        continue;
      }
      items.push_back(make(id, group, status_e::pass, title,
        gpu.driver_version.empty() ? tr(lang, "Driver version not reported", "驱动版本未报告") : tr(lang, "Driver ", "驱动 ") + gpu.driver_version));
    }

    // 混合显卡: a laptop with an iGPU and a dGPU streams from whichever adapter the display hangs on.
    const auto integrated = std::ranges::count_if(facts.gpus, [](const auto &g) { return g.integrated; });
    const auto discrete = static_cast<long>(facts.gpus.size()) - integrated;
    if (integrated > 0 && discrete > 0) {
      if (facts.configured_adapter.empty()) {
        items.push_back(make("gpu.hybrid", group, status_e::warn, tr(lang, "Hybrid graphics, no encoding adapter chosen", "混合显卡,没有指定编码显卡"),
          tr(lang, "Integrated and discrete GPUs are both present; without a choice the display's adapter encodes, often the integrated one.", "核显和独显同时存在;不指定时由显示器所在的显卡编码,通常是核显。"),
          open_setting(tr(lang, "Choose the discrete GPU in Adapter name.", "在「适配器名称」里选择独立显卡。"), "adapter_name")));
      }
      else {
        items.push_back(make("gpu.hybrid", group, status_e::pass, tr(lang, "Hybrid graphics", "混合显卡"),
          tr(lang, "Encoding adapter: ", "编码显卡:") + facts.configured_adapter));
      }
    }

    // 适配器选择: a configured adapter that matches nothing makes the host fall back silently.
    const bool any_streaming = std::ranges::any_of(facts.gpus, [](const auto &g) { return g.is_streaming_adapter; });
    if (!facts.configured_adapter.empty() && !any_streaming) {
      items.push_back(make("gpu.adapter", group, status_e::fail, tr(lang, "The configured adapter does not exist", "配置的显卡不存在"),
        tr(lang, "Adapter name is \"" + facts.configured_adapter + "\", which matches no adapter on this host.", "「适配器名称」是 \"" + facts.configured_adapter + "\",本机没有这块显卡。"),
        open_setting(tr(lang, "Pick an adapter from the list, or clear the setting.", "从列表里重新选择,或清空这一项。"), "adapter_name")));
    }
    else if (!facts.configured_adapter.empty() || facts.gpus.size() > 1) {
      std::string name;
      for (const auto &g : facts.gpus) {
        if (g.is_streaming_adapter) name = g.name;
      }
      items.push_back(make("gpu.adapter", group, status_e::pass, tr(lang, "Encoding adapter", "编码显卡"),
        name.empty() ? tr(lang, "Chosen by the system", "由系统选择") : name));
    }
    return items;
  }

  // ---- 编码器 ----

  std::vector<item_t>
  evaluate_encoders(const encoder_facts_t &facts, lang_e lang) {
    std::vector<item_t> items;
    const std::string group = "encoder";
    auto skipped_text = [&](std::string_view reason) {
      if (reason == "streaming") return tr(lang, "Not checked: a stream is running and a probe would disturb it", "未检查:正在串流,探测会打断画面");
      if (reason == "busy") return tr(lang, "Not checked: another probe kept the GPU for over 12 s", "未检查:另一次探测占着显卡超过 12 秒");
      if (reason == "no_display") return tr(lang, "Not checked: no active display to probe with", "未检查:没有可用于探测的活动显示器");
      if (reason == "timeout") return tr(lang, "This probe did not finish within 15 s; its result was dropped", "这次探测 15 秒内没有结束,结果不计入");
      return tr(lang, "Not checked", "未检查");
    };

    if (!facts.startup_error.empty()) {
      items.push_back(make("encoder.startup", group, status_e::fail, tr(lang, "The host's own encoder probe failed", "主机启动时的编码器探测失败"),
        facts.startup_error, fix(facts.startup_hint.empty() ? tr(lang, "Check the display and the driver, then probe again.", "检查显示器和驱动后重新检查。") : facts.startup_hint, "rerun", group)));
    }

    bool any_usable = false;
    bool any_ran = false;
    for (const auto &encoder : facts.encoders) {
      const auto id = "encoder." + encoder.name;
      const auto title = encoder_label(encoder.name);
      if (!encoder.ran) {
        const auto timed_out = encoder.skipped_reason == "timeout";
        items.push_back(make(id, group, timed_out ? status_e::warn : status_e::skip, title, skipped_text(encoder.skipped_reason),
          encoder.skipped_reason == "no_display" ? open_page(tr(lang, "Turn on a display or a virtual display, then check again.", "打开一块显示器或虚拟显示器后重新检查。"), "display") : fix(tr(lang, timed_out ? "Run this group again in a minute." : "Check again when no stream is running.", timed_out ? "过一分钟重新检查这一组。" : "没有串流时重新检查。"), "rerun", group)));
        continue;
      }
      any_ran = true;
      if (!encoder.usable) {
        std::string why;
        if (encoder.failure == "display") why = tr(lang, "Could not open the display for this encoder's capture", "无法为这个编码器打开显示器采集");
        else if (encoder.failure == "unsupported_gpu") why = tr(lang, "This GPU does not support it", "这块显卡不支持");
        else why = tr(lang, "An H.264 encode session could not be opened", "无法打开 H.264 编码会话");
        // A backend that does not match this GPU is expected to fail; only the configured one is a problem.
        const auto status = encoder.configured ? status_e::fail : status_e::pass;
        auto detail = encoder.configured ? why : tr(lang, "Not available here: ", "本机不可用:") + why;
        items.push_back(make(id, group, status, title, std::move(detail),
          encoder.configured ? open_setting(tr(lang, "Set Encoder to automatic, or install the driver for this GPU.", "把「编码器」设为自动,或安装这块显卡的驱动。"), "encoder") : fix_t {}));
        continue;
      }
      any_usable = true;
      std::vector<std::string> parts;
      auto codec = [&](std::string_view name, const codec_fact_t &c) {
        if (!c.tested) return;
        std::string text { name };
        text += c.passed ? " ✓" : " ✗";
        if (c.passed && c.hdr) text += " HDR";
        if (c.passed && c.yuv444) text += " 4:4:4";
        parts.push_back(text);
      };
      codec("H.264", encoder.h264);
      codec("HEVC", encoder.hevc);
      codec("AV1", encoder.av1);
      auto detail = join(parts, " · ");
      if (encoder.active) detail += tr(lang, " · in use", " · 正在使用");
      const bool hevc_wanted = facts.hevc_mode >= 2 && encoder.hevc.tested && !encoder.hevc.passed;
      const bool av1_wanted = facts.av1_mode >= 2 && encoder.av1.tested && !encoder.av1.passed;
      if ((encoder.active || encoder.configured) && (hevc_wanted || av1_wanted)) {
        const auto codec_name = hevc_wanted ? "HEVC"s : "AV1"s;
        items.push_back(make(id, group, status_e::warn, title, detail,
          open_setting(tr(lang, codec_name + " is forced on but this encoder cannot encode it; set it to automatic.", codec_name + " 设为强制开启,但这个编码器编不了;改回自动。"), hevc_wanted ? "hevc_mode" : "av1_mode")));
      }
      else {
        items.push_back(make(id, group, status_e::pass, title, detail));
      }
    }

    if (any_ran && !any_usable) {
      items.push_back(make("encoder.none", group, status_e::fail, tr(lang, "No encoder works", "没有能用的编码器"),
        tr(lang, "Every encoder failed, so no stream can start.", "所有编码器都失败,串流无法开始。"),
        link(tr(lang, "Update the graphics driver, then check again.", "更新显卡驱动后重新检查。"), "https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2troubleshooting.html")));
    }
    if (!facts.configured.empty()) {
      const bool known = std::ranges::any_of(facts.encoders, [](const auto &e) { return e.configured; });
      if (!known) {
        items.push_back(make("encoder.configured", group, status_e::fail, tr(lang, "The configured encoder does not exist", "配置的编码器不存在"),
          tr(lang, "Encoder is \"" + facts.configured + "\", which this build does not have.", "「编码器」是 \"" + facts.configured + "\",这个版本没有它。"),
          open_setting(tr(lang, "Set Encoder to automatic.", "把「编码器」设为自动。"), "encoder")));
      }
    }
    return items;
  }

  // ---- 画面采集 ----

  std::vector<item_t>
  evaluate_capture(const capture_fact_t &facts, lang_e lang) {
    const std::string group = "capture";
    const auto title = tr(lang, "Test capture", "试采集") + " (" + (facts.method.empty() ? "auto"s : facts.method) + ")";
    if (!facts.ran) {
      std::string detail;
      fix_t f;
      if (facts.skipped_reason == "streaming") {
        detail = tr(lang, "Not checked: a stream is running and owns the capture", "未检查:正在串流,采集被占用");
        f = fix(tr(lang, "Check again when no stream is running.", "没有串流时重新检查。"), "rerun", group);
      }
      else if (facts.skipped_reason == "no_display") {
        detail = tr(lang, "Not checked: no active display", "未检查:没有活动的显示器");
        f = open_page(tr(lang, "Turn on a display or a virtual display.", "打开一块显示器或虚拟显示器。"), "display");
      }
      else {
        detail = tr(lang, "Not checked: another probe kept the GPU for over 12 s", "未检查:另一次探测占着显卡超过 12 秒");
        f = fix(tr(lang, "Check again in a moment.", "稍后重新检查。"), "rerun", group);
      }
      return { make("capture.test", group, status_e::skip, title, detail, f) };
    }
    if (facts.ok) {
      auto detail = (facts.display.empty() ? ""s : facts.display + " · ") + std::to_string(facts.width) + "×" + std::to_string(facts.height) +
                    tr(lang, " · first frame in ", " · 首帧 ") + std::to_string(facts.first_frame_ms) + " ms" + tr(lang, " · nothing stored", " · 未保存画面");
      return { make("capture.test", group, status_e::pass, title, detail) };
    }
    if (facts.failure == "no_frame") {
      return { make("capture.test", group, status_e::warn, title,
        tr(lang, "The capture opened but delivered no frame in time", "采集已打开,但规定时间内没有拿到画面"),
        open_setting(tr(lang, "Try another Capture method, or wake the display.", "换一种「采集方式」,或唤醒显示器。"), "capture")) };
    }
    return { make("capture.test", group, status_e::fail, title,
      facts.failure == "open" ? tr(lang, "The capture could not be opened on ", "无法在这块显示器上打开采集:") + (facts.display.empty() ? tr(lang, "the selected display", "所选显示器") : facts.display) :
                                tr(lang, "The capture stopped with an error", "采集出错停止"),
      open_setting(tr(lang, "Choose another display or Capture method.", "换一块显示器或另一种「采集方式」。"), facts.failure == "open" ? "output_name" : "capture")) };
  }

  // ---- 虚拟显示器 ----

  std::vector<item_t>
  evaluate_vdd(const vdd_fact_t &facts, lang_e lang) {
    const std::string group = "vdd";
    std::vector<item_t> items;
    if (!facts.supported_platform) {
      items.push_back(make("vdd.driver", group, status_e::skip, tr(lang, "Foundation Virtual Display driver", "基地虚拟显示器驱动"),
        tr(lang, "Not checked: the virtual display driver exists only on Windows", "未检查:虚拟显示器驱动只有 Windows 版")));
      return items;
    }
    const auto version = facts.driver_version.empty() ? ""s : " · " + facts.driver_version;
    if (!facts.installed) {
      const auto status = facts.capture == "vdd" ? status_e::fail : status_e::warn;
      items.push_back(make("vdd.driver", group, status, tr(lang, "Foundation Virtual Display driver", "基地虚拟显示器驱动"),
        tr(lang, "Not installed; streaming without a monitor attached is not possible", "未安装;没有显示器时无法串流"),
        repair(tr(lang, "Install the virtual display (in the control panel, or Components).", "安装虚拟显示器(在控制面板里修复,或到「组件」)。"), "vdd", lang)));
      return items;
    }
    if (!facts.problem_code_valid) {
      items.push_back(make("vdd.driver", group, status_e::warn, tr(lang, "Foundation Virtual Display driver", "基地虚拟显示器驱动"),
        tr(lang, "Installed; Windows did not report its device state", "已安装;Windows 没有报告设备状态") + version,
        open_page(tr(lang, "Open Device Manager status in Components.", "在「组件」里查看设备状态。"), "components")));
    }
    else if (!facts.running) {
      const bool reboot = facts.problem_code == 14;
      items.push_back(make("vdd.driver", group, reboot ? status_e::warn : status_e::fail, tr(lang, "Foundation Virtual Display driver", "基地虚拟显示器驱动"),
        reboot ? tr(lang, "Installed; Windows needs a restart to start it (code 14)", "已安装;需要重启 Windows 才能启用(代码 14)") + version :
                 tr(lang, "Installed but not running (device problem code ", "已安装但没有运行(设备问题代码 ") + std::to_string(facts.problem_code) + ")" + version,
        reboot ? fix(tr(lang, "Restart Windows.", "重启 Windows。")) : repair(tr(lang, "Repair the virtual display (in the control panel, or Components).", "修复虚拟显示器(在控制面板里修复,或到「组件」)。"), "vdd", lang)));
    }
    else {
      items.push_back(make("vdd.driver", group, status_e::pass, tr(lang, "Foundation Virtual Display driver", "基地虚拟显示器驱动"),
        tr(lang, "Installed and running", "已安装并运行") + version));
    }

    // 创建/挂接: creating a monitor changes the display layout, so the doctor checks its precondition, the control channel.
    if (facts.running) {
      if (facts.control_available) {
        items.push_back(make("vdd.control", group, status_e::pass, tr(lang, "Create and attach", "创建与挂接"),
          (facts.monitor_active ? tr(lang, "A virtual monitor is attached now", "现在挂着一块虚拟显示器") : tr(lang, "Control channel answers", "控制通道有应答")) +
            tr(lang, "; no monitor was created (that would change the display layout)", ";没有实际创建(会改变显示布局)")));
      }
      else {
        items.push_back(make("vdd.control", group, status_e::fail, tr(lang, "Create and attach", "创建与挂接"),
          tr(lang, "The driver runs but its control channel does not answer, so Sunshine cannot create a monitor", "驱动在运行,但控制通道没有应答,Sunshine 无法创建显示器"),
          fix(tr(lang, "Restart Sunshine; if it repeats, reinstall the virtual display.", "重启 Sunshine;反复出现时重新安装虚拟显示器。"), "restart", "sunshine", true)));
      }
    }

    // HDR on an indirect display needs IddCx 1.10, which ships with Windows 11 22H2 (build 22621).
    if (facts.os_build) {
      if (*facts.os_build >= 22621) {
        items.push_back(make("vdd.hdr", group, status_e::pass, tr(lang, "HDR on the virtual display", "虚拟显示器 HDR"),
          tr(lang, "Available (Windows build ", "可用(Windows 版本号 ") + std::to_string(*facts.os_build) + ")"));
      }
      else {
        items.push_back(make("vdd.hdr", group, status_e::warn, tr(lang, "HDR on the virtual display", "虚拟显示器 HDR"),
          tr(lang, "Needs Windows 11 22H2 (build 22621); this is build ", "需要 Windows 11 22H2(版本号 22621);本机是 ") + std::to_string(*facts.os_build),
          fix(tr(lang, "Update Windows to stream HDR from the virtual display.", "要从虚拟显示器串流 HDR,请更新 Windows。"))));
      }
    }
    items.push_back(make("vdd.refresh", group, status_e::skip, tr(lang, "Refresh rates", "刷新率"),
      tr(lang, "Not checked: the driver reports its modes only for a monitor it has created", "未检查:驱动只为已创建的显示器报告模式")));
    return items;
  }

  // ---- 音频 ----

  std::vector<item_t>
  evaluate_audio(const audio_fact_t &facts, lang_e lang) {
    const std::string group = "audio";
    std::vector<item_t> items;
    if (!facts.stream_audio) {
      items.push_back(make("audio.sink", group, status_e::skip, tr(lang, "Audio output", "音频输出"),
        tr(lang, "Not checked: audio streaming is off", "未检查:没有开启音频串流"),
        open_setting(tr(lang, "Turn on Stream audio.", "打开「串流音频」。"), "stream_audio")));
    }
    else if (!facts.checked) {
      items.push_back(make("audio.sink", group, status_e::fail, tr(lang, "Audio output", "音频输出"),
        tr(lang, "The audio system could not be opened", "无法打开系统音频") + (facts.error.empty() ? "" : ": " + facts.error),
        fix(tr(lang, "Check that the Windows Audio service runs, then restart Sunshine.", "确认系统音频服务在运行,然后重启 Sunshine。"), "restart", "sunshine", true)));
    }
    else if (facts.sink_found) {
      items.push_back(make("audio.sink", group, status_e::pass, tr(lang, "Audio output", "音频输出"),
        facts.sink.empty() ? tr(lang, "System default output", "系统默认输出") : facts.sink));
    }
    else {
      items.push_back(make("audio.sink", group, status_e::fail, tr(lang, "Audio output", "音频输出"),
        facts.sink.empty() ? tr(lang, "There is no default output device", "没有默认输出设备") : tr(lang, "\"" + facts.sink + "\" does not exist", "\"" + facts.sink + "\" 不存在"),
        open_setting(tr(lang, "Choose an existing output in Audio sink.", "在「音频输出设备」里选一个存在的设备。"), "audio_sink")));
    }

    if (facts.stream_audio && facts.virtual_checked) {
      if (facts.virtual_found) {
        items.push_back(make("audio.virtual", group, status_e::pass, tr(lang, "Virtual audio device", "虚拟音频设备"), facts.virtual_name));
      }
      else {
        items.push_back(make("audio.virtual", group, status_e::warn, tr(lang, "Virtual audio device", "虚拟音频设备"),
          facts.virtual_sink.empty() ? tr(lang, "Steam Streaming Speakers not found; without it the host's own speakers play the sound too", "没有找到 Steam Streaming Speakers;没有它时主机自己的扬声器也会出声") :
                                       tr(lang, "\"" + facts.virtual_sink + "\" not found", "没有找到 \"" + facts.virtual_sink + "\""),
          open_setting(tr(lang, "Install the Steam audio drivers, or choose a Virtual sink.", "安装 Steam 音频驱动,或选择「虚拟输出设备」。"), "virtual_sink")));
      }
    }

    if (!facts.mic_checked) {
      items.push_back(make("audio.mic", group, status_e::skip, tr(lang, "Microphone", "麦克风"),
        tr(lang, "Not checked: microphone redirection is Windows only", "未检查:麦克风重定向只在 Windows 上")));
    }
    else if (facts.mic_state == "disabled" || facts.mic_backend == "none" || facts.mic_backend.empty()) {
      items.push_back(make("audio.mic", group, status_e::skip, tr(lang, "Microphone", "麦克风"),
        tr(lang, "Not checked: microphone redirection is off", "未检查:麦克风重定向未开启")));
    }
    else if (!facts.mic_error.empty() || facts.mic_state == "error") {
      items.push_back(make("audio.mic", group, status_e::fail, tr(lang, "Microphone", "麦克风"),
        facts.mic_backend + ": " + (facts.mic_error.empty() ? facts.mic_state : facts.mic_error),
        open_page(tr(lang, "Open the microphone settings and run the test.", "打开麦克风设置并运行测试。"), "audio")));
    }
    else if (!facts.mic_component) {
      items.push_back(make("audio.mic", group, status_e::warn, tr(lang, "Microphone", "麦克风"),
        facts.mic_backend + tr(lang, ": the virtual microphone device is missing", ":虚拟麦克风设备不存在"),
        open_page(tr(lang, "Install the virtual microphone from Components.", "在「组件」里安装虚拟麦克风。"), "components")));
    }
    else {
      items.push_back(make("audio.mic", group, status_e::pass, tr(lang, "Microphone", "麦克风"),
        facts.mic_backend + " · " + facts.mic_state));
    }
    return items;
  }

  // ---- 输入与手柄 ----

  std::vector<item_t>
  evaluate_input(const input_fact_t &facts, lang_e lang) {
    const std::string group = "input";
    std::vector<item_t> items;
    const bool gamepads_off = facts.gamepad == "disabled";
    if (facts.windows) {
      if (gamepads_off) {
        items.push_back(make("input.vigem", group, status_e::skip, "ViGEmBus", tr(lang, "Not checked: gamepads are off", "未检查:手柄已关闭")));
      }
      else if (!facts.vigem_checked) {
        items.push_back(make("input.vigem", group, status_e::skip, "ViGEmBus", tr(lang, "Not checked", "未检查")));
      }
      else if (facts.vigem_ok) {
        items.push_back(make("input.vigem", group, status_e::pass, "ViGEmBus", tr(lang, "Installed; a client connected", "已安装;客户端连接成功")));
      }
      else {
        items.push_back(make("input.vigem", group, status_e::fail, "ViGEmBus",
          tr(lang, "Not installed or not running", "未安装或没有运行") + (facts.vigem_error.empty() ? "" : " (" + facts.vigem_error + ")"),
          repair(tr(lang, "Install ViGEmBus (in the control panel, or Components), or turn gamepads off.", "安装 ViGEmBus(在控制面板里修复,或到「组件」),或关闭手柄。"), "vigem", lang)));
      }
    }
    else {
      if (!facts.uinput_checked) {
        items.push_back(make("input.uinput", group, status_e::skip, "uinput", tr(lang, "Not checked", "未检查")));
      }
      else if (facts.uinput_ok) {
        items.push_back(make("input.uinput", group, status_e::pass, "uinput", tr(lang, "/dev/uinput is writable", "/dev/uinput 可写")));
      }
      else {
        items.push_back(make("input.uinput", group, status_e::fail, "uinput",
          tr(lang, "/dev/uinput is not writable", "/dev/uinput 不可写") + (facts.uinput_error.empty() ? "" : " (" + facts.uinput_error + ")"),
          link(tr(lang, "Install the Sunshine udev rule and add the user to the input group, then log in again.", "安装 Sunshine 的 udev 规则并把用户加入 input 组,然后重新登录。"), "https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2getting__started.html")));
      }
    }

    // DS5: Windows forwards the controller as a USB device (usbip-win2); Linux creates it through uhid.
    if (gamepads_off) {
      items.push_back(make("input.ds5", group, status_e::skip, tr(lang, "DualSense driver path", "DualSense 驱动路径"), tr(lang, "Not checked: gamepads are off", "未检查:手柄已关闭")));
    }
    else if (facts.windows) {
      if (facts.usbip_tool_found && (!facts.usbip_driver_checked || facts.usbip_driver_found)) {
        items.push_back(make("input.ds5", group, status_e::pass, tr(lang, "DualSense driver path", "DualSense 驱动路径"),
          tr(lang, "usbip-win2 found", "找到 usbip-win2") + (facts.usbip_driver.empty() ? "" : " · " + facts.usbip_driver)));
      }
      else {
        items.push_back(make("input.ds5", group, status_e::warn, tr(lang, "DualSense driver path", "DualSense 驱动路径"),
          facts.usbip_tool_found ? tr(lang, "usbip.exe found, its driver is not installed", "找到 usbip.exe,但驱动没有安装") : tr(lang, "usbip-win2 not found; DualSense falls back to an emulated Xbox pad", "没有找到 usbip-win2;DualSense 会退回为模拟的 Xbox 手柄"),
          repair(tr(lang, "Install USB forwarding (in the control panel, or Components).", "安装 USB 转发(在控制面板里修复,或到「组件」)。"), "usbip", lang)));
      }
    }
    else if (facts.uhid_checked) {
      items.push_back(make("input.ds5", group, facts.uhid_ok ? status_e::pass : status_e::warn, tr(lang, "DualSense driver path", "DualSense 驱动路径"),
        facts.uhid_ok ? tr(lang, "/dev/uhid is writable", "/dev/uhid 可写") : tr(lang, "/dev/uhid is not writable; DualSense features (touchpad, motion, light bar) are lost", "/dev/uhid 不可写;DualSense 的触摸板、体感和灯条不可用"),
        facts.uhid_ok ? fix_t {} : link(tr(lang, "Install the Sunshine udev rule for uhid.", "安装 Sunshine 的 uhid udev 规则。"), "https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2getting__started.html")));
    }

    if (!facts.usbip_supported) {
      items.push_back(make("input.usb", group, status_e::skip, tr(lang, "USB forwarding", "USB 转发"),
        tr(lang, "Not checked: USB forwarding is Windows only in this build", "未检查:这个版本只在 Windows 上支持 USB 转发")));
    }
    else if (facts.usbip_tool_found && (!facts.usbip_driver_checked || facts.usbip_driver_found)) {
      items.push_back(make("input.usb", group, status_e::pass, tr(lang, "USB forwarding", "USB 转发"),
        tr(lang, "usbip-win2 ready", "usbip-win2 就绪") + (facts.usbip_driver_checked ? "" : tr(lang, " (driver service not checked)", "(驱动服务未检查)"))));
    }
    else {
      items.push_back(make("input.usb", group, status_e::warn, tr(lang, "USB forwarding", "USB 转发"),
        facts.usbip_tool_found ? tr(lang, "usbip.exe found, its driver is not installed", "找到 usbip.exe,但驱动没有安装") : tr(lang, "usbip-win2 is not installed", "没有安装 usbip-win2"),
        repair(tr(lang, "Install USB forwarding (in the control panel, or Components).", "安装 USB 转发(在控制面板里修复,或到「组件」)。"), "usbip", lang)));
    }
    return items;
  }

  // ---- 网络 ----

  std::vector<item_t>
  evaluate_network(const network_fact_t &facts, lang_e lang) {
    const std::string group = "network";
    std::vector<item_t> items;
    if (!facts.checked) {
      items.push_back(make("network.reachability", group, status_e::fail, tr(lang, "Network check", "网络检查"),
        tr(lang, "The reachability probe failed", "可达性检查失败") + (facts.error.empty() ? "" : ": " + facts.error),
        fix(tr(lang, "Run the check again.", "重新检查。"), "rerun", group)));
      return items;
    }

    std::vector<std::string> closed;
    std::vector<std::string> blocked;
    int on_demand = 0;
    for (const auto &port : facts.ports) {
      const auto label = port.proto + " " + std::to_string(port.port) + " (" + port.role + ")";
      if (!port.listening && !port.on_demand) closed.push_back(label);
      if (port.on_demand && !port.listening) ++on_demand;
      if (port.firewall == "blocked") blocked.push_back(label);
    }
    if (closed.empty()) {
      auto detail = std::to_string(facts.ports.size()) + tr(lang, " ports listening", " 个端口在监听");
      if (on_demand) detail += tr(lang, " (" + std::to_string(on_demand) + " open when a stream starts)", "(其中 " + std::to_string(on_demand) + " 个在开始串流时才打开)");
      items.push_back(make("network.ports", group, status_e::pass, tr(lang, "Ports", "端口"), detail));
    }
    else {
      items.push_back(make("network.ports", group, status_e::fail, tr(lang, "Ports", "端口"),
        tr(lang, "Not listening: ", "没有监听:") + join(closed, ", "),
        fix(tr(lang, "Another program may hold the port; change Port or close it, then restart Sunshine.", "端口可能被别的程序占用;改「端口」或关掉那个程序,然后重启 Sunshine。"), "restart", "sunshine", true)));
    }

    const bool firewall_known = std::ranges::any_of(facts.ports, [](const auto &p) { return p.firewall != "unknown"; });
    if (!blocked.empty()) {
      items.push_back(make("network.firewall", group, status_e::fail, tr(lang, "Firewall", "防火墙"),
        tr(lang, "Blocked: ", "被拦截:") + join(blocked, ", "),
        link(tr(lang, "Allow Sunshine in the firewall (the installer adds the rule; run it again to repair).", "在防火墙里放行 Sunshine(安装程序会加规则,重新运行它可修复)。"), "https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2troubleshooting.html")));
    }
    else if (firewall_known) {
      items.push_back(make("network.firewall", group, status_e::pass, tr(lang, "Firewall", "防火墙"), tr(lang, "Sunshine is allowed", "已放行 Sunshine")));
    }
    else {
      items.push_back(make("network.firewall", group, status_e::skip, tr(lang, "Firewall", "防火墙"),
        tr(lang, "Not checked: the firewall rules could not be read", "未检查:读不到防火墙规则")));
    }

    if (!facts.upnp_enabled) {
      items.push_back(make("network.upnp", group, status_e::skip, "UPnP", tr(lang, "Not checked: UPnP is off", "未检查:UPnP 未开启"),
        open_setting(tr(lang, "Turn UPnP on if the router should open the ports.", "需要路由器自动开端口时打开 UPnP。"), "upnp")));
    }
    else if (facts.router_protocol == "none" || facts.router_protocol.empty()) {
      items.push_back(make("network.upnp", group, status_e::warn, "UPnP", tr(lang, "UPnP is on but no router answered", "UPnP 已开启,但没有路由器应答"),
        fix(tr(lang, "Turn UPnP on in the router, or forward the ports by hand.", "在路由器里打开 UPnP,或手动转发端口。"))));
    }
    else if (facts.mappings_expected > 0 && facts.mappings_present < facts.mappings_expected) {
      items.push_back(make("network.upnp", group, status_e::warn, "UPnP",
        tr(lang, "Router " + facts.router_protocol + ": " + std::to_string(facts.mappings_present) + " of " + std::to_string(facts.mappings_expected) + " mappings present", "路由器 " + facts.router_protocol + ":" + std::to_string(facts.mappings_expected) + " 个映射里有 " + std::to_string(facts.mappings_present) + " 个"),
        fix(tr(lang, "Restart Sunshine to renew the mappings.", "重启 Sunshine 以重新建立映射。"), "restart", "sunshine", true)));
    }
    else {
      items.push_back(make("network.upnp", group, status_e::pass, "UPnP", tr(lang, "Router ", "路由器 ") + facts.router_protocol));
    }

    // 可达性: the same verdicts as the 网络 page, one line per path.
    const auto has_evidence = [&](std::string_view kind) { return std::ranges::find(facts.evidence_kinds, kind) != facts.evidence_kinds.end(); };
    const auto has_reason_prefix = [&](std::string_view prefix) {
      return std::ranges::any_of(facts.public_reasons, [&](const std::string &reason) { return reason.starts_with(prefix); });
    };
    // A missing public address is a setting to fill in, not a failure of the
    // public path. A closed port or a blocked firewall is still a failure of
    // its own, so it keeps the verdict below.
    const bool public_address_hint =
      facts.public_address_unset && !has_reason_prefix("not_listening") && !has_reason_prefix("firewall_blocked");
    const bool probed_v4 = has_evidence("external-probe-v4"), probed_v6 = has_evidence("external-probe-v6");
    if (probed_v4 || probed_v6) {
      // a test from outside reached this host (the phone or edge probe of audit #5): that is a fact, not an inference
      items.push_back(make("network.public", group, status_e::pass, tr(lang, "From the internet", "公网"),
        tr(lang, "Verified by a test from outside: ", "外网实测已验证:") + std::string { probed_v4 && probed_v6 ? "IPv4 + IPv6" : probed_v4 ? "IPv4" : "IPv6" }));
    }
    else if (facts.public_verdict == "verified" || facts.public_verdict == "likely") {
      items.push_back(make("network.public", group, status_e::pass, tr(lang, "From the internet", "公网"),
        facts.public_verdict == "verified" ? tr(lang, "Verified", "已验证") : tr(lang, "Likely reachable (inferred; not tested from outside)", "很可能可连(推断,没有从外网实测)")));
    }
    else if (public_address_hint) {
      items.push_back(make("network.public", group, status_e::skip, tr(lang, "From the internet", "公网"),
        tr(lang, "Not checked: no public address is set; clients use the address this host discovers.", "未检查:没有设置公网地址;客户端使用主机自己发现的地址。"),
        open_setting(tr(lang, "Set the public address clients should use.", "设置客户端应使用的公网地址。"), "external_endpoints")));
    }
    else if (facts.public_verdict == "unreachable") {
      std::string detail = facts.wan_ip_class == "cgnat" ? tr(lang, "Not reachable: the router's WAN address is carrier-grade NAT", "连不上:路由器的外网地址是运营商级 NAT") : tr(lang, "Not reachable", "连不上");
      items.push_back(make("network.public", group, status_e::warn, tr(lang, "From the internet", "公网"), detail,
        open_page(tr(lang, "Use a mesh network (Tailscale, ZeroTier…) or forward the ports.", "使用组网(Tailscale、ZeroTier…)或转发端口。"), "network")));
    }
    else {
      items.push_back(make("network.public", group, status_e::skip, tr(lang, "From the internet", "公网"),
        tr(lang, "Not checked: no client has connected from the internet and the router gave no answer", "未检查:没有客户端从公网连过,路由器也没有应答"),
        open_page(tr(lang, "See the paths on the Network page.", "到「网络」页看各条路径。"), "network")));
    }
    if (facts.mesh_verdict == "verified" || facts.mesh_verdict == "likely") {
      items.push_back(make("network.mesh", group, status_e::pass, tr(lang, "Mesh network", "组网"),
        facts.mesh_verdict == "verified" ? tr(lang, "Verified", "已验证") : tr(lang, "Likely reachable", "很可能可连")));
    }
    else if (facts.mesh_verdict == "unknown") {
      items.push_back(make("network.mesh", group, status_e::warn, tr(lang, "Mesh network", "组网"),
        tr(lang, "A mesh adapter exists but Sunshine is not reachable on it", "有组网网卡,但 Sunshine 在上面不可达"),
        open_page(tr(lang, "See the mesh path on the Network page.", "到「网络」页看组网路径。"), "network")));
    }
    else {
      items.push_back(make("network.mesh", group, status_e::skip, tr(lang, "Mesh network", "组网"), tr(lang, "Not checked: no mesh adapter", "未检查:没有组网网卡")));
    }
    items.push_back(make("network.lan", group, facts.lan_listening ? status_e::pass : status_e::fail, tr(lang, "Local network", "局域网"),
      !facts.lan_listening ? tr(lang, "Sunshine listens on no LAN address", "Sunshine 没有在任何局域网地址上监听") :
      facts.lan_address == "*"  ? tr(lang, "Listening on every address", "在所有地址上监听") :
                                  tr(lang, "Listening on " + facts.lan_address, "在 " + facts.lan_address + " 上监听"),
      facts.lan_listening ? fix_t {} : open_page(tr(lang, "Check the network adapter and Address family.", "检查网卡和「地址族」。"), "network")));

    // NAT in front of the router: the WAN address class of UPnP (RFC 6598 for 100.64/10)
    if (facts.wan_ip_class == "cgnat") {
      items.push_back(make("network.nat.cgnat", group, status_e::warn, tr(lang, "Carrier-grade NAT", "运营商级 NAT"),
        tr(lang, "The router's WAN address is in 100.64.0.0/10: port forwarding cannot reach this host from the internet", "路由器的外网地址在 100.64.0.0/10:端口转发没法让外网连到这台主机"),
        open_page(tr(lang, "Use a mesh network, IPv6, or ask the ISP for a public IPv4 address.", "使用组网、IPv6,或向运营商申请公网 IPv4。"), "network")));
    }
    else if (facts.wan_ip_class == "private") {
      items.push_back(make("network.nat.double", group, status_e::warn, tr(lang, "Double NAT", "双层 NAT"),
        tr(lang, "The router's WAN address is private: another router or modem sits in front of it", "路由器的外网地址是私网地址:前面还有一层路由器或光猫"),
        open_page(tr(lang, "Put the modem in bridge mode, or forward the ports on both routers.", "把光猫改为桥接,或在两层路由器上都转发端口。"), "network")));
    }
    else if (facts.wan_ip_class == "public") {
      items.push_back(make("network.nat.cgnat", group, status_e::pass, tr(lang, "NAT in front of the router", "路由器前的 NAT"), tr(lang, "None: the router has a public address", "没有:路由器拿到的是公网地址")));
    }
    else {
      items.push_back(make("network.nat.cgnat", group, status_e::skip, tr(lang, "NAT in front of the router", "路由器前的 NAT"),
        tr(lang, "Not checked: the router did not report its WAN address (UPnP off or not answering)", "未检查:路由器没有报告外网地址(UPnP 关闭或没有应答)")));
    }
    // mapping behaviour (RFC 4787 terms, RFC 5780 method) comes with the STUN work
    if (facts.nat_mapping == "endpoint_independent") {
      items.push_back(make("network.nat.mapping", group, status_e::pass, tr(lang, "NAT mapping", "NAT 映射"), tr(lang, "Independent of the destination: direct P2P is likely", "与目标无关:P2P 直连大概率可行")));
    }
    else if (facts.nat_mapping == "dependent") {
      items.push_back(make("network.nat.mapping", group, status_e::warn, tr(lang, "NAT mapping", "NAT 映射"), tr(lang, "Changes with the destination: a relay is likely needed", "随目标变化:大概率需要中继"),
        open_page(tr(lang, "Use a mesh network or forward the ports.", "使用组网或转发端口。"), "network")));
    }
    else {
      items.push_back(make("network.nat.mapping", group, status_e::skip, tr(lang, "NAT mapping", "NAT 映射"), tr(lang, "Not checked: the STUN probe lands with the network work", "未检查:STUN 探测等网络工作合入")));
    }
    if (!facts.ipv6_global.empty()) {
      items.push_back(make("network.v6.global", group, status_e::pass, tr(lang, "Global IPv6", "全局 IPv6"), facts.ipv6_global.front()));
    }
    else if (facts.public_verdict == "unreachable" || facts.wan_ip_class == "cgnat" || facts.wan_ip_class == "private") {
      items.push_back(make("network.v6.global", group, status_e::warn, tr(lang, "Global IPv6", "全局 IPv6"),
        tr(lang, "None, and IPv4 cannot be reached from the internet", "没有,而 IPv4 从外网连不上"),
        fix(tr(lang, "Turn IPv6 on in the router: it often reaches through carrier NAT.", "在路由器里打开 IPv6:它常常能绕过运营商 NAT。"))));
    }
    else {
      items.push_back(make("network.v6.global", group, status_e::pass, tr(lang, "Global IPv6", "全局 IPv6"), tr(lang, "None (IPv4 only)", "没有(只走 IPv4)")));
    }
    if (!facts.ipv6_global.empty() && !facts.ipv6_stability_known) {
      items.push_back(make("network.v6.stable", group, status_e::skip, tr(lang, "Stable IPv6 address", "稳定的 IPv6 地址"),
        tr(lang, "Not checked: telling temporary from stable addresses lands with the network work", "未检查:区分临时和稳定地址等网络工作合入")));
    }
    if (facts.pcp.empty()) {
      items.push_back(make("network.pcp", group, status_e::skip, "NAT-PMP / PCP", tr(lang, "Not checked: the gateway probe lands with the network work", "未检查:网关探测等网络工作合入")));
    }
    else {
      items.push_back(make("network.pcp", group, status_e::pass, "NAT-PMP / PCP", facts.pcp == "none" ? tr(lang, "The gateway answers neither", "网关两种都不支持") : facts.pcp));
    }
    return items;
  }

  // ---- 系统 ----

  std::vector<item_t>
  evaluate_system(const system_fact_t &facts, lang_e lang) {
    const std::string group = "system";
    std::vector<item_t> items;
    constexpr std::uint64_t GIB = 1024ull * 1024 * 1024;
    for (const auto &disk : facts.disks) {
      const auto id = "system.disk." + disk.role;
      const auto title = disk.role == "logs" ? tr(lang, "Free space for logs", "日志所在磁盘空间") : tr(lang, "Free space for configuration", "配置所在磁盘空间");
      if (!disk.checked) {
        items.push_back(make(id, group, status_e::skip, title, tr(lang, "Not checked: the drive could not be read", "未检查:读不到这个磁盘")));
        continue;
      }
      const auto detail = format_bytes(disk.available) + tr(lang, " free of ", " 可用 / 共 ") + format_bytes(disk.capacity) + " · " + disk.path;
      if (disk.available < GIB) {
        items.push_back(make(id, group, status_e::fail, title, detail, fix(tr(lang, "Free at least 1 GB; Sunshine stops writing logs and settings when the drive is full.", "至少腾出 1 GB;磁盘满了 Sunshine 无法写日志和设置。"))));
      }
      else if (disk.available < 5 * GIB) {
        items.push_back(make(id, group, status_e::warn, title, detail, fix(tr(lang, "Free some space (below 5 GB).", "空间低于 5 GB,建议清理。"))));
      }
      else {
        items.push_back(make(id, group, status_e::pass, title, detail));
      }
    }

    if (!facts.time_checked) {
      items.push_back(make("system.time", group, status_e::skip, tr(lang, "Time sync", "时间同步"), tr(lang, "Not checked: the time service could not be queried", "未检查:无法查询时间服务")));
    }
    else if (facts.time_synced) {
      items.push_back(make("system.time", group, status_e::pass, tr(lang, "Time sync", "时间同步"), facts.time_source + (facts.time_detail.empty() ? "" : " · " + facts.time_detail)));
    }
    else {
      items.push_back(make("system.time", group, status_e::warn, tr(lang, "Time sync", "时间同步"),
        tr(lang, "The clock is not synchronised", "时钟没有同步") + (facts.time_detail.empty() ? "" : " · " + facts.time_detail),
        fix(facts.windows ? tr(lang, "Turn on Set time automatically in Windows Settings › Time.", "在 Windows 设置 › 时间里打开「自动设置时间」。") : tr(lang, "Enable NTP (timedatectl set-ntp true).", "启用 NTP(timedatectl set-ntp true)。"))));
    }

    if (!facts.account_checked) {
      items.push_back(make("system.account", group, status_e::skip, tr(lang, "Service account", "运行账户"), tr(lang, "Not checked", "未检查")));
    }
    else {
      const bool ok = facts.windows ? facts.account == "system" : facts.account != "root";
      std::string detail;
      if (facts.account == "system") detail = tr(lang, "LocalSystem (service)", "LocalSystem(服务)");
      else if (facts.account == "admin") detail = tr(lang, "An administrator, not the service", "管理员,不是服务");
      else if (facts.account == "root") detail = "root";
      else detail = tr(lang, "A standard user", "普通用户");
      if (!facts.config_writable) {
        items.push_back(make("system.account", group, status_e::fail, tr(lang, "Service account", "运行账户"),
          detail + tr(lang, "; the configuration folder is not writable", ";配置文件夹不可写"),
          fix(tr(lang, "Give the account write access to the configuration folder.", "给这个账户配置文件夹的写权限。"))));
      }
      else if (ok) {
        items.push_back(make("system.account", group, status_e::pass, tr(lang, "Service account", "运行账户"), detail));
      }
      else {
        items.push_back(make("system.account", group, status_e::warn, tr(lang, "Service account", "运行账户"),
          detail + (facts.windows ? tr(lang, "; the lock screen and UAC prompts cannot be captured", ";无法采集锁屏和 UAC 弹窗") : tr(lang, "; running as root is not needed", ";不需要以 root 运行")),
          fix(facts.windows ? tr(lang, "Start Sunshine through the SunshineService service.", "通过 SunshineService 服务启动 Sunshine。") : tr(lang, "Run Sunshine as your desktop user.", "以桌面用户运行 Sunshine。"))));
      }
    }

    if (!facts.reboot_checked) {
      items.push_back(make("system.reboot", group, status_e::skip, tr(lang, "Pending restart", "待重启"), tr(lang, "Not checked: the system does not report it", "未检查:系统不报告")));
    }
    else if (facts.reboot_reasons.empty()) {
      items.push_back(make("system.reboot", group, status_e::pass, tr(lang, "Pending restart", "待重启"), tr(lang, "None", "没有")));
    }
    else {
      items.push_back(make("system.reboot", group, status_e::warn, tr(lang, "Pending restart", "待重启"), join(facts.reboot_reasons, ", "),
        fix(tr(lang, "Restart the computer; drivers installed before it may not work yet.", "重启电脑;之前装的驱动可能还没生效。"))));
    }

    const auto os = facts.os_name + (facts.os_version.empty() ? "" : " " + facts.os_version);
    if (facts.windows) {
      // Windows 10 22H2 (build 19045) is the oldest release Sunshine supports.
      if (!facts.os_build) {
        items.push_back(make("system.os", group, status_e::skip, tr(lang, "Operating system", "操作系统"), os + tr(lang, " · build not read", " · 读不到版本号")));
      }
      else if (*facts.os_build < 19045) {
        items.push_back(make("system.os", group, status_e::fail, tr(lang, "Operating system", "操作系统"),
          os + tr(lang, " · build ", " · 版本号 ") + std::to_string(*facts.os_build) + tr(lang, "; Windows 10 22H2 (19045) or newer is required", ";需要 Windows 10 22H2(19045)或更新"),
          fix(tr(lang, "Update Windows.", "更新 Windows。"))));
      }
      else {
        items.push_back(make("system.os", group, status_e::pass, tr(lang, "Operating system", "操作系统"), os + tr(lang, " · build ", " · 版本号 ") + std::to_string(*facts.os_build)));
      }
    }
    else {
      items.push_back(make("system.os", group, status_e::pass, tr(lang, "Operating system", "操作系统"), os.empty() ? tr(lang, "Unknown", "未知") : os));
    }
    return items;
  }

  // ---- 主机状态 ----

  std::string_view
  channel_of(std::string_view version, std::string_view branch) {
    // ".杂鱼" ends every CI release (<tag>.杂鱼): it says nothing about the channel or a dirty tree, only the build record (dirty) does.
    if (!branch.empty()) return branch == "master" || branch == "main" ? "stable" : "dev";
    const auto l = lower(version);
    if (l.find("beta") != std::string::npos || l.find("rc") != std::string::npos || l.find("alpha") != std::string::npos) return "beta";
    // A build off another branch than master appends the commit: "2026.1104.121813.96a1e2b6".
    int dots = 0;
    std::size_t last = 0;
    for (std::size_t i = 0; i < version.size(); ++i) {
      if (version[i] == '.') {
        ++dots;
        last = i + 1;
      }
    }
    if (dots >= 3) {
      const auto tail = version.substr(last);
      const bool hex = !tail.empty() && std::ranges::all_of(tail, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
      const bool letters = std::ranges::any_of(tail, [](char c) { return std::isalpha(static_cast<unsigned char>(c)); });
      if (hex && letters) return "dev";
    }
    return "stable";
  }

  std::vector<item_t>
  evaluate_host(const host_fact_t &facts, lang_e lang) {
    const std::string group = "host";
    std::vector<item_t> items;
    if (facts.build) {
      // the build says what it is (audit #1): commit, channel, time, variant
      const auto &b = *facts.build;
      std::vector<std::string> parts { b.version.empty() ? facts.version : b.version };
      for (const auto *part : { &b.channel, &b.variant, &b.package, &b.build_time_utc }) {
        if (!part->empty()) parts.push_back(*part);
      }
      if (!b.commit.empty()) parts.push_back(b.commit.substr(0, 10));
      items.push_back(make("host.version", group, b.dirty ? status_e::warn : status_e::pass, tr(lang, "Sunshine version", "Sunshine 版本"),
        join(parts, " · ") + (b.dirty ? tr(lang, " · uncommitted changes", " · 含未提交的改动") : ""),
        b.dirty ? open_page(tr(lang, "Install a release build before reporting problems.", "报告问题前请装正式版。"), "update") : fix_t {}));
    }
    else {
      const auto channel = channel_of(facts.version, facts.branch);
      std::string channel_text;
      if (channel == "dev") channel_text = tr(lang, "development build", "开发构建");
      else if (channel == "beta") channel_text = tr(lang, "beta", "测试版");
      else channel_text = tr(lang, "release or local build", "正式版或本地构建");
      // Without the build record nothing tells a dirty tree: the version string never does (".杂鱼" is on every CI release).
      items.push_back(make("host.version", group, status_e::pass, tr(lang, "Sunshine version", "Sunshine 版本"),
        facts.version + " · " + channel_text + tr(lang, " (read from the version string)", "(由版本号推断)")));
    }
    items.push_back(make("host.uptime", group, status_e::pass, tr(lang, "Running for", "已运行"), format_duration(facts.uptime_s, lang)));

    if (!facts.crashes_checked) {
      items.push_back(make("host.crashes", group, status_e::skip, tr(lang, "Recent crashes", "最近的崩溃"),
        tr(lang, "Not checked: this system keeps no crash records the host can read", "未检查:系统没有主机可读的崩溃记录")));
    }
    else if (facts.crashes.empty()) {
      items.push_back(make("host.crashes", group, status_e::pass, tr(lang, "Recent crashes", "最近的崩溃"),
        tr(lang, "None in 7 days", "7 天内没有") + (facts.crash_source.empty() ? "" : " · " + facts.crash_source)));
    }
    else {
      items.push_back(make("host.crashes", group, status_e::warn, tr(lang, "Recent crashes", "最近的崩溃"),
        std::to_string(facts.crashes.size()) + tr(lang, " in 7 days, last ", " 次(7 天内),最近一次 ") + facts.crashes.front(),
        open_page(tr(lang, "Export the diagnostics bundle with the crash and report it.", "导出带崩溃记录的诊断包并反馈。"), "logs")));
    }

    if (!facts.config_problems.empty()) {
      std::vector<std::string> shown(facts.config_problems.begin(), facts.config_problems.begin() + std::min<std::size_t>(3, facts.config_problems.size()));
      auto detail = join(shown, "; ");
      if (facts.config_problems.size() > shown.size()) detail += tr(lang, "; +" + std::to_string(facts.config_problems.size() - shown.size()) + " more", ";另有 " + std::to_string(facts.config_problems.size() - shown.size()) + " 条");
      items.push_back(make("host.config", group, status_e::warn, tr(lang, "Configuration at start", "启动时的配置"), detail,
        open_page(tr(lang, "Correct or remove these entries in the configuration file.", "在配置文件里改正或删除这些项。"), "config")));
    }
    else {
      items.push_back(make("host.config", group, status_e::pass, tr(lang, "Configuration at start", "启动时的配置"), tr(lang, "No unknown or invalid entries", "没有未知或无效的项")));
    }

    if (!facts.log_checked) {
      items.push_back(make("host.log", group, status_e::skip, tr(lang, "Fatal errors in the log", "日志里的致命错误"), tr(lang, "Not checked: the log file could not be read", "未检查:读不到日志文件")));
    }
    else if (facts.fatal_lines > 0) {
      items.push_back(make("host.log", group, status_e::warn, tr(lang, "Fatal errors in the log", "日志里的致命错误"),
        std::to_string(facts.fatal_lines) + tr(lang, " Fatal lines since start", " 条 Fatal(自本次启动)"),
        open_page(tr(lang, "Open Logs and diagnostics to see them.", "到「日志与诊断」查看。"), "logs")));
    }
    else {
      items.push_back(make("host.log", group, status_e::pass, tr(lang, "Fatal errors in the log", "日志里的致命错误"), tr(lang, "None since start", "本次启动以来没有")));
    }
    return items;
  }

  std::optional<build_info_t>
  current_build_info() {
#ifdef DOCTOR_HAS_BUILD_INFO
    const auto b = ::build_info::current();
    if (b.commit.empty()) return std::nullopt;  // the build had no commit: read the version string instead
    return build_info_t { b.version, b.commit, b.branch, b.dirty, b.channel, b.build_time_utc, b.variant, b.package, b.builder };
#else
    // until feat/build-identity is merged the version string is read
    return std::nullopt;
#endif
  }

  // ---- config problems ----

  namespace {
    std::mutex &
    problems_mutex() {
      static std::mutex mutex;
      return mutex;
    }

    std::vector<std::string> &
    problems_store() {
      static std::vector<std::string> store;
      return store;
    }
  }  // namespace

  void
  record_config_problem(std::string text) {
    std::lock_guard lock { problems_mutex() };
    auto &store = problems_store();
    if (store.size() < 64 && std::ranges::find(store, text) == store.end()) store.push_back(std::move(text));
  }

  std::vector<std::string>
  config_problems() {
    std::lock_guard lock { problems_mutex() };
    return problems_store();
  }
}  // namespace doctor

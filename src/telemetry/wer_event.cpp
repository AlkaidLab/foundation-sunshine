/**
 * @file src/telemetry/wer_event.cpp
 * @brief Parsing of the driver's WER fault events; the Windows reader is in wer_event_win.cpp.
 */
#include "wer_event.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <vector>

namespace telemetry {

  namespace {
    std::string
    lower(std::string text) {
      std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return text;
    }

    /** The text of `<tag ...>text</tag>` after `from`, or nullopt. */
    std::optional<std::string>
    element_text(std::string_view xml, std::string_view tag, std::size_t from = 0) {
      const auto open = xml.find("<" + std::string { tag }, from);
      if (open == std::string_view::npos) return std::nullopt;
      const auto start = xml.find('>', open);
      if (start == std::string_view::npos) return std::nullopt;
      const auto end = xml.find("</" + std::string { tag } + ">", start);
      if (end == std::string_view::npos) return std::nullopt;
      return std::string { xml.substr(start + 1, end - start - 1) };
    }

    /** The values of every <Data> element of <EventData>, in order. */
    std::vector<std::string>
    event_data(std::string_view xml) {
      std::vector<std::string> out;
      const auto begin = xml.find("<EventData");
      if (begin == std::string_view::npos) return out;
      std::size_t at = begin;
      while ((at = xml.find("<Data", at)) != std::string_view::npos) {
        const auto close = xml.find('>', at);
        if (close == std::string_view::npos) break;
        if (xml[close - 1] == '/') {  // <Data/> is an empty value
          out.emplace_back();
          at = close;
          continue;
        }
        const auto end = xml.find("</Data>", close);
        if (end == std::string_view::npos) break;
        out.emplace_back(xml.substr(close + 1, end - close - 1));
        at = end;
      }
      return out;
    }

    /** "2026-11-15T13:20:11.123456700Z" -> UTC milliseconds since 1970, or 0. */
    std::int64_t
    parse_system_time(const std::string &text) {
      static const std::regex shape(R"(^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,9}))?Z$)");
      std::smatch m;
      if (!std::regex_match(text, m, shape)) return 0;
      const int y = std::stoi(m[1]), mo = std::stoi(m[2]), d = std::stoi(m[3]);
      // days from civil (Howard Hinnant's algorithm)
      const int yy = y - (mo <= 2);
      const int era = (yy >= 0 ? yy : yy - 399) / 400;
      const unsigned yoe = static_cast<unsigned>(yy - era * 400);
      const unsigned doy = (153 * static_cast<unsigned>(mo + (mo > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
      const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
      const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
      std::int64_t ms = ((days * 24 + std::stoi(m[4])) * 60 + std::stoi(m[5])) * 60 + std::stoi(m[6]);
      ms *= 1000;
      if (m[7].matched) {
        auto frac = m[7].str();
        frac.resize(3, '0');
        ms += std::stoi(frac);
      }
      return ms;
    }
  }  // namespace

  std::optional<wer_fault_t>
  parse_wer_event(std::string_view xml) {
    if (xml.size() > 16 * 1024) return std::nullopt;
    const auto id = element_text(xml, "EventID");
    if (!id || *id != "1000") return std::nullopt;
    const auto data = event_data(xml);
    if (data.size() < 8) return std::nullopt;
    wer_fault_t fault;
    fault.event_id = 1000;
    fault.module = lower(data[3]);
    if (fault.module != VDD_MODULE) return std::nullopt;  // anything else is not ours and is not copied
    static const std::regex code(R"(^(?:0x)?([0-9a-fA-F]{8})$)");
    static const std::regex offset(R"(^(?:0x)?0*([0-9a-fA-F]{1,16})$)");
    std::smatch m;
    if (!std::regex_match(data[6], m, code)) return std::nullopt;
    fault.exception_code = lower(m[1]);
    if (!std::regex_match(data[7], m, offset)) return std::nullopt;
    fault.offset = "0x" + lower(m[1]);
    const auto tc = xml.find("<TimeCreated");
    if (tc != std::string_view::npos) {
      // EvtRender writes attributes in single quotes; a double-quoted copy of the same XML is read too
      const auto at = xml.find("SystemTime=", tc);
      if (at != std::string_view::npos && at + 11 < xml.size() && (xml[at + 11] == '"' || xml[at + 11] == '\'')) {
        const auto from = at + 12;
        const auto end = xml.find(xml[at + 11], from);
        if (end != std::string_view::npos) fault.time_ms = parse_system_time(std::string { xml.substr(from, end - from) });
      }
    }
    return fault;
  }

  nlohmann::json
  wer_props(const wer_fault_t &fault) {
    return nlohmann::json {
      { "event_id", std::to_string(fault.event_id) },
      { "kind", "crash" },
      { "module", fault.module },
      { "exception_code", fault.exception_code },
      { "offset", fault.offset },
    };
  }

#ifndef _WIN32
  std::vector<wer_fault_t>
  read_vdd_faults(std::int64_t, std::size_t) {
    return {};
  }
#endif

}  // namespace telemetry

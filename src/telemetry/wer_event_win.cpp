/**
 * @file src/telemetry/wer_event_win.cpp
 * @brief Windows: read the virtual display driver's faults from the Application log (Windows Event Log API, wevtapi).
 *
 * EvtQuery with an XPath filter on the provider "Application Error", event 1000 and the event time, EvtNext for the events, EvtRender (EvtRenderEventXml) for each; the parser keeps only the driver's own.
 * Reading the Application log needs no special right for an ordinary event log reader. A failure at any step returns what was read so far.
 */
#ifdef _WIN32

  #include "wer_event.h"

  #include <windows.h>
  #include <winevt.h>

  #include <chrono>
  #include <string>
  #include <vector>

namespace telemetry {

  std::vector<wer_fault_t>
  read_vdd_faults(std::int64_t since_ms, std::size_t limit) {
    std::vector<wer_fault_t> out;
    // timediff(@SystemTime) is the age in milliseconds; a cursor older than 30 days is a day of 30 at most
    const std::int64_t now_ms = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    std::int64_t age = now_ms - since_ms;
    if (since_ms <= 0 || age > 30LL * 24 * 3600 * 1000) age = 30LL * 24 * 3600 * 1000;
    if (age < 0) age = 0;
    const std::wstring query = L"*[System[Provider[@Name='Application Error'] and (EventID=1000) and TimeCreated[timediff(@SystemTime) <= " + std::to_wstring(age) + L"]]]";
    EVT_HANDLE results = EvtQuery(nullptr, L"Application", query.c_str(), EvtQueryChannelPath | EvtQueryForwardDirection);
    if (!results) return out;
    EVT_HANDLE events[16] {};
    DWORD returned = 0;
    while (out.size() < limit && EvtNext(results, 16, events, 2000, 0, &returned)) {
      for (DWORD i = 0; i < returned; ++i) {
        DWORD used = 0, props = 0;
        EvtRender(nullptr, events[i], EvtRenderEventXml, 0, nullptr, &used, &props);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && used > 0 && used < 64 * 1024) {
          std::vector<wchar_t> buffer(used / sizeof(wchar_t) + 1);
          if (EvtRender(nullptr, events[i], EvtRenderEventXml, used, buffer.data(), &used, &props)) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 1 && bytes < 64 * 1024) {
              std::string xml(static_cast<std::size_t>(bytes - 1), '\0');
              WideCharToMultiByte(CP_UTF8, 0, buffer.data(), -1, xml.data(), bytes, nullptr, nullptr);
              if (auto fault = parse_wer_event(xml); fault && fault->time_ms > since_ms && out.size() < limit) out.push_back(*fault);
            }
          }
        }
        EvtClose(events[i]);
      }
    }
    EvtClose(results);
    return out;
  }

}  // namespace telemetry

#endif

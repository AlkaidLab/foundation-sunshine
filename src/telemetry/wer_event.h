/**
 * @file src/telemetry/wer_event.h
 * @brief A Windows Error Reporting fault of the virtual display driver, as the fixed facts the plan promises: it happened, when, which module, the exception code and the offset. Pure code (std + nlohmann).
 *
 * The driver runs in a user-mode driver host (UMDF, WUDFHost.exe). When it faults, Windows writes event 1000 (Application Error) or 1002 (Application Hang) to the Application log with the faulting
 * module, the exception code and the fault offset among the event data (Microsoft Learn, "Windows Error Reporting" and "Application Error" events). There is no stack of the driver and none is asked for.
 * Only a fault whose module is the driver's own file is taken; the event data of everything else is never copied.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace telemetry {

  struct wer_fault_t {
    int event_id = 0;  ///< 1000 (crash), 1002 (hang)
    std::string module;  ///< the faulting module's file name, lower case
    std::string exception_code;  ///< 8 hex digits, lower case ("c0000005")
    std::string offset;  ///< "0x" and lower-case hex ("0x1a2b3c")
    std::int64_t time_ms = 0;  ///< UTC milliseconds since 1970 of the event
  };

  /** The file name of the virtual display driver (cmake/packaging: ZakoVDD.dll). Matched ignoring case, whole name. */
  inline constexpr std::string_view VDD_MODULE = "zakovdd.dll";

  /**
   * The Application log event XML (EvtRender, EvtRenderEventXml) of an event 1000 or 1002 whose faulting module is the driver -> its facts; nullopt for any other event, module or shape.
   * Data positions as Windows writes them: Application Error: Data[3] module, Data[6] exception code, Data[7] fault offset. Application Hang has no module of its own and is not taken.
   */
  std::optional<wer_fault_t>
  parse_wer_event(std::string_view event_xml);

  /** The props of a `wer_event` event: {event_id, kind, module, exception_code, offset}. */
  nlohmann::json
  wer_props(const wer_fault_t &fault);

  /** Windows only: the driver faults in the Application log newer than `since_ms` (UTC ms), oldest first, at most `limit`. Empty elsewhere or when the log cannot be read. */
  std::vector<wer_fault_t>
  read_vdd_faults(std::int64_t since_ms, std::size_t limit = 5);

}  // namespace telemetry

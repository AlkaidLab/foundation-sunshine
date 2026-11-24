/**
 * @file src/log_text.h
 * @brief How a name that a person chose (a client, a window title, a command) is written into a log line.
 *
 * The log excerpt that follows a serious stream problem drops what stands between double quotes (scrub_log_message in telemetry/events.cpp). A name written between double quotes is dropped whole, whatever it
 * contains, so every log line that writes such a name uses quoted_name(). A double quote inside the name becomes a single quote, so it cannot end the span early; line breaks and other control characters become spaces.
 */
#pragma once

#include <string>
#include <string_view>

namespace logging {

  inline std::string
  quoted_name(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char c : value) {
      if (c == '"') {
        out.push_back('\'');
      }
      else if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
        out.push_back(' ');
      }
      else {
        out.push_back(c);
      }
    }
    out.push_back('"');
    return out;
  }

}  // namespace logging

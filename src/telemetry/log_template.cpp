#include "log_template.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <tuple>

namespace telemetry {
  namespace {
    struct entry_t {
      std::string_view file;
      int line;
      std::string_view message;
    };

    constexpr entry_t entries[] {
#include "error_templates.generated.inc"
    };
  }  // namespace

  std::string_view
  log_template_at(std::string_view file, int line) {
    // CMake and the catalogue use '/'. MSVC may supply a source path with '\'.
    std::string normalized;
    if (file.find('\\') != std::string_view::npos) {
      normalized = file;
      std::ranges::replace(normalized, '\\', '/');
      file = normalized;
    }
    for (;;) {
      const auto key = std::tie(file, line);
      const auto it = std::lower_bound(std::begin(entries), std::end(entries), key, [](const entry_t &entry, const auto &target) {
        return std::tie(entry.file, entry.line) < target;
      });
      if (it != std::end(entries) && std::tie(it->file, it->line) == key) return it->message;
      // Source workspaces can themselves be nested under a directory named src.
      const auto next_root = file.find("/src/");
      if (next_root == std::string_view::npos) return {};
      file.remove_prefix(next_root + 1);
    }
  }
  // which part of the host a log line comes from: only a known word at the start of the line names a component
  std::string
  log_component_of(std::string_view message) {
    static constexpr std::string_view known[] {
      "config",
      "nvhttp",
      "rtsp",
      "stream",
      "video",
      "audio",
      "input",
      "capture",
      "display",
      "encoder",
      "nvenc",
      "webhook",
      "vdd",
      "sparxie",
      "upnp",
      "network",
      "clipboard",
      "gamepad",
      "tray",
      "process",
      "doctor",
      "pair",
      "nat",
      "mic",
    };
    std::string head;
    for (std::size_t i = 0; i < message.size() && i < 24; ++i) {
      const auto c = static_cast<unsigned char>(message[i]);
      if (!std::isalnum(c)) break;
      head.push_back(static_cast<char>(std::tolower(c)));
    }
    for (const auto &k : known)
      if (head == k) return head;
    return "host";
  }
}  // namespace telemetry

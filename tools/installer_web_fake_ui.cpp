// Test double of the setup UI process (installer-ui/ in the Panel repo) for tools/installer_web.inc: speaks the same
// pipe protocol without a window, so the web host can be exercised where WebView2 cannot run (session 0). Never
// packaged. Env: FAKE_UI_LOG=<file> (every line received), FAKE_UI_DRIVE=a,b,toggle-component:usbip (one action per
// view received, as the page would send them).
//   g++ -std=c++23 -O2 -municode -static installer_web_fake_ui.cpp -o foundation-setup-ui.exe
#ifndef UNICODE
  #define UNICODE
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void
put(const std::string &line) {
  const std::string data = line + "\n";
  DWORD written = 0;
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
}

int
wmain() {
  const char *log_path = std::getenv("FAKE_UI_LOG");
  FILE *log = log_path ? std::fopen(log_path, "ab") : nullptr;
  std::vector<std::string> drive;
  if (const char *d = std::getenv("FAKE_UI_DRIVE")) {
    std::string all = d;
    size_t pos = 0;
    while (pos <= all.size()) {
      size_t e = all.find(',', pos);
      if (e == std::string::npos) e = all.size();
      if (e > pos) drive.push_back(all.substr(pos, e - pos));
      pos = e + 1;
    }
  }
  put("{\"hwnd\":0,\"type\":\"hello\"}");  // serde_json key order, as the real UI
  put("{\"type\":\"loaded\"}");
  put("{\"action\":{\"type\":\"ready\"},\"type\":\"action\"}");
  size_t next = 0;
  std::string pending;
  char buffer[4096];
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &read, nullptr) || read == 0) break;
    pending.append(buffer, read);
    size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
      const std::string line = pending.substr(0, nl);
      pending.erase(0, nl + 1);
      if (log) {
        std::fprintf(log, "%s\n", line.c_str());
        std::fflush(log);
      }
      if (line.find("\"type\":\"view\"") == std::string::npos || next >= drive.size()) continue;
      Sleep(300);
      const std::string a = drive[next++];
      const size_t colon = a.find(':');
      // the real UI's key order (serde_json, alphabetical): the nested "type" comes before the top-level one
      put(colon == std::string::npos ? "{\"action\":{\"type\":\"" + a + "\"},\"type\":\"action\"}" :
                                       "{\"action\":{\"id\":\"" + a.substr(colon + 1) + "\",\"type\":\"" + a.substr(0, colon) + "\"},\"type\":\"action\"}");
    }
  }
  if (log) std::fprintf(log, "EOF\n"), std::fclose(log);
  return 0;
}

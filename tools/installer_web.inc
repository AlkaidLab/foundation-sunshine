// ================================================================ web front end host
//
// The pages are a Vue app (foundation-ui src/vue/installer, the Panel's m3e components) in a Tauri 2 WebView2
// window: the setup UI process (foundation-setup-ui.exe, built from the Panel repo, installer-ui/). It is appended
// to this exe as a second payload (append_installer_payload.py --ui). This process stays the installer: it owns
// the view, starts the elevated worker, reads its events and acts on the person's choices with the same functions
// as the native window (click(), begin_install(), poll_run(), finish_run()). The UI process only draws and reports
// actions.
//
// Wire (one JSON object per line, UTF-8):
//   host -> UI (its stdin):  {"type":"view","view":InstallerView} | {"type":"minimise"} | {"type":"focus"}
//                            end of stream = close the window and exit
//   UI -> host (its stdout): {"type":"hello","hwnd":N} | {"type":"loaded"} | {"type":"action","action":{...}}
// InstallerView and the actions are specified in foundation-ui src/vue/installer/bridge.js.
//
// Fallback to the native window (shell_main): no WebView2 Runtime (installer-ms-webview2-distribution-20261109),
// no UI payload, the UI process cannot start, or it has not loaded its page within kWebLoadTimeoutMs.

constexpr UINT WM_WEB_LINE = WM_APP + 41;  // lParam: std::string * (one line from the UI)
constexpr UINT WM_WEB_GONE = WM_APP + 42;  // the UI closed its stdout (it exited)
constexpr UINT_PTR kWebWatchdog = 9, kWebTick = 10;
constexpr DWORD kWebLoadTimeoutMs = 15000;
constexpr int kWebFallback = -77;

struct web_state {
  HANDLE child = nullptr;   // the UI process (started by us: the only process this code ever terminates)
  DWORD child_pid = 0;
  HANDLE to_child = nullptr;  // write end of its stdin
  HANDLE from_child = nullptr;  // read end of its stdout
  HANDLE reader = nullptr;
  std::wstring dir;  // extraction directory of the UI exe and its WebView2 user data
  bool loaded = false;
  bool quitting = false;
  int exit_code = 0;
  std::string last_sent;
  int seq = 0;
} W;

// %TEMP%\FoundationSunshine-setup-web.log: why the web front end was or was not used, and what the page asked for.
// The person can send it with a report; nothing secret is in it (pages and action names only).
void
web_log(const std::string &text) {
  wchar_t temp_dir[MAX_PATH + 2] {};
  GetTempPathW(MAX_PATH, temp_dir);
  SYSTEMTIME t {};
  GetLocalTime(&t);
  char stamp[48];
  snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u pid %lu ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
  append_event(std::wstring(temp_dir) + L"FoundationSunshine-setup-web.log", std::string(stamp) + text + "\n");
}

std::string
utf8(const std::wstring &w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::string
json_text(const std::string &s) {
  std::string out = "\"";
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += ch;
    }
    else if (c < 0x20) {
      char buffer[8];
      snprintf(buffer, sizeof(buffer), "\\u%04x", c);
      out += buffer;
    }
    else
      out += ch;
  }
  return out + "\"";
}

const char *
page_name(pg p) {
  switch (p) {
    case pg::welcome: return "welcome";
    case pg::maintenance: return "maintenance";
    case pg::components: return "components";
    case pg::uac: return "uac";
    case pg::installing: return "installing";
    case pg::failed: return "failed";
    case pg::done: return "done";
  }
  return "welcome";
}

const char *
step_state_name(sstate s) {
  switch (s) {
    case sstate::pending: return "pending";
    case sstate::running: return "running";
    case sstate::done: return "done";
    case sstate::failed: return "failed";
    case sstate::reboot: return "reboot";
  }
  return "pending";
}

const char *
step_name(int d) {
  static const char *names[d_count] = { "", "worker", "engine", "wizard", "prepare", "files", "path", "config", "vdd", "gamepad",
    "usbip", "vmouse", "firewall", "service", "autostart", "permissions", "reboot" };
  return d > 0 && d < d_count ? names[d] : "";
}

constexpr const char *kComponentIds[kTileCount] = { "vdd", "gamepad", "usbip", "vmouse", "firewall", "autostart", "tools", "desktopicon" };

// The InstallerView of bridge.js, without seq (added when sent).
std::string
view_json() {
  const view &v = S.v;
  const auto b = [](bool x) { return x ? "true" : "false"; };
  std::string j = "{\"schema\":1";
  j += ",\"page\":\"" + std::string(page_name(v.page)) + "\"";
  j += std::string(",\"locale\":\"") + (v.zh ? "zh" : "en") + "\"";
  j += std::string(",\"upgrade\":") + b(v.upgrade || v.maintenance);
  if (v.maintenance) {
    j += ",\"maintenance\":{\"installedVersion\":" + json_text(v.installed_version == L"未知" ? std::string("unknown") : utf8(v.installed_version)) +
         ",\"packageVersion\":" + json_text(utf8(v.package_version)) + ",\"downgrade\":" + b(v.maintenance_downgrade) +
         ",\"same\":" + b(v.maintenance_same) + ",\"hasUninstaller\":" + b(v.has_uninstaller) + "}";
  }
  else
    j += ",\"maintenance\":null";
  bool all = true;
  j += ",\"components\":{";
  for (int i = 0; i < kTileCount; ++i) {
    all = all && v.tiles[i];
    j += std::string(i ? "," : "") + "\"" + kComponentIds[i] + "\":" + b(v.tiles[i]);
  }
  j += std::string("},\"allSelected\":") + b(all);
  // components this Windows cannot run: the Windows 10 version each needs (the page disables the row and says so)
  j += ",\"unsupported\":{";
  {
    bool first_limit = true;
    for (int i = 0; i < kTileCount; ++i) {
      if (component_min_build(i) <= windows_build()) continue;
      j += std::string(first_limit ? "" : ",") + "\"" + kComponentIds[i] + "\":\"" + component_min_name(i) + "\"";
      first_limit = false;
    }
  }
  j += "}";
  j += std::string(",\"windowsBuild\":") + std::to_string(windows_build());
  j += std::string(",\"uacCancelled\":") + b(v.uac_cancelled);
  j += ",\"progress\":{\"groups\":[";
  static const char *groups[kGroups] = { "prepare", "files", "drivers", "system" };
  for (int g = 0; g < kGroups; ++g)
    j += std::string(g ? "," : "") + "{\"id\":\"" + groups[g] + "\",\"state\":\"" + step_state_name(group_state(v, g)) + "\"}";
  j += "],\"current\":" + std::to_string(current_group(v)) + ",\"steps\":{";
  bool first = true;
  for (int d = 1; d < d_count; ++d) {
    if (v.subs[d].group < 0) continue;
    j += std::string(first ? "" : ",") + "\"" + step_name(d) + "\":\"" + step_state_name(v.subs[d].st) + "\"";
    first = false;
  }
  j += "},\"planned\":[";
  {
    // flow order of bridge.js PROGRESS_GROUPS
    static const int order[] = { d_worker, d_engine, d_wizard, d_prepare, d_files, d_path, d_config, d_permissions, d_vdd,
      d_gamepad, d_usbip, d_vmouse, d_firewall, d_service, d_autostart, d_reboot };
    bool planned[d_count] = {};
    if (!v.planned.empty()) {
      const std::string list = "," + v.planned + ",";
      for (const int d : order) planned[d] = list.find(std::string(",") + step_name(d) + ",") != std::string::npos;
    }
    else {
      // before the engine's plan: the estimate from the selection (the engine's own selection may differ when every
      // tile is on: it then keeps its defaults)
      for (const int d : { d_worker, d_engine, d_wizard, d_prepare, d_files, d_path, d_config, d_service }) planned[d] = true;
      const int drivers[] = { d_vdd, d_gamepad, d_usbip, d_vmouse };
      for (int i = 0; i < 4; ++i) planned[drivers[i]] = v.tiles[i];
      planned[d_firewall] = v.tiles[4];
      planned[d_autostart] = v.tiles[5];
    }
    bool first_planned = true;
    for (const int d : order) {
      if (!planned[d] && v.subs[d].group < 0) continue;  // a step that reported is added
      j += std::string(first_planned ? "" : ",") + "\"" + step_name(d) + "\"";
      first_planned = false;
    }
  }
  j += "],\"fraction\":{";
  {
    bool first_fraction = true;
    for (int d = 1; d < d_count; ++d) {
      if (v.permille[d] < 0 || v.subs[d].st != sstate::running) continue;
      char number[16];
      snprintf(number, sizeof(number), "%.3f", v.permille[d] / 1000.0);
      j += std::string(first_fraction ? "" : ",") + "\"" + step_name(d) + "\":" + number;
      first_fraction = false;
    }
  }
  j += "},\"lastStep\":" + (v.last_detail == d_none ? std::string("null") : "\"" + std::string(step_name(v.last_detail)) + "\"");
  j += std::string(",\"finished\":") + b(v.finished) + "}";
  // Windows asked for a restart: the engine exited with 3010 (finish_run: needs-reboot) or reported reasons.
  const bool restart = v.page == pg::done && (v.exit_code == 3010 || (!v.reboot_reasons.empty() && v.reboot_reasons != "none"));
  j += std::string(",\"restart\":\"") + (restart ? "optional" : "none") + "\"";
  j += ",\"rebootReasons\":[";
  if (restart && v.reboot_reasons != "none") {
    size_t pos = 0;
    bool f = true;
    while (pos <= v.reboot_reasons.size()) {
      size_t e = v.reboot_reasons.find(',', pos);
      if (e == std::string::npos) e = v.reboot_reasons.size();
      const std::string r = v.reboot_reasons.substr(pos, e - pos);
      if (!r.empty()) {
        j += std::string(f ? "" : ",") + json_text(r);
        f = false;
      }
      pos = e + 1;
    }
  }
  j += "]";
  const char *failure = v.failure == fail_kind::driver ? "driver" : v.failure == fail_kind::service ? "service" : "generic";
  j += ",\"failure\":" + (v.page == pg::failed ? "\"" + std::string(failure) + "\"" : std::string("null"));
  j += ",\"exitCode\":" + std::to_string(v.exit_code);
  j += ",\"runId\":" + json_text(utf8(g_run.id));
  j += std::string(",\"guidePending\":") + b(v.page == pg::done ? guide_pending() : !(v.upgrade || v.maintenance));
  j += std::string(",\"opening\":") + b(v.opening);
  j += std::string(",\"panelFailed\":") + b(v.panel_failed);
  j += std::string(",\"busy\":") + b(v.page == pg::uac || v.page == pg::installing);
  return j + "}";
}

bool
web_write(const std::string &line) {
  if (!W.to_child) return false;
  const std::string data = line + "\n";
  DWORD written = 0;
  return WriteFile(W.to_child, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) != FALSE && written == data.size();
}

// Send the view when it changed. The page cannot tell "unchanged" from "lost", so nothing else is sent.
void
web_push(bool force = false) {
  // the native shell sends a cancelled UAC prompt to the welcome page; with an installed copy that page is maintenance
  if (S.v.maintenance && S.v.page == pg::welcome) S.v.page = pg::maintenance;
  const std::string body = view_json();
  if (!force && body == W.last_sent) return;
  W.last_sent = body;
  ++W.seq;
  web_log("view " + std::to_string(W.seq) + " page " + page_name(S.v.page));
  web_write("{\"type\":\"view\",\"view\":" + body.substr(0, body.size() - 1) + ",\"seq\":" + std::to_string(W.seq) + "}}");
}

void
web_close_ui() {
  if (W.to_child) {
    CloseHandle(W.to_child);  // end of stream: the UI closes its window and exits
    W.to_child = nullptr;
  }
}

void
web_quit(int code) {
  if (W.quitting) return;
  W.quitting = true;
  W.exit_code = code;
  web_close_ui();
  if (S.window) DestroyWindow(S.window);
}

// One action from the page. Each is the same click() as the native window's button for that page, so both front
// ends run exactly the same logic.
void
web_action(const std::string &line) {
  const size_t at = line.find("\"action\":");
  const std::string action = at == std::string::npos ? std::string() : line.substr(at);
  const std::string type = json_str(action, "type");
  const pg page = S.v.page;
  web_log("action " + type + " on " + page_name(page));
  if (type == "ready") {
    web_push(true);
    return;
  }
  if (type == "toggle-component") {
    const std::string id = json_str(action, "id");
    for (int i = 0; i < kTileCount; ++i)
      if (id == kComponentIds[i] && page == pg::components) click(kTileBase + i);
  }
  else if (type == "select-all") {
    if (page == pg::components) click(kSelectAll);
  }
  else if (type == "install" || type == "retry") {
    const bool allowed = type == "install" ? (page == pg::welcome || page == pg::maintenance || page == pg::components) : page == pg::failed;
    if (allowed) {
      // The page sees the wait for Windows' consent at once: ShellExecuteEx(runas) below does not return before the
      // person answers the UAC prompt, and this thread sends nothing meanwhile.
      S.v.page = pg::uac;
      web_push();
      S.v.page = page;
      if (GetEnvironmentVariableW(L"FOUNDATION_SETUP_TEST_OFFSCREEN", nullptr, 0) != 0) {
        // a guarded test instance never raises a UAC prompt: it answers as if the person had said No
        web_log("test instance: install not started (no UAC prompt)");
        S.v.uac_cancelled = true;
      }
      else
        click(kBtnPrimary);
    }
  }
  else if (type == "customize") {
    if (page == pg::welcome || page == pg::maintenance) click(kBtnSecond);
  }
  else if (type == "back") {
    if (page == pg::components) click(kBtnSecond);
  }
  else if (type == "uninstall") {
    if (page == pg::maintenance && S.v.has_uninstaller) {
      if (!S.uninstall_string.empty() && installer_maintenance::start_uninstaller(S.uninstall_string)) web_quit(0);
      return;
    }
  }
  else if (type == "open-panel") {
    if (page == pg::done) click(kBtnPrimary);
  }
  else if (type == "restart-pc") {
    if (page == pg::done) click(kBtnSecond);
  }
  else if (type == "copy-details") {
    if (page == pg::failed) click(kBtnSecond);
  }
  else if (type == "open-log-folder") {
    if (page == pg::failed) click(kBtnThird);
  }
  else if (type == "exit") {
    // Once the elevated worker runs, the window can be hidden but not abandoned mid-install (as WM_CLOSE natively).
    if (page == pg::uac || page == pg::installing)
      web_write("{\"type\":\"minimise\"}");
    else
      web_quit(0);
    return;
  }
  web_push();
}

LRESULT CALLBACK
web_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_WEB_LINE: {
      std::unique_ptr<std::string> line(reinterpret_cast<std::string *>(lparam));
      // the TOP-LEVEL type: the UI writes {"action":{"type":...},"type":"action"} (installer_json.h)
      const std::string type = installer_json::top_str(*line, "type");
      if (type == "hello") {
        web_log("ui hello");
        const size_t at = line->find("\"hwnd\":");
        g_uac_owner = at == std::string::npos ? nullptr : reinterpret_cast<HWND>(static_cast<intptr_t>(std::strtoll(line->c_str() + at + 7, nullptr, 10)));
        if (g_uac_owner && !IsWindow(g_uac_owner)) g_uac_owner = nullptr;
      }
      else if (type == "loaded") {
        web_log("page loaded");
        W.loaded = true;
        KillTimer(window, kWebWatchdog);
        web_push(true);
      }
      else if (type == "action")
        web_action(*line);
      else if (type == "log")
        web_log("ui: " + installer_json::top_str(*line, "text"));
      else
        web_log("ui line not understood (" + std::to_string(line->size()) + " bytes): " + line->substr(0, 120));  // never silent
      return 0;
    }
    case WM_WEB_GONE:
      web_log(std::string("ui closed its output") + (W.loaded ? "" : " before its page loaded: native window"));
      if (!W.quitting) {
        if (!W.loaded) {
          W.quitting = true;
          W.exit_code = kWebFallback;
          DestroyWindow(window);
        }
        else if (S.worker) {
          // The window was closed while the worker runs (only possible if the UI died): wait for the result, no UI.
          WaitForSingleObject(S.worker, INFINITE);
          web_quit(0);
        }
        else
          web_quit(0);
      }
      return 0;
    case WM_TIMER:
      if (wparam == kWebWatchdog) {
        KillTimer(window, kWebWatchdog);
        if (!W.loaded) {
          web_log("page did not load in time: native window");
          W.quitting = true;
          W.exit_code = kWebFallback;
          DestroyWindow(window);
        }
        return 0;
      }
      if (wparam == kTimer) {
        poll_run();
        if (S.v.page != pg::uac && S.v.page != pg::installing) KillTimer(window, kTimer);
      }
      if (S.v.page == pg::done && S.v.opening && GetTickCount64() - S.opening_since > 1800) S.v.opening = false;
      web_push();
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

DWORD WINAPI
web_reader(void *param) {
  const HWND window = static_cast<HWND>(param);
  std::string pending;
  char buffer[4096];
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(W.from_child, buffer, sizeof(buffer), &read, nullptr) || read == 0) break;
    pending.append(buffer, read);
    size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, nl);
      pending.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.size() > 64 * 1024 || line.empty()) continue;
      PostMessageW(window, WM_WEB_LINE, 0, reinterpret_cast<LPARAM>(new std::string(std::move(line))));
    }
  }
  PostMessageW(window, WM_WEB_GONE, 0, 0);
  return 0;
}

// Start the UI process with its stdin and stdout on our pipes, and only those two handles inherited
// (PROC_THREAD_ATTRIBUTE_HANDLE_LIST, installer-ms-updateprocthreadattribute-20261109).
bool
start_ui_process(const std::wstring &exe) {
  SECURITY_ATTRIBUTES inherit { sizeof(inherit), nullptr, TRUE };
  HANDLE child_in_read = nullptr, child_out_write = nullptr;
  if (!CreatePipe(&child_in_read, &W.to_child, &inherit, 0)) return false;
  if (!CreatePipe(&W.from_child, &child_out_write, &inherit, 0)) {
    CloseHandle(child_in_read);
    return false;
  }
  SetHandleInformation(W.to_child, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(W.from_child, HANDLE_FLAG_INHERIT, 0);
  HANDLE handles[2] = { child_in_read, child_out_write };
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  std::vector<unsigned char> attr_buffer(attr_size);
  auto *attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buffer.data());
  bool ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
            UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr);
  STARTUPINFOEXW startup {};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = child_in_read;
  startup.StartupInfo.hStdOutput = child_out_write;
  startup.StartupInfo.hStdError = child_out_write;
  startup.lpAttributeList = attrs;
  std::wstring command = L"\"" + exe + L"\" --host-pid " + std::to_wstring(GetCurrentProcessId()) + L" --data-dir \"" + W.dir + L"\\wv2\"";
  std::vector<wchar_t> command_line(command.begin(), command.end());
  command_line.push_back(L'\0');
  PROCESS_INFORMATION process {};
  ok = ok && CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT, nullptr,
               W.dir.c_str(), &startup.StartupInfo, &process);
  if (attr_size) DeleteProcThreadAttributeList(attrs);
  CloseHandle(child_in_read);
  CloseHandle(child_out_write);
  if (!ok) {
    CloseHandle(W.to_child);
    CloseHandle(W.from_child);
    W.to_child = W.from_child = nullptr;
    return false;
  }
  CloseHandle(process.hThread);
  W.child = process.hProcess;
  W.child_pid = process.dwProcessId;
  AllowSetForegroundWindow(process.dwProcessId);  // the person started us: the UI window may come to the front
  return true;
}

void
web_cleanup() {
  web_close_ui();
  if (W.child) {
    if (WaitForSingleObject(W.child, 5000) == WAIT_TIMEOUT) TerminateProcess(W.child, 1);  // our own child only
    CloseHandle(W.child);
    W.child = nullptr;
  }
  if (W.reader) {
    if (WaitForSingleObject(W.reader, 2000) == WAIT_TIMEOUT) CancelSynchronousIo(W.reader);
    WaitForSingleObject(W.reader, 1000);
    CloseHandle(W.reader);
    W.reader = nullptr;
  }
  if (W.from_child) {
    CloseHandle(W.from_child);
    W.from_child = nullptr;
  }
  if (!W.dir.empty()) {
    // The WebView2 browser processes may still hold files for a moment; whatever stays is in the user's temp folder.
    for (int i = 0; i < 10; ++i) {
      std::wstring from = W.dir;
      from.push_back(L'\0');
      from.push_back(L'\0');
      SHFILEOPSTRUCTW op {};
      op.wFunc = FO_DELETE;
      op.pFrom = from.c_str();
      op.fFlags = FOF_NO_UI;
      if (SHFileOperationW(&op) == 0 && GetFileAttributesW(W.dir.c_str()) == INVALID_FILE_ATTRIBUTES) break;
      Sleep(200);
    }
  }
}

// The web front end. Returns kWebFallback when the native window must be used instead (nothing was shown).
int
web_main() {
  if (!webview2_runtime_present()) {
    web_log("no WebView2 Runtime: native window");
    return kWebFallback;
  }
  g_run.self = module_path();
  uint64_t offset = 0, size = 0;
  if (!read_named_payload(g_run.self, "ui", &offset, &size)) {
    web_log("no setup UI payload: native window");
    return kWebFallback;
  }
  wchar_t temp_dir[MAX_PATH + 2] {};
  GetTempPathW(MAX_PATH, temp_dir);
  W.dir = std::wstring(temp_dir) + L"FoundationSunshine-setup-ui-" + make_run_id();
  const std::wstring exe = W.dir + L"\\foundation-setup-ui.exe";
  if (!make_directory_tree(W.dir) || !extract_range(g_run.self, offset, size, exe)) {
    web_log("setup UI could not be extracted: native window");
    web_cleanup();
    return kWebFallback;
  }
  g_web = true;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (!installer_maintenance::populate_view(S.v, &S.uninstall_string)) {
    const std::wstring dir = install_dir();
    S.v.upgrade = !dir.empty() && GetFileAttributesW((dir + L"\\sunshine.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
  }
  apply_os_limits(S.v);
  S.v.zh = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
  WNDCLASSW window_class {};
  window_class.lpfnWndProc = web_window_proc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = L"FoundationSunshineInstallerWebHost";
  RegisterClassW(&window_class);
  // message-only: timers, the clipboard owner of copy_details() and the target of the reader thread
  S.window = CreateWindowExW(0, window_class.lpszClassName, kWindowTitle, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, window_class.hInstance, nullptr);
  if (!S.window || !start_ui_process(exe)) {
    web_log("setup UI could not start: native window");
    if (S.window) DestroyWindow(S.window);
    S.window = nullptr;
    g_web = false;
    web_cleanup();
    return kWebFallback;
  }
  web_log("setup UI started, pid " + std::to_string(W.child_pid));
  web_push(true);  // waits in the pipe until the UI reads it
  W.reader = CreateThread(nullptr, 0, web_reader, S.window, 0, nullptr);
  SetTimer(S.window, kWebWatchdog, kWebLoadTimeoutMs, nullptr);
  SetTimer(S.window, kWebTick, 250, nullptr);  // the done page's "opening" state ends on time
  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  S.window = nullptr;
  const int code = W.exit_code;
  web_log("web front end ends with " + std::to_string(code));
  web_cleanup();
  if (code == kWebFallback && GetEnvironmentVariableW(L"FOUNDATION_SETUP_TEST_OFFSCREEN", nullptr, 0) != 0) {
    // a guarded test instance never opens the native window on the person's desk
    web_log("test instance: no native fallback window");
    return 0;
  }
  if (code == kWebFallback) {
    // Nothing was shown (the page never loaded): the native window takes over from the start.
    g_web = false;
    g_uac_owner = nullptr;
    S.v = view {};
    W.last_sent.clear();
  }
  return code;
}

// ================================================================ user-level shell

using namespace ui;

constexpr int kBtnPrimary = 101, kBtnSecond = 102, kBtnThird = 103, kTileBase = 120, kSelectAll = 130;
constexpr int kTitleMinimise = 140, kTitleClose = 141;
constexpr int kTileCount = 8;
constexpr int kSlots = 3 + kTileCount + 1;  // buttons, rows and the select-all text button; the two caption buttons follow
constexpr int kStateSlots = kSlots + 2;
constexpr double kTransitionSeconds = 0.300;  // Material 3 shared axis, see motion below
constexpr float kBtnInflate = 4;  // logical px around a button for the focus ring

struct shell_state {
  view v;
  HWND window = nullptr;
  HWND controls[3 + kTileCount + 1] {};
  HWND title_buttons[2] {};
  float max_scroll = 0, view_h = 0, row_h = 0, row_y[8] = {};
  ID2D1Factory *d2d = nullptr;
  IDWriteFactory *write = nullptr;
  IWICImagingFactory *wic = nullptr;
  IWICBitmapSource *brand_source = nullptr;
  ID2D1Bitmap *brand = nullptr;
  ID2D1RenderTarget *target = nullptr;
  IWICBitmap *bench_bitmap = nullptr;
  HICON icon_big = nullptr;
  float scale = 1.f;
  bool dark = false, hc = false;
  palette pal = light_palette();
  std::vector<button_spec> specs;
  int frame_w = 0, frame_h = 0;
  HANDLE worker = nullptr;
  ULONGLONG start_tick = 0;
  ULONGLONG opening_since = 0;
  int hover_id = 0;
  int tick = 0;
  bool restart_marker_written = false;
  std::wstring last_title;
  std::wstring uninstall_string;
  float scroll_target = 0;
  ULONGLONG transition_until = 0;
  bool title_min_hover = false, title_close_hover = false;
  std::vector<double> bench_frame_ms;      // render + present cost of a frame
  std::vector<double> bench_interval_ms;   // time between two presented frames
  // --- motion ---
  text_cache tcache;
  view drawn_v;                 // the view as last drawn live; the outgoing page of a transition
  bool dirty = true;            // something changed: draw one frame
  float hover_amt[kStateSlots] = {}, press_amt[kStateSlots] = {};
  int hover_slot = -1, press_slot = -1;
  bool tr_on = false, tr_plain = false;
  double tr_start = -1;         // seconds; set by the first transition frame, so frame 0 is at progress 0
  int tr_dir = 1;
  ID2D1Bitmap *tr_old = nullptr;
  ID2D1BitmapRenderTarget *tr_rt = nullptr;
  double last_tick = 0, last_end = 0, refresh_s = 1.0 / 60.0, start_s = 0;
  size_t spec_sig = 0;
  size_t events_size = 0;
  bool focus_visible = false;   // like CSS :focus-visible: the ring shows after keyboard navigation, not after a mouse click or a page change
} S;
bool g_bench = false;
std::wstring g_bench_output;
std::wstring g_selftest_out;  // --selftest-window OUT: the real window, every page captured with PrintWindow to OUT-<page>.png
int selftest_window(HWND window);  // installer_preview.inc
// Web front end (installer_web.inc): the pages are drawn by the WebView2 window of the setup UI process; this
// process keeps the install logic and drives the same view. go() and transitions then draw nothing here.
bool g_web = false;
HWND g_uac_owner = nullptr;  // owner of the UAC prompt: the window the person looks at (the setup UI window in web mode)

int
slot_id(const int slot) {
  return slot < 3 ? kBtnPrimary + slot : slot < 3 + kTileCount ? kTileBase + slot - 3 :
                                                                 kSelectAll;
}

// Windows 10+ DPI and DWM entry points are looked up at run time so the exe still starts on
// an older user32 and needs no import library for them.
double
now_s() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f {};
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER t {};
  QueryPerformanceCounter(&t);
  return static_cast<double>(t.QuadPart) / static_cast<double>(freq.QuadPart);
}

int
slot_of_id(const int id) {
  if (id >= kBtnPrimary && id <= kBtnThird) return id - kBtnPrimary;
  if (id >= kTileBase && id < kTileBase + kTileCount) return 3 + id - kTileBase;
  if (id == kSelectAll) return 3 + kTileCount;
  if (id == kTitleMinimise) return kSlots;
  if (id == kTitleClose) return kSlots + 1;
  return -1;
}

template <typename F>
F
proc(const wchar_t *dll, const char *name) {
  HMODULE m = LoadLibraryW(dll);
  return m ? reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(m, name))) : nullptr;
}

float
window_scale(HWND window) {
  using fn = UINT(WINAPI *)(HWND);
  static fn get = proc<fn>(L"user32.dll", "GetDpiForWindow");
  return get && window ? static_cast<float>(get(window)) / 96.f : 1.f;
}

bool
system_dark() {
  DWORD value = 1, size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
    return false;
  }
  return value == 0;
}

unsigned
sys_rgb(int index) {
  const COLORREF c = GetSysColor(index);
  return (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
}

void
refresh_environment() {
  HIGHCONTRASTW hc {};
  hc.cbSize = sizeof(hc);
  S.hc = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
  S.dark = system_dark();
  if (S.hc)
    S.pal = hc_palette(sys_rgb(COLOR_WINDOW), sys_rgb(COLOR_WINDOWTEXT), sys_rgb(COLOR_HIGHLIGHT), sys_rgb(COLOR_HIGHLIGHTTEXT), sys_rgb(COLOR_GRAYTEXT));
  else
    S.pal = S.dark ? dark_palette() : light_palette();
  BOOL animate = TRUE;
  SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
  S.v.animated = animate != FALSE;
  S.v.zh = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
  if (S.window) {
    using dwm_fn = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    static dwm_fn set = proc<dwm_fn>(L"dwmapi.dll", "DwmSetWindowAttribute");
    if (set) {
      const BOOL dark = S.dark && !S.hc;
      set(S.window, 20, &dark, sizeof(dark));
      const COLORREF caption = RGB(S.pal.page.GetR(), S.pal.page.GetG(), S.pal.page.GetB());
      set(S.window, 35, &caption, sizeof(caption));
      // Authority: installer-ms-dwm-window-attributes-20261107 and installer-ms-custom-frame-20261107.
      const DWORD corners = 2;  // DWMWCP_ROUND, retained as a value for older SDK headers.
      set(S.window, 33, &corners, sizeof(corners));
    }
  }
}

void
load_brand() {
  if (!S.wic || !S.d2d) return;
  IWICStream *stream = nullptr;
  IWICBitmapDecoder *decoder = nullptr;
  IWICBitmapFrameDecode *frame = nullptr;
  IWICFormatConverter *converter = nullptr;
  if (FAILED(S.wic->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(const_cast<BYTE *>(kIconPng), sizeof(kIconPng))) ||
      FAILED(S.wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame)) || FAILED(S.wic->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut))) {
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    return;
  }
  S.brand_source = converter;
  converter = nullptr;
  frame->Release();
  decoder->Release();
  stream->Release();
}

void
release_transition() {
  S.tr_on = false;
  if (S.tr_old) {
    S.tr_old->Release();
    S.tr_old = nullptr;
  }
  if (S.tr_rt) {
    S.tr_rt->Release();
    S.tr_rt = nullptr;
  }
}

// Everything that belongs to the render target: it is recreated on resize, DPI change or device loss.
void
release_target() {
  release_transition();
  if (S.brand) {
    S.brand->Release();
    S.brand = nullptr;
  }
  if (S.bench_bitmap) {
    S.bench_bitmap->Release();
    S.bench_bitmap = nullptr;
  }
  if (S.target) {
    S.target->Release();
    S.target = nullptr;
  }
}

void
ensure_target() {
  // Authority: installer-ms-direct2d-portal-20261107 and installer-ms-id2d1factory1-20261107.
  // The default target uses the GPU path; WARP is the documented software fallback for basic displays.
  // The render target is pinned to 96 dpi: every coordinate is multiplied by the window scale (the real
  // per-monitor DPI), and the factory's default (the desktop dpi) would scale a second time on a
  // 125-200 % display.
  if (!S.window || !S.d2d) {
    return;
  }
  RECT rect {};
  GetClientRect(S.window, &rect);
  const UINT width = static_cast<UINT>(max(1L, rect.right - rect.left));
  const UINT height = static_cast<UINT>(max(1L, rect.bottom - rect.top));
  if (S.target && S.frame_w == static_cast<int>(width) && S.frame_h == static_cast<int>(height)) return;
  release_target();
  S.frame_w = static_cast<int>(width);
  S.frame_h = static_cast<int>(height);
  D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
  // Vsync present (the default): EndDraw returns at the display's pace, which paces the frame loop.
  D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props = D2D1::HwndRenderTargetProperties(S.window, D2D1::SizeU(width, height));
  ID2D1HwndRenderTarget *hwnd_target = nullptr;
  const HRESULT create_result = S.d2d->CreateHwndRenderTarget(props, hwnd_props, &hwnd_target);
  S.target = hwnd_target;
  if (FAILED(create_result)) {
    if (hwnd_target) {
      hwnd_target->Release();
      S.target = nullptr;
    }
    props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
    const HRESULT software_result = S.d2d->CreateHwndRenderTarget(props, hwnd_props, &hwnd_target);
    S.target = hwnd_target;
    if (FAILED(software_result) && hwnd_target) {
      hwnd_target->Release();
      S.target = nullptr;
    }
    if (g_bench && FAILED(software_result) && S.wic) {
      S.wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &S.bench_bitmap);
      if (S.bench_bitmap) S.d2d->CreateWicBitmapRenderTarget(S.bench_bitmap, props, &S.target);
    }
  }
  if (S.target) S.target->SetDpi(96.f, 96.f);
  if (S.target && S.brand_source && !S.brand) S.target->CreateBitmapFromWicBitmap(S.brand_source, nullptr, &S.brand);
  S.dirty = true;
}

// ---------------------------------------------------------------- Windows builds (docs/windows-compatibility.md)
// The build of this Windows from RtlGetVersion (not version-lied by a manifest); 0 before Windows 10.
DWORD
windows_build() {
  static DWORD build = [] {
    using fn = LONG(WINAPI *)(OSVERSIONINFOW *);
    fn get = proc<fn>(L"ntdll.dll", "RtlGetVersion");
    OSVERSIONINFOW info {};
    info.dwOSVersionInfoSize = sizeof(info);
    if (!get || get(&info) != 0) return static_cast<DWORD>(0);
    return info.dwMajorVersion >= 10 ? info.dwBuildNumber : static_cast<DWORD>(0);
  }();
  return build;
}
// Tile order: vdd, gamepad, usbip, vmouse, firewall, autostart, tools, desktopicon. VDD: UMDF 2.25 + IddCx 1.2 (1803);
// usbip-win2: 1903. The same limits are MinVersion in sunshine.iss.in, so a silent install skips them too.
DWORD
component_min_build(int tile) {
  return tile == 0 ? 17134 : tile == 2 ? 18362 : 0;
}
const char *
component_min_name(int tile) {
  return tile == 0 ? "1803" : tile == 2 ? "1903" : "";
}
// A component this Windows cannot run is unticked and stays so (the page shows it disabled with the version it needs).
void
apply_os_limits(view &v) {
  for (int i = 0; i < kTileCount; ++i)
    if (component_min_build(i) > windows_build()) v.tiles[i] = false;
}

std::wstring
install_dir() {
  wchar_t buffer[1024] {};
  DWORD size = sizeof(buffer) - sizeof(wchar_t);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AlkaidLab\\Sunshine", L"InstallDir", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
        nullptr, buffer, &size) != ERROR_SUCCESS) {
    return {};
  }
  return buffer;
}

std::wstring
panel_exe() {
  const std::wstring dir = install_dir();
  if (dir.empty()) return {};
  const std::wstring exe = dir + L"\\assets\\gui\\sunshine-gui.exe";
  return GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES ? std::wstring() : exe;
}

// The host records the start guide in its configuration (config\sunshine.conf next to sunshine.exe): the keys
// guide_done, guide_finished, guide_closed and guide_active that foundation-ui guideHost.js writes. Only
// guide_finished matters here. An unreadable file reads as "not recorded".
bool
guide_finished_on_host() {
  const std::wstring dir = install_dir();
  if (dir.empty()) return false;
  const std::string conf = read_file(dir + L"\\config\\sunshine.conf");
  const auto trim = [](std::string v) {
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
    size_t b = 0;
    while (b < v.size() && (v[b] == ' ' || v[b] == '\t')) ++b;
    return v.substr(b);
  };
  size_t pos = 0;
  while (pos < conf.size()) {
    size_t end = conf.find('\n', pos);
    if (end == std::string::npos) end = conf.size();
    const std::string line = conf.substr(pos, end - pos);
    pos = end + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos || trim(line.substr(0, eq)) != "guide_finished") continue;
    const std::string value = trim(line.substr(eq + 1));
    return value == "true" || value == "1";
  }
  return false;
}

// 开启串流旅程 opens the Panel at the start guide only when the guide is still to do: always after a fresh install;
// after an upgrade or a maintenance run only when the host has not recorded the guide as finished. Otherwise the
// Panel starts plainly and its own first-run logic (guideEntry) decides.
bool
guide_pending() {
  if (!S.v.upgrade && !S.v.maintenance) return true;
  return !guide_finished_on_host();
}

// The panel is started from this (user-level) process, so it gets the user's token and the
// desktop session.
bool
launch_panel() {
  const std::wstring exe = panel_exe();
  if (exe.empty()) return false;
  const std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
  SHELLEXECUTEINFOW info {};
  info.cbSize = sizeof(info);
  info.lpVerb = L"open";
  info.lpFile = exe.c_str();
  info.lpDirectory = dir.c_str();
  info.lpParameters = guide_pending() ? L"--start-guide" : nullptr;  // --start-guide: the Panel opens at the first step of its start guide
  info.nShow = SW_SHOWNORMAL;
  return ShellExecuteExW(&info) != FALSE;
}

// Resume the guide after a restart: a per-user RunOnce entry that starts the panel once.
void
write_restart_resume() {
  if (S.restart_marker_written) return;
  const std::wstring exe = panel_exe();
  if (exe.empty()) return;
  const std::wstring command = L"\"" + exe + L"\"";
  if (RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", L"FoundationSunshineFirstRun",
        REG_SZ, command.c_str(), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS) {
    S.restart_marker_written = true;
  }
}

void
restart_windows() {
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    TOKEN_PRIVILEGES privileges {};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid))
      AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
    CloseHandle(token);
  }
  ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_INSTALLATION | SHTDN_REASON_FLAG_PLANNED);
}

void
copy_details() {
  std::wstring text = L"Foundation Sunshine installer\r\nrun: " + g_run.id + L"\r\nexit code: " + std::to_wstring(S.v.exit_code) +
                      L"\r\nfolder: " + g_run.directory + L"\r\nlog: " + g_run.directory + L"\\inno.log\r\n";
  if (!OpenClipboard(S.window)) return;
  EmptyClipboard();
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
  if (mem) {
    std::memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(mem);
    if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
  }
  CloseClipboard();
}

void
apply_page_focus();

// ---------------------------------------------------------------- motion
// Authority: Material 3 motion (m3.material.io/styles/motion): emphasized decelerate cubic-bezier(0.05, 0.7, 0.1, 1),
// emphasized accelerate (0.3, 0, 0.8, 0.15); shared axis X: the outgoing page fades out and moves 30 dp
// toward the direction of travel, then the incoming page fades in from 30 dp on the other side; 300 ms.
// State layers 100-150 ms. All of it is a function of the clock, never of the frame count.
float
bezier_y(const float x1, const float y1, const float x2, const float y2, const float x) {
  if (x <= 0) return 0;
  if (x >= 1) return 1;
  float lo = 0, hi = 1, u = x;
  for (int i = 0; i < 24; ++i) {  // x(u) is monotonic for these control points: bisection is enough
    u = (lo + hi) / 2;
    const float inv = 1 - u;
    const float bx = 3 * inv * inv * u * x1 + 3 * inv * u * u * x2 + u * u * u;
    if (bx < x)
      lo = u;
    else
      hi = u;
  }
  const float inv = 1 - u;
  return 3 * inv * inv * u * y1 + 3 * inv * u * u * y2 + u * u * u;
}
float
ease_decelerate(const float x) { return bezier_y(0.05f, 0.7f, 0.1f, 1.f, x); }
float
ease_accelerate(const float x) { return bezier_y(0.3f, 0.f, 0.8f, 0.15f, x); }

int
page_rank(const pg page) {
  switch (page) {
    case pg::welcome:
    case pg::maintenance:
      return 0;
    case pg::components:
      return 1;
    case pg::uac:
      return 2;
    case pg::installing:
      return 3;
    default:
      return 4;
  }
}

// One page: every pixel of it, buttons included, drawn into `rt` (the window or an offscreen copy of it).
frame_result
draw_scene(ID2D1RenderTarget &rt, canvas &c, view &v, const bool live) {
  frame_result result = draw_page(c, v, S.brand);
  if (live && (v.scroll > result.max_scroll || v.scroll < 0)) {  // the window or the list changed: clamp and draw again
    v.scroll = std::clamp(v.scroll, 0.f, result.max_scroll);
    S.scroll_target = v.scroll;
    c.clear(S.pal.page);
    result = draw_page(c, v, S.brand);
  }
  (void) rt;
  for (const auto &button : result.buttons) {
    if (!button.visible || button.kind == bkind::row || button.kind == bkind::tile) continue;
    c.save();
    c.translate(button.x * S.scale, button.y * S.scale);
    btn_state state;
    const int slot = slot_of_id(button.id);
    if (live && slot >= 0) {
      state.hover = S.hover_amt[slot];
      state.press = S.press_amt[slot];
      state.focus = S.focus_visible && GetFocus() == S.controls[slot < 3 ? slot : 3 + kTileCount];
    }
    draw_button(c, button, state, S.pal.card);
    c.restore();
  }
  return result;
}

ID2D1BitmapRenderTarget *
make_offscreen() {
  ID2D1BitmapRenderTarget *bt = nullptr;
  const D2D1_SIZE_U px = D2D1::SizeU(static_cast<UINT>(S.frame_w), static_cast<UINT>(S.frame_h));
  if (!S.target || FAILED(S.target->CreateCompatibleRenderTarget(nullptr, &px, nullptr, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &bt))) return nullptr;
  bt->SetDpi(96.f, 96.f);
  return bt;
}

// Called when the page is about to change: keep the last drawn page as the outgoing image.
void
begin_transition(const pg from, const pg to) {
  if (g_web) return;
  release_transition();
  ensure_target();
  if (!S.v.animated || !S.target || from == to) return;
  ID2D1BitmapRenderTarget *snap = make_offscreen();
  if (!snap) return;
  view old = S.drawn_v;
  old.focus_row = -1;
  for (int i = 0; i < 8; ++i) old.row_hover[i] = old.row_press[i] = 0;
  snap->BeginDraw();
  {
    canvas c(*snap, S.write, S.d2d, S.scale, S.pal, old.zh, &S.tcache);
    draw_scene(*snap, c, old, false);
  }
  if (SUCCEEDED(snap->EndDraw())) snap->GetBitmap(&S.tr_old);
  snap->Release();
  S.tr_rt = make_offscreen();
  S.tr_dir = page_rank(to) >= page_rank(from) ? 1 : -1;
  S.tr_plain = from == pg::done || to == pg::done;  // the success page has no rail: cross-fade the whole window
  S.tr_start = -1;
  S.tr_on = S.tr_old && S.tr_rt;
  if (!S.tr_on) release_transition();
}

// The incoming page, live, is drawn into tr_rt and composed with the frozen outgoing page. `el` seconds since the start.
frame_result
compose_transition(ID2D1RenderTarget &rt, canvas &c, const double el) {
  S.tr_rt->BeginDraw();
  frame_result result;
  {
    canvas nc(*S.tr_rt, S.write, S.d2d, S.scale, S.pal, S.v.zh, &S.tcache);
    result = draw_scene(*S.tr_rt, nc, S.v, false);
  }
  S.tr_rt->EndDraw();
  ID2D1Bitmap *incoming = nullptr;
  S.tr_rt->GetBitmap(&incoming);
  const float s = S.scale, fw = static_cast<float>(S.frame_w), fh = static_cast<float>(S.frame_h);
  const float t = static_cast<float>(std::clamp(el / kTransitionSeconds, 0.0, 1.0));
  const float exit_end = 0.3f;  // 90 ms of 300 ms
  const float t_out = std::clamp(t / exit_end, 0.f, 1.f), t_in = std::clamp((t - exit_end) / (1 - exit_end), 0.f, 1.f);
  const D2D1_RECT_F whole = D2D1::RectF(0, 0, fw, fh);
  rt.DrawBitmap(incoming, whole, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);  // the rail and window background, live
  if (S.tr_plain) {
    // No shared card: old image, the new one fading in over it.
    rt.DrawBitmap(S.tr_old, whole, 1.f - t, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    if (incoming) incoming->Release();
    return result;
  }
  // The card's inner area (inside the 28 dp corners): the content moves and fades here and nowhere else.
  const D2D1_RECT_F inner = D2D1::RectF(c.snap((kCardX + 14) * s), c.snap((kCardY + 14) * s), c.snap((kCardX + kCardW - 14) * s), c.snap((kCardY + kCardH - 14) * s));
  c.fill_r(inner.left / s, inner.top / s, (inner.right - inner.left) / s, (inner.bottom - inner.top) / s, 0, S.pal.card);
  rt.PushAxisAlignedClip(inner, D2D1_ANTIALIAS_MODE_ALIASED);
  const float travel = 30.f * s * static_cast<float>(S.tr_dir);
  if (t_out < 1.f) {
    const float dx = c.snap(-travel * ease_accelerate(t_out));
    rt.DrawBitmap(S.tr_old, D2D1::RectF(inner.left + dx, inner.top, inner.right + dx, inner.bottom), 1.f - t_out, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, inner);
  }
  if (t_in > 0.f) {
    const float dx = c.snap(travel * (1.f - ease_decelerate(t_in)));
    rt.DrawBitmap(incoming, D2D1::RectF(inner.left + dx, inner.top, inner.right + dx, inner.bottom), t_in, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, inner);
  }
  rt.PopAxisAlignedClip();
  if (incoming) incoming->Release();
  return result;
}

void
render_frame() {
  ensure_target();
  if (!S.target) return;
  LARGE_INTEGER bench_start {}, bench_end {}, frequency {};
  if (g_bench) {
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&bench_start);
  }
  const double now = now_s();
  S.v.t = now - S.start_s;
  for (int i = 0; i < kTileCount; ++i) {
    S.v.row_hover[i] = S.hover_amt[3 + i];
    S.v.row_press[i] = S.press_amt[3 + i];
  }
  S.v.focus_row = -1;
  {
    const HWND focus = GetFocus();
    for (int i = 0; i < kTileCount && S.focus_visible; ++i)
      if (S.controls[3 + i] == focus) S.v.focus_row = i;
  }
  bool in_transition = S.tr_on && S.tr_old && S.tr_rt;
  double elapsed = 0;
  if (in_transition) {
    if (S.tr_start < 0) S.tr_start = now;  // the first frame of a transition is at progress 0, whatever the snapshot cost
    elapsed = now - S.tr_start;
    if (elapsed >= kTransitionSeconds) {
      release_transition();
      in_transition = false;
    }
  }
  S.target->BeginDraw();
  canvas c(*S.target, S.write, S.d2d, S.scale, S.pal, S.v.zh, &S.tcache);
  frame_result result = in_transition ? compose_transition(*S.target, c, elapsed) : draw_scene(*S.target, c, S.v, true);
  if (!in_transition) S.drawn_v = S.v;
  S.max_scroll = result.max_scroll;
  S.view_h = result.view_h;
  S.row_h = result.row_h;
  std::copy(std::begin(result.row_y), std::end(result.row_y), std::begin(S.row_y));
  const std::wstring status = result.status;
  S.specs = std::move(result.buttons);
  // The title controls are drawn on the same surface as the page, avoiding a second
  // software-painted child surface. Their hit test remains native and accessible.
  const float right = static_cast<float>(S.frame_w) / S.scale;
  c.draw_brand(S.brand, 12, 8, 24, 24);
  c.text(44, 11, 300, kWindowTitle, 13, 1, S.pal.on_surface);
  if (S.hover_amt[kSlots] > 0.002f) c.disc(right - 60, 20, 32, alpha(S.pal.primary, static_cast<int>(22 * S.hover_amt[kSlots] + (S.press_amt[kSlots] * 14))));
  if (S.hover_amt[kSlots + 1] > 0.002f) c.disc(right - 20, 20, 32, alpha(S.pal.error, static_cast<int>(30 * S.hover_amt[kSlots + 1] + (S.press_amt[kSlots + 1] * 20))));
  c.line(right - 66, 20, right - 54, 20, S.pal.on_surface, 2.f);
  icon(c, ic::close, right - 20, 20, 16, S.pal.on_surface, 1.6f);
  const std::wstring title = std::wstring(kWindowTitle) + L" - " + status;
  if (title != S.last_title) {
    S.last_title = title;
    SetWindowTextW(S.window, title.c_str());  // the window name is the live status for screen readers
  }
  const HRESULT end = S.target->EndDraw();
  if (end == D2DERR_RECREATE_TARGET) release_target();
  S.dirty = false;
  if (g_bench) {
    QueryPerformanceCounter(&bench_end);
    if (frequency.QuadPart > 0)
      S.bench_frame_ms.push_back(1000.0 * static_cast<double>(bench_end.QuadPart - bench_start.QuadPart) / frequency.QuadPart);
  }
}

std::wstring
accessible_name(const button_spec &b) {
  if (b.kind != bkind::tile) return b.text;
  const wchar_t *on = S.v.zh ? L"已选择" : L"selected";
  const wchar_t *off = S.v.zh ? L"未选择" : L"not selected";
  return b.text + (S.v.zh ? L"," : L", ") + b.desc + (S.v.zh ? L"," : L", ") + (b.checked ? on : off);
}

void
sync_controls() {
  const float pad = kBtnInflate * S.scale;
  for (int slot = 0; slot < kSlots; ++slot) {
    const int id = slot_id(slot);
    const button_spec *spec = nullptr;
    for (const auto &b : S.specs)
      if (b.id == id) spec = &b;
    HWND control = S.controls[slot];
    if (!spec || !spec->visible) {
      if (IsWindowVisible(control)) ShowWindow(control, SW_HIDE);
      continue;
    }
    const std::wstring name = accessible_name(*spec);
    wchar_t current[256] {};
    GetWindowTextW(control, current, 256);
    if (name != current) SetWindowTextW(control, name.c_str());
    const int x = static_cast<int>(spec->x * S.scale - pad), y = static_cast<int>(spec->y * S.scale - pad);
    const int w = static_cast<int>(spec->w * S.scale + 2 * pad), h = static_cast<int>(spec->h * S.scale + 2 * pad);
    RECT now {};
    GetWindowRect(control, &now);
    POINT origin { now.left, now.top };
    ScreenToClient(S.window, &origin);
    const bool moved = origin.x != x || origin.y != y || now.right - now.left != w || now.bottom - now.top != h;
    // Move without the move's own repaint (SWP_NOREDRAW) and without copying the old pixels
    // (SWP_NOCOPYBITS); refresh() repaints everything once, with no erase, so a scroll does not
    // flash the button class background between the move and our WM_DRAWITEM.
    if (moved) SetWindowPos(control, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);
    EnableWindow(control, spec->enabled);
    if (!IsWindowVisible(control)) ShowWindow(control, SW_SHOWNA);
  }
  const int title_width = static_cast<int>(40 * S.scale);
  for (int i = 0; i < 2; ++i) {
    SetWindowPos(S.title_buttons[i], nullptr, S.frame_w - title_width * (2 - i), 0, title_width, static_cast<int>(40 * S.scale),
      SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    if (!IsWindowVisible(S.title_buttons[i])) ShowWindow(S.title_buttons[i], SW_SHOWNA);
  }
}

// Ask for a frame. Nothing is drawn here: the frame loop draws once per display refresh, however many
// events arrived, and draws nothing at all while nothing changes.
void
refresh() { S.dirty = true; }

size_t
spec_signature() {
  size_t h = 1469598103934665603ull;
  const auto mix = [&](size_t v) { h = (h ^ v) * 1099511628211ull; };
  for (const auto &b : S.specs) {
    mix(static_cast<size_t>(b.id));
    mix(static_cast<size_t>(b.x * 4 + 100000));
    mix(static_cast<size_t>(b.y * 4 + 100000));
    mix(static_cast<size_t>(b.w * 4));
    mix(b.visible * 2 + b.enabled);
    mix(b.checked);
    mix(std::hash<std::wstring> {}(b.text));
  }
  return h;
}

// Draw now (page changes, startup) so the controls exist before focus is moved to them.
void
flush_now() {
  render_frame();
  sync_controls();
  S.spec_sig = spec_signature();
}

bool
continuous_page() {
  return S.v.animated && !IsIconic(S.window) && (S.v.page == pg::uac || S.v.page == pg::installing || (S.v.page == pg::done && S.v.opening));
}

bool
frame_wanted() {
  if (S.dirty || S.tr_on) return true;
  if (continuous_page()) return true;
  for (int i = 0; i < kStateSlots; ++i) {
    if (std::fabs(S.hover_amt[i] - (S.hover_slot == i ? 1.f : 0.f)) > 0.001f) return true;
    if (std::fabs(S.press_amt[i] - (S.press_slot == i ? 1.f : 0.f)) > 0.001f) return true;
  }
  return S.v.page == pg::components && std::fabs(S.v.scroll - S.scroll_target) > 0.001f;
}

// Advance every animation to `now`. All of them are driven by the elapsed time, so a slow frame
// moves things further instead of slowing the motion down.
void
tick(const double now) {
  // After an idle stretch (nothing was animating) the first step is one display refresh, not the whole gap.
  double dt = S.last_tick > 0 ? now - S.last_tick : 0.0;
  if (dt > 0.1) dt = S.refresh_s;
  dt = std::min(dt, 0.05);
  S.last_tick = now;
  const bool animate = S.v.animated;
  for (int i = 0; i < kStateSlots; ++i) {
    const float hover_to = S.hover_slot == i ? 1.f : 0.f, press_to = S.press_slot == i ? 1.f : 0.f;
    const float hover_step = animate ? static_cast<float>(dt / 0.12) : 1.f;  // state layers: 120 ms
    const float press_step = animate ? static_cast<float>(dt / 0.08) : 1.f;
    S.hover_amt[i] += std::clamp(hover_to - S.hover_amt[i], -hover_step, hover_step);
    S.press_amt[i] += std::clamp(press_to - S.press_amt[i], -press_step, press_step);
  }
  if (S.v.page == pg::components && std::fabs(S.v.scroll - S.scroll_target) > 0.001f) {
    // Exponential ease toward the target (time constant 70 ms): small touchpad deltas and big wheel notches both glide.
    const float diff = S.scroll_target - S.v.scroll;
    if (!animate || std::fabs(diff) * S.scale < 0.4f)
      S.v.scroll = S.scroll_target;
    else
      S.v.scroll += diff * static_cast<float>(1.0 - std::exp(-dt / 0.07));
  }
  if (S.v.page == pg::done && S.v.opening && GetTickCount64() - S.opening_since > 1800) {
    S.v.opening = false;
    S.dirty = true;
  }
}

// Hold the loop to the display. EndDraw of the window target already waits for the vertical blank; when it
// did not (software target, occluded window, VM without a display) wait for the next DWM composition instead.
void
pace() {
  using flush_fn = HRESULT(WINAPI *)();
  static flush_fn flush = proc<flush_fn>(L"dwmapi.dll", "DwmFlush");
  const double now = now_s();
  if (now - S.last_end < S.refresh_s * 0.6) {
    if (!flush || FAILED(flush())) {
      const double wait = S.refresh_s - (now - S.last_end);
      if (wait > 0.002) Sleep(static_cast<DWORD>(wait * 1000.0 - 1.0));
    }
  }
  S.last_end = now_s();
}

void
present_frame() {
  const double now = now_s();
  tick(now);
  render_frame();
  const size_t signature = spec_signature();
  if (signature != S.spec_sig) {
    S.spec_sig = signature;
    sync_controls();
  }
  const double before = S.last_end;
  pace();
  // Only frames that followed each other directly count: the gap after an idle stretch is not a dropped frame.
  if (g_bench && before > 0 && now - before < 0.05) S.bench_interval_ms.push_back((S.last_end - before) * 1000.0);
}

void
go(pg page) {
  if (g_web) {
    S.v.page = page;
    return;
  }
  const pg previous = S.drawn_v.page;
  S.v.page = page;
  S.focus_visible = false;  // a new page starts with the ring hidden; Tab shows it
  if (previous != page) begin_transition(previous, page);
  S.scroll_target = S.v.scroll;
  flush_now();
  apply_page_focus();
}

void
apply_page_focus() {
  // Focus the primary action so the keyboard works at once; the ring itself appears on Tab.
  for (int slot = 0; slot < 3; ++slot) {
    if (IsWindowVisible(S.controls[slot]) && IsWindowEnabled(S.controls[slot])) {
      SetFocus(S.controls[slot]);
      return;
    }
  }
}

std::wstring
component_list() {
  // tile order: vdd, gamepad, usbip, vmouse, firewall, autostart, tools, then the desktop shortcut task
  static const wchar_t *names[] = { L"vdd", L"gamepad", L"usbip", L"vmouse", L"firewall", L"autostart", L"tools" };
  bool all = true;
  for (int i = 0; i < kTileCount; ++i) all = all && S.v.tiles[i];
  g_run.tasks = all ? L"" : (S.v.tiles[7] ? L"desktopicon" : L"none");
  if (all) return {};
  std::wstring list = L"application,assets,gui";
  for (int i = 0; i < 7; ++i)
    if (S.v.tiles[i]) list += std::wstring(L",") + names[i];
  return list;
}

void
begin_install() {
  g_run.id = make_run_id();
  g_run.directory = program_data() + L"\\" + kRunRoot + L"\\" + g_run.id;
  g_run.events = g_run.directory + L"\\events.jsonl";
  g_run.result = g_run.directory + L"\\install-result.json";
  g_run.components = component_list();
  // Authority: installer-ms-shellexecuteex-20261107 (SHELLEXECUTEINFO/runas)
  // and installer-ms-application-manifests-20261107 (UAC elevation model).
  // The elevated worker runs from a copy with a neutral name: the engine stops "sunshine.exe"
  // by image name, and a package that is itself called Sunshine.exe must survive that.
  wchar_t temp_dir[MAX_PATH + 2] {};
  GetTempPathW(MAX_PATH, temp_dir);
  g_run.worker_copy = std::wstring(temp_dir) + L"FoundationSunshine-setup-" + g_run.id + L".exe";
  std::wstring worker_path = g_run.self;
  if (CopyFileW(g_run.self.c_str(), g_run.worker_copy.c_str(), FALSE))
    worker_path = g_run.worker_copy;
  else
    g_run.worker_copy.clear();
  std::wstring parameters = L"--worker --run-id " + g_run.id + L" --shell-pid " + std::to_wstring(GetCurrentProcessId());
  if (!g_run.components.empty()) parameters += L" --components " + g_run.components;
  if (!g_run.tasks.empty()) parameters += L" --tasks " + g_run.tasks;
  SHELLEXECUTEINFOW execute {};
  execute.cbSize = sizeof(execute);
  execute.fMask = SEE_MASK_NOCLOSEPROCESS;
  execute.lpVerb = L"runas";
  execute.hwnd = g_uac_owner;
  execute.lpFile = worker_path.c_str();
  execute.lpParameters = parameters.c_str();
  execute.nShow = SW_HIDE;
  S.v.uac_cancelled = false;
  S.v.panel_failed = false;
  S.v.opening = false;
  S.v.finished = false;
  for (auto &x : S.v.subs) x = { -1, sstate::pending };
  S.v.last_detail = d_none;
  if (!ShellExecuteExW(&execute)) {
    if (GetLastError() == ERROR_CANCELLED) {
      S.v.uac_cancelled = true;
      go(pg::welcome);
    }
    else {
      S.v.exit_code = static_cast<int>(GetLastError());
      S.v.failure = fail_kind::generic;
      go(pg::failed);
    }
    return;
  }
  S.worker = execute.hProcess;
  S.events_size = 0;
  go(pg::uac);
  SetTimer(S.window, kTimer, 200, nullptr);  // the worker is polled five times a second; frames are paced separately
}

void
finish_run(const std::string &result_json, DWORD fallback_code) {
  std::string state = json_str(result_json, "state");
  long code = json_int(result_json, "result_code", static_cast<long>(fallback_code));
  if (result_json.empty()) {
    state = "unknown";
    code = static_cast<long>(fallback_code);
  }
  S.v.finished = true;
  S.v.exit_code = static_cast<int>(code);
  if (S.worker) {
    CloseHandle(S.worker);
    S.worker = nullptr;
  }
  if (state == "success" || state == "needs-reboot") S.v.webview2_missing = !webview2_runtime_present();
  if (state == "success") {
    // The success page waits for "Start your streaming journey"; nothing is opened on its own.
    S.v.opening = false;
    S.v.panel_failed = false;
    go(pg::done);
  }
  else if (state == "needs-reboot") {
    // A restart is optional on the done page (the weak "Restart PC" button); nothing is said about it. The first-run
    // guide still resumes after a sign-in when a real Windows pending-restart signal was reported. The virtual
    // display alone (reasons == "vdd") does not set the RunOnce entry.
    if (S.v.reboot_reasons != "vdd") write_restart_resume();
    S.v.opening = false;
    S.v.panel_failed = false;
    go(pg::done);
  }
  else if (state == "component-failed") {
    S.v.failure = fail_kind::driver;
    go(pg::failed);
  }
  else if (state == "service-failed") {
    S.v.failure = fail_kind::service;
    go(pg::failed);
  }
  else if (state == "cancelled") {
    S.v.uac_cancelled = true;
    go(pg::welcome);
  }
  else {
    S.v.failure = fail_kind::generic;
    go(pg::failed);
  }
}

void
poll_run() {
  if (S.v.page != pg::uac && S.v.page != pg::installing) return;
  const std::string events = read_file(g_run.events);
  if (!events.empty()) {
    const bool changed = events.size() != S.events_size;
    S.events_size = events.size();
    apply_events(S.v, events);
    if (S.v.page == pg::uac) {
      S.v.page = pg::installing;
      begin_transition(S.drawn_v.page, pg::installing);
    }
    if (changed) S.dirty = true;
  }
  std::string result = read_file(g_run.result);
  DWORD exit_code = 0;
  bool exited = false;
  if (result.empty() && S.worker && WaitForSingleObject(S.worker, 0) == WAIT_OBJECT_0) {
    GetExitCodeProcess(S.worker, &exit_code);
    exited = true;
    result = read_file(g_run.result);  // the result may land just before the process ends
  }
  if (!result.empty() || exited) {
    finish_run(result, exit_code);
    return;
  }
}

void
click(int id) {
  if (id >= kTileBase && id < kTileBase + kTileCount) {
    if (component_min_build(id - kTileBase) > windows_build()) return;  // not offered on this Windows (os_limits)
    S.v.tiles[id - kTileBase] = !S.v.tiles[id - kTileBase];
    refresh();
    return;
  }
  if (id == kSelectAll) {
    bool all = true;
    for (int i = 0; i < kTileCount; ++i) all = all && S.v.tiles[i];
    for (int i = 0; i < kTileCount; ++i) S.v.tiles[i] = !all;
    refresh();
    return;
  }
  switch (S.v.page) {
    case pg::welcome:
      if (id == kBtnPrimary)
        begin_install();
      else if (id == kBtnSecond)
        go(pg::components);
      else if (id == kBtnThird)
        DestroyWindow(S.window);
      break;
    case pg::components:
      if (id == kBtnPrimary)
        begin_install();
      else if (id == kBtnSecond)
        go(S.v.maintenance ? pg::maintenance : pg::welcome);
      break;
    case pg::maintenance:
      if (id == kBtnPrimary)
        begin_install();
      else if (id == kBtnSecond)
        go(pg::components);
      else if (id == kBtnThird) {
        if (S.uninstall_string.empty() || !installer_maintenance::start_uninstaller(S.uninstall_string))
          return;
        DestroyWindow(S.window);
      }
      break;
    case pg::installing:
      if (id == kBtnPrimary) ShowWindow(S.window, SW_MINIMIZE);
      break;
    case pg::failed:
      if (id == kBtnPrimary) {
        begin_install();
      }
      else if (id == kBtnSecond) {
        copy_details();
      }
      else if (id == kBtnThird) {
        ShellExecuteW(nullptr, L"open", g_run.directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      }
      break;
    case pg::done:
      if (id == kBtnPrimary && S.v.webview2_missing) {
        ShellExecuteW(nullptr, L"open", L"https://localhost:47990", nullptr, nullptr, SW_SHOWNORMAL);
        break;
      }
      if (id == kBtnPrimary) {
        S.v.panel_failed = !launch_panel();
        S.v.opening = !S.v.panel_failed;
        S.opening_since = GetTickCount64();
        go(pg::done);
      }
      else {
        restart_windows();  // the weak button; Windows asks open apps to close, and the user can cancel there
      }
      break;
    default:
      break;
  }
}

void
draw_control(const DRAWITEMSTRUCT &item) {
  // Buttons remain native child controls for keyboard and screen-reader exposure. Their
  // pixels are composed by the parent Direct2D surface in render_frame().
  (void) item;
}

// Scroll the components list so row `i` (and its group header) is fully in view.
void
reveal_row(const int i) {
  const float top = S.row_y[i];
  const float bottom = (i == 0 || i == 7 ? S.row_y[i] + 30 + S.row_h : S.row_y[i] + S.row_h);
  if (top < S.scroll_target)
    S.scroll_target = top;
  else if (bottom > S.scroll_target + S.view_h)
    S.scroll_target = bottom - S.view_h;
  S.scroll_target = std::clamp(S.scroll_target, 0.f, S.max_scroll);
}

void
focus_row(const int i) {
  reveal_row(i);
  refresh();
  SetFocus(S.controls[3 + i]);
}

// Up/Down/Home/End move between rows (and scroll them into view); Page Up/Down scroll by a page.
bool
list_key(const WPARAM key) {
  if (S.v.page != pg::components) return false;
  const HWND focus = GetFocus();
  int row = -1;
  for (int i = 0; i < kTileCount; ++i)
    if (S.controls[3 + i] == focus) row = i;
  if (key == VK_PRIOR || key == VK_NEXT) {
    S.scroll_target = std::clamp(S.scroll_target + (key == VK_NEXT ? 1 : -1) * (S.view_h - S.row_h), 0.f, S.max_scroll);
    refresh();
    return true;
  }
  if (row < 0 && key != VK_HOME && key != VK_END) return false;
  int target = row;
  if (key == VK_UP)
    target = std::max(0, row - 1);
  else if (key == VK_DOWN)
    target = std::min(kTileCount - 1, row + 1);
  else if (key == VK_HOME)
    target = 0;
  else if (key == VK_END)
    target = kTileCount - 1;
  else
    return false;
  focus_row(target);
  return true;
}

// Keep the window on the screen at 150 % and 200 % scaling: never larger than the work area.
float
fitting_scale(const float wanted) {
  RECT work {};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const float by_width = static_cast<float>(work.right - work.left) / (kW + 16);
  const float by_height = static_cast<float>(work.bottom - work.top) / (kH + 48);
  return std::max(1.f, std::min(wanted, std::min(by_width, by_height)));
}

void
fit_window(HWND window, bool center) {
  using adjust_fn = BOOL(WINAPI *)(LPRECT, DWORD, BOOL, DWORD, UINT);
  static adjust_fn adjust = proc<adjust_fn>(L"user32.dll", "AdjustWindowRectExForDpi");
  RECT rect { 0, 0, static_cast<LONG>(kW * S.scale), static_cast<LONG>(kH * S.scale) };
  const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
  const DWORD ex = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
  if (adjust)
    adjust(&rect, style, FALSE, ex, static_cast<UINT>(S.scale * 96));
  else
    AdjustWindowRectEx(&rect, style, FALSE, ex);
  const int w = rect.right - rect.left, h = rect.bottom - rect.top;
  RECT work {};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  RECT now {};
  GetWindowRect(window, &now);
  const int x = center ? work.left + (work.right - work.left - w) / 2 : now.left;
  const int y = center ? work.top + (work.bottom - work.top - h) / 2 : now.top;
  SetWindowPos(window, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

WNDPROC g_button_proc = nullptr;

// Owner-drawn rows: draw_control paints every pixel, so the BUTTON class erase (its light
// background brush) is only a flash before WM_DRAWITEM. Skip it.
LRESULT CALLBACK
row_button_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_ERASEBKGND) return 1;
  // The buttons are child windows for the keyboard and screen readers only; their pixels are the parent's Direct2D
  // frame (the parent has no WS_CLIPCHILDREN, so it draws through them). A child that painted nothing over a clipped
  // parent was a see-through hole: it must not paint at all, only validate.
  if (message == WM_PAINT) {
    PAINTSTRUCT paint {};
    BeginPaint(window, &paint);
    EndPaint(window, &paint);
    return 0;
  }
  // The rows and buttons are child windows, so the pointer events arrive here and not at the page: they drive
  // the hover and press state layers (animated by the frame loop, 120 ms / 80 ms).
  const int slot = slot_of_id(GetDlgCtrlID(window));
  switch (message) {
    case WM_MOUSEMOVE:
      if (slot >= 0 && S.hover_slot != slot) {
        S.hover_slot = slot;
        S.dirty = true;
        TRACKMOUSEEVENT track { sizeof(track), TME_LEAVE, window, 0 };
        TrackMouseEvent(&track);
      }
      break;
    case WM_MOUSELEAVE:
      if (S.hover_slot == slot) {
        S.hover_slot = -1;
        S.dirty = true;
      }
      break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
      S.focus_visible = false;
      S.dirty = true;
      if (slot >= 0) {
        S.press_slot = slot;
        S.dirty = true;
      }
      break;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
      if (S.press_slot == slot) {
        S.press_slot = -1;
        S.dirty = true;
      }
      break;
    case WM_ENABLE:
      S.dirty = true;
      break;
  }
  return CallWindowProcW(g_button_proc, window, message, wparam, lparam);
}

LRESULT CALLBACK
window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_NCCALCSIZE:
      // Keep the client area full size; the title area below is a composed control.
      return 0;
    case WM_NCHITTEST: {
      POINT point { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
      ScreenToClient(window, &point);
      RECT client {};
      GetClientRect(window, &client);
      if (point.y >= 0 && point.y < 40) {
        if (point.x >= client.right - 80) return HTCLIENT;
        return HTCAPTION;
      }
      return HTCLIENT;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
      S.focus_visible = false;
      S.dirty = true;
      break;
    case WM_LBUTTONUP: {
      RECT client {};
      GetClientRect(window, &client);
      const int x = GET_X_LPARAM(lparam), y = GET_Y_LPARAM(lparam);
      if (y < 40 && x >= client.right - 80 && x < client.right - 40) {
        ShowWindow(window, SW_MINIMIZE);
        return 0;
      }
      if (y < 40 && x >= client.right - 40) {
        SendMessageW(window, WM_CLOSE, 0, 0);
        return 0;
      }
      break;
    }
    case WM_GETMINMAXINFO: {
      auto *info = reinterpret_cast<MINMAXINFO *>(lparam);
      RECT work {};
      SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
      const int width = static_cast<int>(kW * S.scale), height = static_cast<int>(kH * S.scale);
      info->ptMinTrackSize = { width, height };
      info->ptMaxTrackSize = { width, height };
      (void) work;
      return 0;
    }
    case WM_SYSKEYDOWN:
      if (wparam == VK_SPACE) {
        HMENU menu = GetSystemMenu(window, FALSE);
        POINT point {};
        GetCursorPos(&point);
        TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, point.x, point.y, 0, window, nullptr);
        return 0;
      }
      break;
    case WM_CREATE: {
      S.window = window;
      S.start_tick = GetTickCount64();
      S.start_s = now_s();
      for (int slot = 0; slot < kSlots; ++slot) {
        const int id = slot_id(slot);
        S.controls[slot] = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, window,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        const auto previous = reinterpret_cast<WNDPROC>(
          SetWindowLongPtrW(S.controls[slot], GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(row_button_proc)));
        if (!g_button_proc) g_button_proc = previous;
      }
      S.title_buttons[0] = CreateWindowW(L"BUTTON", L"Minimize", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTitleMinimise)), nullptr, nullptr);
      S.title_buttons[1] = CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTitleClose)), nullptr, nullptr);
      for (HWND title_button : S.title_buttons) {
        const auto previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(title_button, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(row_button_proc)));
        if (!g_button_proc) g_button_proc = previous;
      }
      return 0;
    }
    case WM_COMMAND:
      if (HIWORD(wparam) == BN_CLICKED) {
        if (LOWORD(wparam) == kTitleMinimise)
          ShowWindow(window, SW_MINIMIZE);
        else if (LOWORD(wparam) == kTitleClose)
          SendMessageW(window, WM_CLOSE, 0, 0);
        else
          click(LOWORD(wparam));
      }
      return 0;
    case WM_DRAWITEM:
      draw_control(*reinterpret_cast<DRAWITEMSTRUCT *>(lparam));
      return TRUE;
    case WM_TIMER:
      if (wparam == kTimer) {
        poll_run();
        if (S.v.page != pg::uac && S.v.page != pg::installing) KillTimer(window, kTimer);
      }
      return 0;
    case WM_DPICHANGED: {
      S.scale = fitting_scale(static_cast<float>(HIWORD(wparam)) / 96.f);
      const RECT *suggested = reinterpret_cast<RECT *>(lparam);
      SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
        suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
      release_target();
      S.tcache.clear();
      refresh();
      return 0;
    }
    case WM_MOUSEWHEEL:
      if (S.v.page == pg::components && S.max_scroll > 0) {
        // Preserve partial high-resolution wheel deltas and ease toward the target.
        S.scroll_target -= static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * S.row_h;
        S.scroll_target = std::clamp(S.scroll_target, 0.f, S.max_scroll);
        refresh();
        return 0;
      }
      return DefWindowProcW(window, message, wparam, lparam);
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      refresh_environment();
      refresh();
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      // The window is validated and one frame is requested; the frame loop draws it (a second draw here is what doubled the cost).
      PAINTSTRUCT paint {};
      BeginPaint(window, &paint);
      EndPaint(window, &paint);
      S.dirty = true;
      return 0;
    }
    case WM_SIZE:
    case WM_SHOWWINDOW: {
      S.dirty = true;
      return DefWindowProcW(window, message, wparam, lparam);
    }
    case WM_CLOSE:
      // Once the elevated worker runs, the window can be hidden but not abandoned mid-install.
      if (S.v.page == pg::uac || S.v.page == pg::installing) {
        ShowWindow(window, SW_MINIMIZE);
        return 0;
      }
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      KillTimer(window, kTimer);
      release_target();
      S.tcache.clear();
      if (S.brand_source) {
        S.brand_source->Release();
        S.brand_source = nullptr;
      }
      if (S.wic) {
        S.wic->Release();
        S.wic = nullptr;
      }
      if (S.write) {
        S.write->Release();
        S.write = nullptr;
      }
      if (S.d2d) {
        S.d2d->Release();
        S.d2d = nullptr;
      }
      CoUninitialize();
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

WPARAM g_exit_code = 0;

// One message: the list keys and the Return/Escape shortcuts first, then the normal dispatch.
void
handle_message(MSG &message) {
  if (message.message == WM_KEYDOWN &&
      (message.wParam == VK_TAB || message.wParam == VK_UP || message.wParam == VK_DOWN || message.wParam == VK_LEFT || message.wParam == VK_RIGHT ||
        message.wParam == VK_HOME || message.wParam == VK_END || message.wParam == VK_PRIOR || message.wParam == VK_NEXT || message.wParam == VK_SPACE ||
        message.wParam == VK_RETURN)) {
    S.focus_visible = true;
    S.dirty = true;
  }
  if (message.message == WM_KEYDOWN &&
      (message.wParam == VK_UP || message.wParam == VK_DOWN || message.wParam == VK_HOME || message.wParam == VK_END ||
        message.wParam == VK_PRIOR || message.wParam == VK_NEXT) &&
      list_key(message.wParam)) {
    return;
  }
  if (message.message == WM_KEYDOWN && (message.wParam == VK_RETURN || message.wParam == VK_ESCAPE)) {
    if (message.wParam == VK_RETURN) {
      const int id = GetDlgCtrlID(message.hwnd);
      const bool ours = GetParent(message.hwnd) == S.window && id != 0;
      if (ours && IsWindowEnabled(message.hwnd))
        click(id);
      else
        click(kBtnPrimary);
    }
    else {
      switch (S.v.page) {
        case pg::welcome:
          click(kBtnThird);
          break;
        case pg::components:
          click(kBtnSecond);
          break;
        case pg::done:
          DestroyWindow(S.window);  // Escape closes the window; it never restarts the PC
          break;
        default:
          break;
      }
    }
    return;
  }
  if (IsDialogMessageW(S.window, &message)) return;
  TranslateMessage(&message);
  DispatchMessageW(&message);
}

int
bench_main();

int
shell_main() {
  using ctx_fn = BOOL(WINAPI *)(HANDLE);
  static ctx_fn set_dpi = proc<ctx_fn>(L"user32.dll", "SetProcessDpiAwarenessContext");
  if (set_dpi) set_dpi(reinterpret_cast<HANDLE>(-4));  // per-monitor v2
  {
    // 1 ms timer resolution: the pacing fallback sleeps, and a default 15.6 ms tick would turn a 15 ms sleep into 31 ms.
    using period_fn = UINT(WINAPI *)(UINT);
    static period_fn begin_period = proc<period_fn>(L"winmm.dll", "timeBeginPeriod");
    if (begin_period) begin_period(1);
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &S.d2d);
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(&S.write));
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&S.wic));
  g_run.self = module_path();
  load_brand();
  if (!installer_maintenance::populate_view(S.v, &S.uninstall_string)) {
    const std::wstring dir = install_dir();
    S.v.upgrade = !dir.empty() && GetFileAttributesW((dir + L"\\sunshine.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
  }
  apply_os_limits(S.v);
  refresh_environment();
  WNDCLASSW window_class {};
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  RegisterClassW(&window_class);
  HWND window = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, kWindowTitle,
    WS_OVERLAPPED | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
    nullptr, nullptr, window_class.hInstance, nullptr);
  if (!window) return 1;
  S.scale = fitting_scale(window_scale(window));
  refresh_environment();
  fit_window(window, true);
  if (g_bench) {
    SetWindowPos(window, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(window, SW_SHOWNOACTIVATE);
  }
  flush_now();
  {
    // The display's refresh interval, for the pacing fallback.
    using timing_fn = HRESULT(WINAPI *)(HWND, DWM_TIMING_INFO *);
    static timing_fn get_timing = proc<timing_fn>(L"dwmapi.dll", "DwmGetCompositionTimingInfo");
    DWM_TIMING_INFO info {};
    info.cbSize = sizeof(info);
    if (get_timing && SUCCEEDED(get_timing(nullptr, &info)) && info.rateRefresh.uiNumerator > 0 && info.rateRefresh.uiDenominator > 0)
      S.refresh_s = static_cast<double>(info.rateRefresh.uiDenominator) / static_cast<double>(info.rateRefresh.uiNumerator);
    if (S.refresh_s < 1.0 / 400.0 || S.refresh_s > 1.0 / 20.0) S.refresh_s = 1.0 / 60.0;
  }
  if (!g_bench) ShowWindow(window, SW_SHOW);
  UpdateWindow(window);
  apply_page_focus();
  if (!g_selftest_out.empty()) return selftest_window(window);
  if (g_bench) {
    const int result = bench_main();
    DestroyWindow(window);
    return result;
  }
  KillTimer(window, kTimer);
  // The frame loop. Idle: GetMessage blocks, no wakeups at all. While anything moves (a page change, a hover
  // layer, a scroll glide, the progress spinner) it draws one frame per display refresh: EndDraw of the window
  // target returns at the vertical blank, and pace() covers the cases where it does not. Messages are always
  // served first, so input is never starved by drawing.
  for (;;) {
    MSG message {};
    if (frame_wanted()) {
      bool quit = false;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
          quit = true;
          break;
        }
        handle_message(message);
      }
      if (quit) {
        g_exit_code = message.wParam;
        break;
      }
      if (frame_wanted()) present_frame();
    }
    else {
      const BOOL got = GetMessageW(&message, nullptr, 0, 0);
      if (got <= 0) {
        g_exit_code = message.wParam;
        break;
      }
      handle_message(message);
      if (S.dirty) present_frame();
    }
  }
  if (S.worker) CloseHandle(S.worker);
  if (!g_run.worker_copy.empty()) DeleteFileW(g_run.worker_copy.c_str());
  return static_cast<int>(g_exit_code);
}

void
pump_bench_for(DWORD milliseconds) {
  // The same loop as the window's, for a fixed time: messages first, then a paced frame when something moves.
  const ULONGLONG until = GetTickCount64() + milliseconds;
  while (GetTickCount64() < until) {
    MSG message {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) return;
      handle_message(message);
    }
    if (frame_wanted())
      present_frame();
    else
      Sleep(2);
  }
}

int
bench_main() {
  const pg pages[] = { pg::welcome, pg::components, pg::uac, pg::installing, pg::failed, pg::done };
  for (const pg page : pages) {
    if (page == pg::installing) S.v.last_detail = d_files;
    go(page);
    pump_bench_for(360);
  }
  go(pg::components);
  S.scroll_target = S.max_scroll;
  pump_bench_for(700);
  S.scroll_target = 0;
  pump_bench_for(700);
  for (int i = 0; i < kTileCount; ++i) {
    S.hover_slot = 3 + i;
    S.dirty = true;
    pump_bench_for(160);
  }
  S.hover_slot = -1;
  S.dirty = true;
  pump_bench_for(160);
  const auto stats = [](std::vector<double> v, double refresh_ms, const char *prefix) {
    std::sort(v.begin(), v.end());
    const auto pct = [&](double p) { return v.empty() ? 0.0 : v[static_cast<size_t>(p * (v.size() - 1))]; };
    size_t slow = 0;
    for (const double x : v)
      if (x > refresh_ms * 1.5) ++slow;
    const std::string k(prefix);
    return "\"" + k + "frames\":" + std::to_string(v.size()) + ",\"" + k + "fps\":" + std::to_string(v.empty() ? 0.0 : 1000.0 / pct(0.5)) + ",\"" + k +
           "p50_ms\":" + std::to_string(pct(0.50)) + ",\"" + k + "p95_ms\":" + std::to_string(pct(0.95)) + ",\"" + k + "p99_ms\":" +
           std::to_string(pct(0.99)) + ",\"" + k + "max_ms\":" + std::to_string(v.empty() ? 0.0 : v.back()) + ",\"" + k + "over_1_5x_refresh\":" + std::to_string(slow);
  };
  FILETIME creation {}, exit {}, kernel {}, user {};
  GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
  ULARGE_INTEGER kernel_ticks {}, user_ticks {};
  kernel_ticks.LowPart = kernel.dwLowDateTime;
  kernel_ticks.HighPart = kernel.dwHighDateTime;
  user_ticks.LowPart = user.dwLowDateTime;
  user_ticks.HighPart = user.dwHighDateTime;
  const double cpu_ms = static_cast<double>(kernel_ticks.QuadPart + user_ticks.QuadPart) / 10000.0;
  const double refresh_ms = S.refresh_s * 1000.0;
  // "render_*": cost of drawing and presenting one frame. "interval_*": time between two presented frames (what the eye sees).
  const std::string json = "{" + stats(S.bench_frame_ms, refresh_ms, "render_") + "," + stats(S.bench_interval_ms, refresh_ms, "interval_") +
                           ",\"refresh_ms\":" + std::to_string(refresh_ms) + ",\"target_is_window\":" + (S.bench_bitmap ? "false" : "true") +
                           ",\"cpu_ms\":" + std::to_string(cpu_ms) + "}\n";
  const std::wstring output = g_bench_output.empty() ? L"installer-bench.json" : g_bench_output;
  return write_text_atomic(output, json) ? 0 : 4;
}

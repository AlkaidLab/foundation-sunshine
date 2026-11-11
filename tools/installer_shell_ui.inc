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

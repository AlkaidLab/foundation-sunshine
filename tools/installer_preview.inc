// ================================================================ preview renderer

bool
save_png(IWICImagingFactory *wic, IWICBitmap *bitmap, const std::wstring &path) {
  IWICStream *stream = nullptr;
  IWICBitmapEncoder *encoder = nullptr;
  IWICBitmapFrameEncode *frame = nullptr;
  bool ok = false;
  if (wic && SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
      SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
      SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr))) {
    UINT width = 0, height = 0;
    bitmap->GetSize(&width, &height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    ok = SUCCEEDED(frame->SetSize(width, height));
    ok = ok && SUCCEEDED(frame->SetPixelFormat(&format));
    ok = ok && SUCCEEDED(frame->WriteSource(bitmap, nullptr));
    ok = ok && SUCCEEDED(frame->Commit());
    ok = ok && SUCCEEDED(encoder->Commit());
  }
  if (frame) frame->Release();
  if (encoder) encoder->Release();
  if (stream) stream->Release();
  return ok;
}

std::string
ev(const char *phase, const char *state, int code = 0, const char *key = "x") {
  return std::string("{\"schema\":1,\"run\":\"fx\",\"seq\":1,\"phase\":\"") + phase + "\",\"state\":\"" + state +
         "\",\"result_code\":" + std::to_string(code) + ",\"message_key\":\"" + key + "\"}\n";
}

struct scenario {
  const char *name;
  pg page;
  std::string events;
  void (*tweak)(view &);
  float scale = 1.f;
  bool light_only = false;
};

int
preview_main(const std::wstring &out_dir) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &S.d2d);
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(&S.write));
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&S.wic));
  load_brand();
  if (!S.d2d || !S.write || !S.wic || !S.brand_source) return 2;
  CreateDirectoryW(out_dir.c_str(), nullptr);

  const std::string start = ev("worker", "running") + ev("engine", "running") + ev("wizard", "running");
  const std::string prep = start + ev("prepare", "running");
  const std::string files = prep + ev("install", "running") + ev("config", "running");
  const std::string vdd = files + ev("install-vdd", "running");
  const std::string vdd_done = files + ev("install-vdd", "done");
  const std::string pad = vdd_done + ev("gamepad", "running");
  const std::string usb = vdd_done + ev("gamepad", "done") + ev("usbip", "running");
  const std::string net = vdd_done + ev("gamepad", "done") + ev("usbip", "done") + ev("vmouse", "done") + ev("firewall", "running");
  const std::string svc = net + ev("firewall", "done") + ev("service", "running");
  const std::string all_done = svc + ev("service", "done") + ev("autostart", "done") + ev("reboot", "done", 0, "none") + ev("install", "done") + ev("installer", "done");

  const std::vector<scenario> scenarios = {
    { "welcome", pg::welcome, "", nullptr },
    { "welcome-tab", pg::welcome, "", nullptr },  // the same page after a Tab: the focus ring is shown
    { "welcome-upgrade", pg::welcome, "", [](view &v) { v.upgrade = true; } },
    { "welcome-uac-cancelled", pg::welcome, "", [](view &v) { v.uac_cancelled = true; } },
    { "maintenance-newer", pg::maintenance, "", [](view &v) {
        v.maintenance = true; v.has_uninstaller = true; v.package_version = L"2026.1109.120000"; v.installed_version = L"2026.1107.120000";
      } },
    { "maintenance-same", pg::maintenance, "", [](view &v) {
        v.maintenance = true; v.has_uninstaller = true; v.package_version = L"2026.1109.120000"; v.installed_version = L"2026.1109.120000"; v.maintenance_same = true;
      } },
    { "maintenance-older", pg::maintenance, "", [](view &v) {
        v.maintenance = true; v.has_uninstaller = true; v.package_version = L"2026.1107.120000"; v.installed_version = L"2026.1109.120000"; v.maintenance_downgrade = true;
      } },
    { "maintenance-modify", pg::components, "", [](view &v) {
        v.maintenance = true; v.tiles[2] = false; v.tiles[7] = false;
      } },
    { "maintenance-uninstall", pg::maintenance, "", [](view &v) {
        v.maintenance = true; v.has_uninstaller = true; v.package_version = L"2026.1109.120000"; v.installed_version = L"2026.1107.120000";
      } },
    { "components", pg::components, "", [](view &v) { v.tiles[3] = false; v.tiles[7] = false; } },
    { "components-scrolled", pg::components, "", [](view &v) { v.scroll = 1e6f; } },
    { "components-dpi150", pg::components, "", [](view &v) { v.tiles[3] = false; }, 1.5f, true },
    { "components-dpi200", pg::components, "", [](view &v) { v.scroll = 1e6f; }, 2.0f, true },
    { "uac-wait", pg::uac, "", nullptr },
    { "install-1-prepare", pg::installing, prep, nullptr },
    { "install-2-files", pg::installing, files, nullptr },
    { "install-3-driver-display", pg::installing, vdd, nullptr },
    { "install-3b-driver-gamepad", pg::installing, pad, nullptr },
    { "install-3c-driver-usbip", pg::installing, usb, nullptr },
    { "install-4-network", pg::installing, net, nullptr },
    { "install-4b-service", pg::installing, svc, nullptr },
    { "done-reboot-reported", pg::done, files + ev("install-vdd", "needs-reboot", 3010) + ev("service", "done") + ev("reboot", "done", 3010, "usbip") + ev("installer", "done", 3010), nullptr },
    { "failed-driver", pg::failed, files + ev("install-vdd", "failed", 101) + ev("installer", "done", 101), [](view &v) { v.failure = fail_kind::driver; v.exit_code = 101; } },
    { "failed-service", pg::failed, net + ev("firewall", "done") + ev("service", "failed", 100) + ev("installer", "done", 100), [](view &v) { v.failure = fail_kind::service; v.exit_code = 100; } },
    { "failed-other", pg::failed, prep + ev("install", "running"), [](view &v) { v.failure = fail_kind::generic; v.exit_code = 4; } },
    { "done-opening-panel", pg::done, all_done, [](view &v) { v.opening = true; } },
    { "done", pg::done, all_done, nullptr },
  };
  struct theme {
    const char *name;
    palette p;
  };
  const std::vector<theme> themes = { { "light", light_palette() }, { "dark", dark_palette() } };

  auto render = [&](const scenario &sc, const std::string &file, const palette &pal, bool zh, int index) {
    view v;
    v.zh = zh;
    v.page = sc.page;
    v.animated = false;
    v.t = 0.35;
    apply_events(v, sc.events);
    v.finished = sc.page == pg::failed || sc.page == pg::done;
    if (sc.page == pg::failed && sc.events.find("\"installer\"") == std::string::npos) v.finished = true;
    if (sc.tweak) sc.tweak(v);
    IWICBitmap *bitmap = nullptr;
    ID2D1RenderTarget *target = nullptr;
    ID2D1Bitmap *brand = nullptr;
    const UINT width = static_cast<UINT>(kW * sc.scale), height = static_cast<UINT>(kH * sc.scale);
    if (FAILED(S.wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) return false;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    if (FAILED(S.d2d->CreateWicBitmapRenderTarget(bitmap, props, &target))) {
      bitmap->Release();
      return false;
    }
    target->CreateBitmapFromWicBitmap(S.brand_source, nullptr, &brand);
    target->BeginDraw();
    canvas c(*target, S.write, S.d2d, sc.scale, pal, zh);
    frame_result r = draw_page(c, v, brand);
    if (r.content_bottom - 14 > r.buttons_top - 8) {  // the content runs into the buttons: report, do not hide
      const std::wstring note = out_dir + L"\\overflow.txt";
      const std::string line = file + " content_bottom=" + std::to_string(r.content_bottom) + " buttons_top=" + std::to_string(r.buttons_top) + "\n";
      append_event(note, line);
    }
    for (const auto &b : r.buttons) {
      if (!b.visible) continue;
      c.save();
      c.translate(b.x * sc.scale, b.y * sc.scale);
      btn_state st;
      if (sc.page != pg::components && b.id == kBtnPrimary && b.enabled && std::string(sc.name) == "welcome-tab") st.focus = true;
      draw_button(c, b, st, pal.card);
      c.restore();
    }
    target->EndDraw();
    wchar_t name[256];
    swprintf_s(name, L"%02d-%hs.png", index, file.c_str());
    const std::wstring path = out_dir + L"\\" + name;
    const bool saved = save_png(S.wic, bitmap, path);
    if (brand) brand->Release();
    target->Release();
    bitmap->Release();
    return saved;
  };

  int failed = 0;
  for (size_t i = 0; i < scenarios.size(); ++i) {
    for (const auto &t : themes) {
      for (const bool zh : { true, false }) {
        if (scenarios[i].light_only && !zh) continue;
        const std::string file = std::string(scenarios[i].name) + "-" + t.name + (zh ? "-zh" : "-en");
        if (!render(scenarios[i], file, t.p, zh, static_cast<int>(i) + 1))
          ++failed;
      }
    }
  }
  // High contrast (Windows "High Contrast Black" roles) for the two pages that matter most.
  const palette hc = hc_palette(0x000000, 0xffffff, 0x1aebff, 0x000000, 0x3ff23f);
  for (size_t i = 0; i < scenarios.size(); ++i) {
    const std::string n = scenarios[i].name;
    if (n != "welcome" && n != "components" && n != "install-3-driver-display" && n != "failed-driver") continue;
    for (const bool zh : { true, false }) {
      const std::string file = n + "-contrast" + (zh ? "-zh" : "-en");
      if (!render(scenarios[i], file, hc, zh, static_cast<int>(i) + 1))
        ++failed;
    }
  }
  return failed == 0 ? 0 : 3;
}

// --strip DIR: one PNG per instant of a page change (0/50/.../300 ms), composed by the same code as the window
// (compose_transition), at progress computed from the clock value instead of the clock. Touches nothing but DIR.
int
strip_main(const std::wstring &out_dir) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &S.d2d);
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(&S.write));
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&S.wic));
  load_brand();
  if (!S.d2d || !S.write || !S.wic || !S.brand_source) return 2;
  CreateDirectoryW(out_dir.c_str(), nullptr);
  const std::string start = ev("worker", "running") + ev("engine", "running") + ev("wizard", "running");
  const std::string svc = start + ev("prepare", "done") + ev("install", "done") + ev("install-vdd", "done") + ev("gamepad", "done") + ev("usbip", "done") +
                          ev("vmouse", "done") + ev("firewall", "done") + ev("service", "running");
  struct step {
    const char *name;
    pg from, to;
    std::string from_events, to_events;
  };
  const std::vector<step> steps = {
    { "welcome-to-components", pg::welcome, pg::components, "", "" },
    { "components-to-welcome-back", pg::components, pg::welcome, "", "" },
    { "uac-to-installing", pg::uac, pg::installing, "", start },
    { "installing-to-done-crossfade", pg::installing, pg::done, svc, svc + ev("service", "done") + ev("installer", "done") },
  };
  const int times_ms[] = { 0, 50, 100, 150, 200, 250, 300 };
  S.scale = 1.f;
  S.frame_w = kW;
  S.frame_h = kH;
  int failed = 0, index = 0;
  for (const bool dark : { false, true }) {
    S.pal = dark ? dark_palette() : light_palette();
    for (const auto &st : steps) {
      if (dark && std::string(st.name) != "welcome-to-components" && std::string(st.name) != "uac-to-installing") continue;
      ++index;
      for (const int ms : times_ms) {
        IWICBitmap *bitmap = nullptr;
        if (FAILED(S.wic->CreateBitmap(kW, kH, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) return 3;
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        ID2D1RenderTarget *target = nullptr;
        if (FAILED(S.d2d->CreateWicBitmapRenderTarget(bitmap, props, &target))) return 3;
        S.target = target;
        if (S.brand) {
          S.brand->Release();
          S.brand = nullptr;
        }
        target->CreateBitmapFromWicBitmap(S.brand_source, nullptr, &S.brand);
        view from, to;
        for (view *v : { &from, &to }) {
          v->zh = true;
          v->animated = true;
          v->t = 0.35;
        }
        from.page = st.from;
        to.page = st.to;
        apply_events(from, st.from_events);
        apply_events(to, st.to_events);
        from.finished = from.page == pg::done;
        to.finished = to.page == pg::done;
        S.drawn_v = from;
        S.v = to;
        begin_transition(st.from, st.to);
        target->BeginDraw();
        {
          canvas c(*target, S.write, S.d2d, 1.f, S.pal, true, &S.tcache);
          if (ms >= 300 || !S.tr_on)
            draw_scene(*target, c, S.v, false);
          else
            compose_transition(*target, c, ms / 1000.0);
          c.draw_brand(S.brand, 12, 8, 24, 24);
          c.text(44, 11, 300, kWindowTitle, 13, 1, S.pal.on_surface);
        }
        target->EndDraw();
        wchar_t name[256];
        swprintf_s(name, L"strip-%02d-%hs-%s-%03dms.png", index, st.name, dark ? L"dark" : L"light", ms);
        if (!save_png(S.wic, bitmap, out_dir + L"\\" + name)) ++failed;
        release_transition();
        if (S.brand) {
          S.brand->Release();
          S.brand = nullptr;
        }
        target->Release();
        S.target = nullptr;
        bitmap->Release();
      }
    }
  }
  return failed == 0 ? 0 : 4;
}

// --selftest-window OUT: the real native window (not a preview render), each page shown and its frame presented, then
// captured from the window itself with PrintWindow(PW_RENDERFULLCONTENT) (installer-ms-printwindow-20261110) to
// OUT-<page>.png. A see-through child control shows up here as a hole. Exit 0 when all five pages were saved.
int
selftest_window(HWND window) {
  struct shot {
    pg page;
    const wchar_t *name;
  };
  const shot shots[] = { { S.v.page, L"start" }, { pg::components, L"components" }, { pg::installing, L"installing" }, { pg::done, L"done" }, { pg::failed, L"failed" } };
  int saved = 0;
  for (const auto &s : shots) {
    if (s.page == pg::installing)
      apply_events(S.v, "{\"phase\":\"worker\",\"state\":\"running\"}\n{\"phase\":\"install\",\"state\":\"running\"}\n");
    if (s.page == pg::failed) S.v.failure = fail_kind::driver;
    go(s.page);
    const double until = now_s() + 0.9;
    while (now_s() < until) {
      MSG message;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      if (frame_wanted())
        present_frame();
      else
        Sleep(10);
    }
    RECT rect {};
    GetWindowRect(window, &rect);
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ previous = SelectObject(memory, dib);
    const BOOL printed = PrintWindow(window, memory, 2 /* PW_RENDERFULLCONTENT */);
    SelectObject(memory, previous);
    if (printed && bits) {
      for (int i = 0; i < w * h; ++i) static_cast<BYTE *>(bits)[i * 4 + 3] = 255;
      IWICBitmap *bitmap = nullptr;
      if (SUCCEEDED(S.wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, w * h * 4, static_cast<BYTE *>(bits), &bitmap))) {
        if (save_png(S.wic, bitmap, g_selftest_out + L"-" + s.name + L".png")) ++saved;
        bitmap->Release();
      }
    }
    DeleteObject(dib);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
  }
  DestroyWindow(window);
  return saved == 5 ? 0 : 1;
}

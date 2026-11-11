#pragma once
// Direct2D/DirectWrite Material 3 drawing layer for the installer shell. The same
// logical layout is used by the live HWND target and the WIC preview target.
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
using std::max;
using std::min;

namespace ui {
  constexpr int kW = 820;
  constexpr int kH = 560;
  constexpr float kMargin = 16, kRailW = 244, kGap = 12, kPad = 32;
  constexpr float kCardX = kMargin + kRailW + kGap;
  constexpr float kCardW = kW - kMargin - kCardX;
  constexpr float kCardY = kMargin, kCardH = kH - 2 * kMargin;

  struct Color {
    BYTE a = 255, r = 0, g = 0, b = 0;
    Color() = default;
    Color(BYTE alpha, BYTE red, BYTE green, BYTE blue): a(alpha), r(red), g(green), b(blue) {}
    BYTE
    GetA() const { return a; }
    BYTE
    GetR() const { return r; }
    BYTE
    GetG() const { return g; }
    BYTE
    GetB() const { return b; }
  };
  inline Color
  rgb(unsigned v, int a = 255) { return Color(static_cast<BYTE>(a), static_cast<BYTE>((v >> 16) & 255), static_cast<BYTE>((v >> 8) & 255), static_cast<BYTE>(v & 255)); }
  inline Color
  alpha(Color c, int a) { return Color(static_cast<BYTE>(a), c.r, c.g, c.b); }
  inline D2D1_COLOR_F
  d2d_color(Color c) { return D2D1::ColorF(c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f); }

  struct palette {
    Color page, card, tile, on_surface, on_variant, outline, outline_var;
    Color primary, on_primary, primary_c, on_primary_c, secondary_c, on_secondary_c, tertiary_c, on_tertiary_c;
    Color error, on_error, error_c, on_error_c, success, success_c, on_success_c, warning, warning_c, on_warning_c;
    bool hc = false;
  };
  inline palette
  light_palette() {
    palette p;
    p.page = rgb(0xe1e8ff);
    p.card = rgb(0xf6f6ff);
    p.tile = rgb(0xe1e8ff);
    p.on_surface = rgb(0x1e2e51);
    p.on_variant = rgb(0x4b5b81);
    p.outline = rgb(0x67769e);
    p.outline_var = rgb(0x9dadd7);
    p.primary = rgb(0x005ca8);
    p.on_primary = rgb(0xeef3ff);
    p.primary_c = rgb(0xd8e6ff);
    p.on_primary_c = rgb(0x0c1c30);
    p.secondary_c = rgb(0xd9e2ff);
    p.on_secondary_c = rgb(0x121f3b);
    p.tertiary_c = rgb(0xf8d8ff);
    p.on_tertiary_c = rgb(0x2c1834);
    p.error = rgb(0xb31b25);
    p.on_error = rgb(0xffefee);
    p.error_c = rgb(0xfb5151);
    p.on_error_c = rgb(0x570008);
    p.success = rgb(0x006b57);
    p.success_c = rgb(0xb5eedb);
    p.on_success_c = rgb(0x002019);
    p.warning = rgb(0x715d00);
    p.warning_c = rgb(0xf7e29c);
    p.on_warning_c = rgb(0x221b00);
    return p;
  }
  inline palette
  dark_palette() {
    palette p;
    p.page = rgb(0x1a1111);
    p.card = rgb(0x322827);
    p.tile = rgb(0x3d3232);
    p.on_surface = rgb(0xf0dede);
    p.on_variant = rgb(0xd7c1c1);
    p.outline = rgb(0xa08c8c);
    p.outline_var = rgb(0x524343);
    p.primary = rgb(0xf8b6b6);
    p.on_primary = rgb(0x613334);
    p.primary_c = rgb(0x764546);
    p.on_primary_c = rgb(0xffdbda);
    p.secondary_c = rgb(0x503434);
    p.on_secondary_c = rgb(0xdeb5b5);
    p.tertiary_c = rgb(0xf5c2e7);
    p.on_tertiary_c = rgb(0x603b59);
    p.error = rgb(0xf97758);
    p.on_error = rgb(0x450900);
    p.error_c = rgb(0x85230a);
    p.on_error_c = rgb(0xff9b82);
    p.success = rgb(0x8bd889);
    p.success_c = rgb(0x2e4e2d);
    p.on_success_c = rgb(0xc6edc0);
    p.warning = rgb(0xffb86e);
    p.warning_c = rgb(0x633f15);
    p.on_warning_c = rgb(0xffdcbd);
    return p;
  }
  inline palette
  hc_palette(unsigned window, unsigned text, unsigned highlight, unsigned highlight_text, unsigned gray) {
    palette p;
    const Color w = rgb(window), t = rgb(text), h = rgb(highlight), ht = rgb(highlight_text), g = rgb(gray);
    p.page = p.card = p.tile = w;
    p.on_surface = p.on_variant = p.outline = t;
    p.outline_var = g;
    p.primary = h;
    p.on_primary = ht;
    p.primary_c = p.secondary_c = p.tertiary_c = p.error_c = p.success_c = p.warning_c = w;
    p.on_primary_c = p.on_secondary_c = p.on_tertiary_c = p.on_error_c = p.on_success_c = p.on_warning_c = t;
    p.error = p.success = p.warning = h;
    p.on_error = ht;
    p.hc = true;
    return p;
  }

  // Resources that outlive one frame: text formats, laid-out text and the round-cap stroke
  // style. Laying out the same string at the same width every frame is the largest CPU cost of
  // an immediate-mode page, so layouts are kept until the scale or language changes.
  struct text_cache {
    std::vector<std::pair<int, IDWriteTextFormat *>> formats;
    std::unordered_map<std::wstring, IDWriteTextLayout *> layouts;
    ID2D1StrokeStyle *round = nullptr;
    float s = 0;
    bool zh = false;
    void
    clear() {
      for (auto &item : formats)
        if (item.second) item.second->Release();
      formats.clear();
      for (auto &item : layouts)
        if (item.second) item.second->Release();
      layouts.clear();
    }
    ~text_cache() {
      clear();
      if (round) round->Release();
    }
  };

  struct canvas {
    ID2D1RenderTarget &g;
    IDWriteFactory *write;
    ID2D1Factory *factory;
    float s;
    palette p;
    bool zh;
    text_cache own_cache;
    text_cache &tc;
    ID2D1SolidColorBrush *shared_brush = nullptr;
    int clip_depth = 0;
    D2D1_MATRIX_3X2_F saved_transform = D2D1::Matrix3x2F::Identity();

    canvas(ID2D1RenderTarget &target, IDWriteFactory *dw, ID2D1Factory *df, float scale, const palette &pal, bool chinese, text_cache *cache = nullptr):
        g(target), write(dw), factory(df), s(scale), p(pal), zh(chinese), tc(cache ? *cache : own_cache) {
      // Authority: installer-ms-direct2d-write-20261107 and installer-ms-id2d1devicecontext-20261107.
      // DirectWrite ClearType assumes an opaque, stable display surface. The installer
      // is composed by DWM and previews use alpha, so grayscale is the documented mode.
      g.SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
      g.SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
      if (tc.s != scale || tc.zh != chinese) {
        tc.clear();
        tc.s = scale;
        tc.zh = chinese;
      }
      if (!tc.round && factory) {
        // Round caps and joins: Material icon strokes end in circles, and the joints of the check
        // mark and the 24 arc segments must not show a notch.
        factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                     D2D1_LINE_JOIN_ROUND, 10.f, D2D1_DASH_STYLE_SOLID, 0.f),
          nullptr, 0, &tc.round);
      }
      g.CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &shared_brush);
    }
    ~canvas() {
      if (shared_brush) shared_brush->Release();
    }
    // One brush per frame; callers must not release it.
    ID2D1SolidColorBrush *
    brush(Color c) {
      if (shared_brush) shared_brush->SetColor(d2d_color(c));
      return shared_brush;
    }
    static float
    snap(float v) { return std::floor(v + 0.5f); }
    IDWriteTextFormat *
    format(float px, int weight, int align, bool wrap) {
      const int key = static_cast<int>(px * 10) * 32 + weight * 4 + align * 2 + (wrap ? 1 : 0);
      for (auto &item : tc.formats)
        if (item.first == key) return item.second;
      const wchar_t *family = zh ? L"Microsoft YaHei UI" : L"Segoe UI Variable Text";
      const DWRITE_FONT_WEIGHT fw = weight == 2 ? DWRITE_FONT_WEIGHT_BOLD : weight == 1 ? DWRITE_FONT_WEIGHT_SEMI_BOLD :
                                                                                          DWRITE_FONT_WEIGHT_NORMAL;
      IDWriteTextFormat *out = nullptr;
      if (!write || FAILED(write->CreateTextFormat(family, nullptr, fw, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                      px * s, L"", &out))) {
        if (write) write->CreateTextFormat(L"Segoe UI", nullptr, fw, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px * s, L"", &out);
      }
      if (out) {
        out->SetTextAlignment(align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING :
                                                                                       DWRITE_TEXT_ALIGNMENT_LEADING);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        out->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
      }
      tc.formats.emplace_back(key, out);
      return out;
    }
    // A cached layout; the cache owns it. `vcenter` centres the paragraph in the box height.
    IDWriteTextLayout *
    layout(const std::wstring &t, float w, float h, float px, int weight, int align, bool wrap, bool vcenter) {
      wchar_t head[96];
      swprintf_s(head, L"%d|%d|%d|%d|%d|%d|%d|", static_cast<int>(w * s * 4), static_cast<int>(h * s), static_cast<int>(px * 10), weight, align, wrap ? 1 : 0, vcenter ? 1 : 0);
      const std::wstring key = std::wstring(head) + t;
      auto found = tc.layouts.find(key);
      if (found != tc.layouts.end()) return found->second;
      IDWriteTextFormat *f = format(px, weight, align, wrap);
      IDWriteTextLayout *out = nullptr;
      if (!f || !write || FAILED(write->CreateTextLayout(t.c_str(), static_cast<UINT32>(t.size()), f, w * s, h * s, &out))) return nullptr;
      if (vcenter) out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      if (tc.layouts.size() > 1500) {  // bounded: step counters and the like are few, but never grow without limit
        for (auto &item : tc.layouts)
          if (item.second) item.second->Release();
        tc.layouts.clear();
      }
      tc.layouts[key] = out;
      return out;
    }
    float
    measure_h(float w, const std::wstring &t, float px, int weight) {
      IDWriteTextLayout *l = layout(t, w, 10000, px, weight, 0, true, false);
      if (!l) return px;
      DWRITE_TEXT_METRICS m {};
      l->GetMetrics(&m);
      return m.height / s;
    }
    float
    measure_w(const std::wstring &t, float px, int weight) {
      IDWriteTextLayout *l = layout(t, 10000, 1000, px, weight, 0, false, false);
      if (!l) return 0;
      DWRITE_TEXT_METRICS m {};
      l->GetMetrics(&m);
      return m.width / s;
    }
    float
    text(float x, float y, float w, const std::wstring &t, float px, int weight, Color c, int align = 0) {
      IDWriteTextLayout *l = layout(t, w, 10000, px, weight, align, true, false);
      if (!l) return px;
      // Origin on the pixel grid: grayscale glyph rendering is crisp only when the origin is not fractional.
      if (ID2D1SolidColorBrush *b = brush(c)) g.DrawTextLayout(D2D1::Point2F(snap(x * s), snap(y * s)), l, b);
      DWRITE_TEXT_METRICS m {};
      l->GetMetrics(&m);
      return m.height / s;
    }
    void
    text_center(float x, float y, float w, float h, const std::wstring &t, float px, int weight, Color c) {
      IDWriteTextLayout *l = layout(t, w, h, px, weight, 1, false, true);
      if (!l) return;
      if (ID2D1SolidColorBrush *b = brush(c)) g.DrawTextLayout(D2D1::Point2F(snap(x * s), snap(y * s)), l, b);
    }
    void
    clear(Color c) { g.Clear(d2d_color(c)); }
    D2D1_ROUNDED_RECT
    snapped(float x, float y, float w, float h, float r) const {
      return D2D1::RoundedRect(D2D1::RectF(snap(x * s), snap(y * s), snap((x + w) * s), snap((y + h) * s)), r * s, r * s);
    }
    void
    fill_r(float x, float y, float w, float h, float r, Color c) {
      if (ID2D1SolidColorBrush *b = brush(c)) g.FillRoundedRectangle(snapped(x, y, w, h, r), b);
    }
    void
    stroke_r(float x, float y, float w, float h, float r, Color c, float width) {
      if (ID2D1SolidColorBrush *b = brush(c)) {
        // The stroke is centred on the edge; an odd pixel width needs the half-pixel offset to stay crisp.
        const float wpx = std::max(1.f, snap(width * s));
        const float half = std::fmod(wpx, 2.f) == 1.f ? 0.5f : 0.f;
        D2D1_ROUNDED_RECT rr = snapped(x, y, w, h, r);
        rr.rect.left += half;
        rr.rect.top += half;
        rr.rect.right += half;
        rr.rect.bottom += half;
        g.DrawRoundedRectangle(rr, b, wpx);
      }
    }
    void
    container(float x, float y, float w, float h, float r, Color fill) {
      if (p.hc) {
        fill_r(x, y, w, h, r, p.card);
        stroke_r(x, y, w, h, r, p.outline, 2);
      }
      else
        fill_r(x, y, w, h, r, fill);
    }
    void
    disc(float cx, float cy, float d, Color c) {
      if (ID2D1SolidColorBrush *b = brush(c)) g.FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx * s, cy * s), d * s / 2, d * s / 2), b);
    }
    void
    ring(float cx, float cy, float d, Color c, float width) {
      if (ID2D1SolidColorBrush *b = brush(c)) g.DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx * s, cy * s), d * s / 2, d * s / 2), b, width * s);
    }
    void
    ellipse(float cx, float cy, float rx, float ry, Color c, float width) {
      if (ID2D1SolidColorBrush *b = brush(c)) g.DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx * s, cy * s), rx * s / 2, ry * s / 2), b, width * s);
    }
    void
    line(float x1, float y1, float x2, float y2, Color c, float width) {
      if (ID2D1SolidColorBrush *b = brush(c)) g.DrawLine(D2D1::Point2F(x1 * s, y1 * s), D2D1::Point2F(x2 * s, y2 * s), b, width * s, tc.round);
    }
    void
    dot(float x, float y, float r, Color c) { disc(x, y, r * 2, c); }
    // A true arc path with round caps (one stroke, no per-segment seams).
    void
    arc(float cx, float cy, float d, float start, float sweep, Color c, float width) {
      ID2D1SolidColorBrush *b = brush(c);
      ID2D1PathGeometry *path = nullptr;
      ID2D1GeometrySink *sink = nullptr;
      if (factory && b && SUCCEEDED(factory->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
        const float rad = 3.14159265f / 180.f, r = d * s / 2;
        const float a0 = start * rad, a1 = (start + sweep) * rad;
        sink->BeginFigure(D2D1::Point2F(cx * s + std::cos(a0) * r, cy * s + std::sin(a0) * r), D2D1_FIGURE_BEGIN_HOLLOW);
        // An arc segment spans at most 180 degrees: split in two halves.
        const float mid = (a0 + a1) / 2;
        const D2D1_SIZE_F radius = D2D1::SizeF(r, r);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx * s + std::cos(mid) * r, cy * s + std::sin(mid) * r), radius, 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx * s + std::cos(a1) * r, cy * s + std::sin(a1) * r), radius, 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        g.DrawGeometry(path, b, width * s, tc.round);
      }
      if (sink) sink->Release();
      if (path) path->Release();
    }
    void
    spinner(float cx, float cy, float d, Color c, float width, double t, bool animated) { arc(cx, cy, d, animated ? static_cast<float>(std::fmod(t * 324.0, 360.0)) : 20.f, 270.f, c, width); }
    void
    triangle(float x1, float y1, float x2, float y2, float x3, float y3, Color c) {
      ID2D1SolidColorBrush *b = brush(c);
      ID2D1PathGeometry *path = nullptr;
      ID2D1GeometrySink *sink = nullptr;
      if (factory && b && SUCCEEDED(factory->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
        sink->BeginFigure(D2D1::Point2F(x1 * s, y1 * s), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(x2 * s, y2 * s));
        sink->AddLine(D2D1::Point2F(x3 * s, y3 * s));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        g.FillGeometry(path, b);
      }
      if (sink) sink->Release();
      if (path) path->Release();
    }
    void
    draw_brand(ID2D1Bitmap *brand, float x, float y, float w, float h) {
      // Linear interpolation (bilinear) at the real device size: the source is larger than the 56 px target,
      // so the GPU's filtered minification is what keeps the mark smooth.
      if (brand) g.DrawBitmap(brand, D2D1::RectF(snap(x * s), snap(y * s), snap((x + w) * s), snap((y + h) * s)), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    void
    save() { g.GetTransform(&saved_transform); }
    // Translation in device pixels, rounded: a fractional offset would blur every glyph and edge it carries.
    void
    translate(float x, float y) {
      D2D1_MATRIX_3X2_F t = D2D1::Matrix3x2F::Translation(snap(x), snap(y));
      D2D1_MATRIX_3X2_F cur {};
      g.GetTransform(&cur);
      g.SetTransform(t * cur);
    }
    void
    restore() { g.SetTransform(saved_transform); }
    void
    clip(float x, float y, float w, float h) {
      g.PushAxisAlignedClip(D2D1::RectF(snap(x * s), snap(y * s), snap((x + w) * s), snap((y + h) * s)), D2D1_ANTIALIAS_MODE_ALIASED);
      ++clip_depth;
    }
    void
    reset_clip() {
      if (clip_depth > 0) {
        g.PopAxisAlignedClip();
        --clip_depth;
      }
    }
  };

  enum class ic { check,
    close,
    exclam,
    info,
    restart,
    shield,
    monitor,
    gamepad,
    mouse,
    globe,
    power,
    folder,
    play,
    wrench,
    doc,
    pc };
  inline void
  icon(canvas &c, ic kind, float cx, float cy, float size, Color col, float stroke = 2.f) {
    const float q = size / 24.f;
    auto line = [&](float x1, float y1, float x2, float y2) { c.line(cx + (x1 - 12) * q, cy + (y1 - 12) * q, cx + (x2 - 12) * q, cy + (y2 - 12) * q, col, stroke * q); };
    auto dot = [&](float x, float y, float r) { c.dot(cx + (x - 12) * q, cy + (y - 12) * q, r * q, col); };
    auto rr = [&](float x, float y, float w, float h, float r) { c.stroke_r(cx + (x - 12) * q, cy + (y - 12) * q, w * q, h * q, r * q, col, stroke * q); };
    switch (kind) {
      case ic::check:
        line(5.5f, 12.5f, 10, 17);
        line(10, 17, 18.5f, 7.5f);
        break;
      case ic::close:
        line(6.5f, 6.5f, 17.5f, 17.5f);
        line(17.5f, 6.5f, 6.5f, 17.5f);
        break;
      case ic::exclam:
        line(12, 5.5f, 12, 13.5f);
        dot(12, 18, 1.4f);
        break;
      case ic::info:
        dot(12, 6.8f, 1.4f);
        line(12, 10.5f, 12, 18);
        break;
      case ic::restart:
        c.arc(cx, cy, size * 14.f / 24.f, -50, 290, col, stroke * q);
        c.triangle(cx + (17.8f - 12) * q, cy + (2.8f - 12) * q, cx + (18.6f - 12) * q, cy + (8.6f - 12) * q, cx + (12.8f - 12) * q, cy + (7.4f - 12) * q, col);
        break;
      case ic::shield:
        line(12, 3, 19, 6);
        line(19, 6, 19, 11.5f);
        line(19, 11.5f, 17, 17);
        line(17, 17, 12, 21);
        line(12, 21, 7, 19);
        line(7, 19, 5, 16);
        line(5, 16, 5, 6);
        line(5, 6, 12, 3);
        line(8.8f, 11.8f, 11.2f, 14.2f);
        line(11.2f, 14.2f, 15.4f, 9.6f);
        break;
      case ic::monitor:
        rr(3.5f, 4.5f, 17, 11, 2);
        line(12, 15.5f, 12, 19.5f);
        line(8, 19.5f, 16, 19.5f);
        break;
      case ic::pc:
        rr(3.5f, 4.5f, 17, 11, 2);
        line(12, 15.5f, 12, 19.5f);
        line(8, 19.5f, 16, 19.5f);
        line(8.4f, 10, 10.8f, 12.3f);
        line(10.8f, 12.3f, 15.6f, 7.6f);
        break;
      case ic::gamepad:
        rr(2.8f, 7.2f, 18.4f, 10, 5);
        line(7.5f, 10.2f, 7.5f, 14.2f);
        line(5.5f, 12.2f, 9.5f, 12.2f);
        dot(15.5f, 11.2f, 1.1f);
        dot(18, 13.2f, 1.1f);
        break;
      case ic::mouse:
        rr(7, 3, 10, 18, 5);
        line(12, 3.5f, 12, 9);
        break;
      case ic::globe:
        c.ring(cx, cy, size * 17.f / 24.f, col, stroke * q);
        c.ellipse(cx, cy, size * 8.f / 24.f, size * 17.f / 24.f, col, stroke * q);
        line(3.5f, 12, 20.5f, 12);
        break;
      case ic::power:
        c.arc(cx, cy, size * 14.f / 24.f, -60, 300, col, stroke * q);
        line(12, 3.5f, 12, 11.5f);
        break;
      case ic::folder:
        line(3.5f, 7, 3.5f, 18);
        line(3.5f, 18, 20.5f, 18);
        line(20.5f, 18, 20.5f, 8.5f);
        line(20.5f, 8.5f, 11.5f, 8.5f);
        line(11.5f, 8.5f, 9.5f, 6);
        line(9.5f, 6, 3.5f, 6);
        break;
      case ic::play:
        c.triangle(cx + (8.5f - 12) * q, cy + (6 - 12) * q, cx + (18.5f - 12) * q, cy, cx + (8.5f - 12) * q, cy + (18 - 12) * q, col);
        break;
      case ic::wrench:
        rr(7.5f, 7.5f, 9, 9, 4.5f);
        for (int i = 0; i < 8; ++i) {
          const double a = i * 3.14159265 / 4;
          line(12 + 6.2f * std::cos(a), 12 + 6.2f * std::sin(a), 12 + 8.6f * std::cos(a), 12 + 8.6f * std::sin(a));
        }
        break;
      case ic::doc:
        rr(5.5f, 3.5f, 13, 17, 2);
        line(9, 9, 15, 9);
        line(9, 12.5f, 15, 12.5f);
        line(9, 16, 12.5f, 16);
        break;
    }
  }

  // ---------------------------------------------------------------- events -> view
  enum class sstate { pending,
    running,
    done,
    failed,
    reboot };
  enum class pg { welcome,
    maintenance,
    components,
    uac,
    installing,
    failed,
    done };
  enum class fail_kind { generic,
    driver,
    service };

  constexpr int kGroups = 4;
  enum detail { d_none,
    d_worker,
    d_engine,
    d_wizard,
    d_prepare,
    d_files,
    d_path,
    d_config,
    d_vdd,
    d_gamepad,
    d_usbip,
    d_vmouse,
    d_firewall,
    d_service,
    d_autostart,
    d_permissions,
    d_reboot,
    d_count };

  struct sub {
    int group = -1;
    sstate st = sstate::pending;
  };
  struct view {
    pg page = pg::welcome;
    bool zh = true, upgrade = false, maintenance = false, maintenance_downgrade = false, maintenance_same = false,
         has_uninstaller = false, uac_cancelled = false, opening = false, panel_failed = false;
    std::wstring installed_version, package_version;
    bool tiles[8] = { true, true, true, true, true, true, true, true };  // vdd, gamepad, usbip, vmouse, firewall, autostart, tools, desktopicon
    float scroll = 0;  // components list, logical px
    std::string reboot_reasons;  // from the 'reboot' event: vdd,vigem,usbip,vmouse or none
    fail_kind failure = fail_kind::generic;
    int exit_code = 0;
    double t = 0;  // seconds, drives the spinner
    float row_hover[8] = {}, row_press[8] = {};  // state layer amounts 0..1 of the component rows
    int focus_row = -1;  // row with keyboard focus, drawn with the focus ring
    bool animated = true;
    // reduced from the event stream
    sub subs[d_count] = {};
    int last_detail = d_none;
    bool finished = false;
    bool webview2_missing = false;  // done: the Panel's WebView2 Runtime is absent (offline, bootstrapper failed)
    std::string planned;  // the engine's 'plan' event: comma list of step ids (installer_web.inc names)
    int permille[d_count] = {};  // own progress of a step, 0..1000 ('permille' of its events); -1 = none
  };

  inline int
  detail_group(int d) {
    switch (d) {
      case d_worker:
      case d_engine:
      case d_wizard:
      case d_prepare:
        return 0;
      case d_files:
      case d_path:
      case d_config:
      case d_permissions:
        return 1;
      case d_vdd:
      case d_gamepad:
      case d_usbip:
      case d_vmouse:
        return 2;
      case d_firewall:
      case d_service:
      case d_autostart:
      case d_reboot:
        return 3;
    }
    return -1;
  }
  inline int
  detail_of_phase(const std::string &ph) {
    if (ph == "worker") return d_worker;
    if (ph == "engine") return d_engine;
    if (ph == "wizard") return d_wizard;
    if (ph == "prepare") return d_prepare;
    if (ph == "install") return d_files;
    if (ph == "permissions") return d_permissions;
    if (ph == "path") return d_path;
    if (ph == "config") return d_config;
    if (ph == "install-vdd") return d_vdd;
    if (ph == "gamepad") return d_gamepad;
    if (ph == "usbip") return d_usbip;
    if (ph == "reboot") return d_reboot;
    if (ph == "vmouse") return d_vmouse;
    if (ph == "firewall") return d_firewall;
    if (ph == "service") return d_service;
    if (ph == "autostart") return d_autostart;
    return d_none;
  }
  inline std::string
  json_str(const std::string &line, const char *key) {
    const std::string needle = std::string("\"") + key + "\":\"";
    const size_t b = line.find(needle);
    if (b == std::string::npos) return {};
    const size_t s = b + needle.size(), e = line.find('"', s);
    return e == std::string::npos ? std::string() : line.substr(s, e - s);
  }
  inline long
  json_int(const std::string &line, const char *key, long fallback = 0) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t b = line.find(needle);
    if (b == std::string::npos) return fallback;
    return std::strtol(line.c_str() + b + needle.size(), nullptr, 10);
  }

  // Fold complete lines of events.jsonl into the view. A half line (no newline) is left alone.
  inline void
  apply_events(view &v, const std::string &text) {
    for (auto &x : v.subs) x = { -1, sstate::pending };
    for (int &p : v.permille) p = -1;
    v.planned.clear();
    v.last_detail = d_none;
    v.finished = false;
    size_t pos = 0;
    while (pos < text.size()) {
      const size_t nl = text.find('\n', pos);
      if (nl == std::string::npos) break;
      const std::string line = text.substr(pos, nl - pos);
      pos = nl + 1;
      const std::string phase = json_str(line, "phase"), state = json_str(line, "state");
      if (phase == "installer") {
        v.finished = true;
        continue;
      }
      if (phase == "plan") {
        v.planned = json_str(line, "message_key");
        continue;
      }
      if (phase == "reboot") v.reboot_reasons = json_str(line, "message_key");
      const int d = detail_of_phase(phase);
      if (d == d_none) continue;
      const long permille = json_int(line, "permille", -1);
      if (permille >= 0) v.permille[d] = static_cast<int>(permille > 1000 ? 1000 : permille);
      sstate st = sstate::running;
      if (state == "done")
        st = sstate::done;
      else if (state == "failed")
        st = sstate::failed;
      else if (state == "needs-reboot")
        st = sstate::reboot;
      if (phase == "install" && state == "done") {
        v.subs[d].group = detail_group(d);
        v.subs[d].st = sstate::done;
        continue;
      }
      v.subs[d] = { detail_group(d), st };
      v.last_detail = d;
    }
  }
  inline sstate
  group_state(const view &v, int g) {
    bool seen = false, run = false, fail = false, reb = false;
    for (int d = 1; d < d_count; ++d) {
      if (v.subs[d].group != g) continue;
      seen = true;
      if (v.subs[d].st == sstate::failed) fail = true;
      if (v.subs[d].st == sstate::reboot) reb = true;
      if (v.subs[d].st == sstate::running) run = true;
    }
    if (fail) return sstate::failed;
    if (reb) return sstate::reboot;
    if (!seen) return sstate::pending;
    const bool current = !v.finished && v.last_detail != d_none && detail_group(v.last_detail) == g;
    if (current) return sstate::running;
    (void) run;
    return sstate::done;
  }
  inline int
  current_group(const view &v) {
    if (v.last_detail == d_none) return 0;
    return max(0, detail_group(v.last_detail));
  }

}  // namespace ui

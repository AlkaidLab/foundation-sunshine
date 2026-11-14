#pragma once
#include "installer_d2d.h"

namespace ui {

  // ---------------------------------------------------------------- buttons
  enum class bkind { filled,
    tonal,
    text,
    tile,
    row };
  struct button_spec {
    int id = 0;
    std::wstring text, desc;
    bkind kind = bkind::filled;
    bool enabled = true, checked = false, visible = true;
    float x = 0, y = 0, w = 0, h = 44;
  };
  struct btn_state {
    bool focus = false;
    float hover = 0, press = 0;  // state layer amounts 0..1, animated by the shell (Material 3: 8 % hover, 10-12 % press)
  };

  // Draws one button in its own local coordinates (0,0)-(w,h). `under` is the colour behind it.
  inline void
  draw_button(canvas &c, const button_spec &b, btn_state st, Color under) {
    const palette &p = c.p;
    c.fill_r(-2, -2, b.w + 4, b.h + 4, 0, under);
    Color fill, fg;
    float radius = b.h / 2;
    if (b.kind == bkind::filled) {
      fill = b.enabled ? p.primary : alpha(p.on_surface, 30);
      fg = b.enabled ? p.on_primary : alpha(p.on_surface, 97);
    }
    else if (b.kind == bkind::tonal) {
      fill = b.enabled ? p.secondary_c : alpha(p.on_surface, 30);
      fg = b.enabled ? p.on_secondary_c : alpha(p.on_surface, 97);
    }
    else if (b.kind == bkind::text) {
      fill = Color(0, 0, 0, 0);
      fg = b.enabled ? p.primary : alpha(p.on_surface, 97);
    }
    else {  // tile or list row
      radius = b.kind == bkind::row ? 14 : 18;
      fill = b.checked ? p.primary_c : p.tile;
      fg = b.checked ? p.on_primary_c : p.on_surface;
      if (!b.enabled) fg = alpha(p.on_surface, 140);
    }
    if (p.hc) {
      if (b.kind == bkind::filled && b.enabled) {
        fill = p.primary;
        fg = p.on_primary;
      }
      else {
        fill = p.card;
        fg = b.enabled ? p.on_surface : p.outline_var;
      }
    }
    if (b.kind != bkind::text || p.hc) c.fill_r(0, 0, b.w, b.h, radius, fill);
    if (p.hc && !(b.kind == bkind::filled && b.enabled)) c.stroke_r(0, 0, b.w, b.h, radius, b.enabled ? p.outline : p.outline_var, b.kind == bkind::tile && b.checked ? 3.f : 1.5f);
    if (!p.hc && b.enabled) {
      const float layer = max(st.hover, st.press * 1.6f);  // one animated layer: hover first, press deepens it
      if (layer > 0.002f) {
        if (b.kind == bkind::text)
          c.fill_r(0, 0, b.w, b.h, radius, alpha(p.primary, static_cast<int>(24 * std::min(layer, 1.5f) + 0.5f)));
        else
          c.fill_r(0, 0, b.w, b.h, radius, alpha(fg, static_cast<int>(20 * std::min(layer, 1.8f) + 0.5f)));
      }
    }
    if (b.kind == bkind::tile) {
      const float cx = 24, cy = 24;
      if (b.checked) {
        c.disc(cx, cy, 24, p.hc ? p.primary : p.primary);
        icon(c, ic::check, cx, cy, 16, p.on_primary, 2.4f);
      }
      else
        c.ring(cx, cy, 24, p.outline, 2);
      const float tx = 48, tw = b.w - tx - 12;
      c.text(tx, 12, tw, b.text, 14, 1, fg);
      if (!b.desc.empty()) c.text(tx, 12 + 22, tw, b.desc, 12.5f, 0, b.checked && !p.hc ? alpha(p.on_primary_c, 200) : p.on_variant);
    }
    else if (b.kind == bkind::row) {
      const float cy = b.h / 2;
      if (b.checked) {
        c.fill_r(16, cy - 11, 22, 22, 6, p.primary);
        icon(c, ic::check, 27, cy, 16, p.on_primary, 2.4f);
      }
      else {
        c.stroke_r(16, cy - 11, 22, 22, 6, p.outline, 2);
      }
      c.text(52, 8, b.w - 64, b.text, 14.5f, 1, fg);
      c.text(52, 29, b.w - 64, b.desc, 12.5f, 0, b.checked && !p.hc ? alpha(p.on_primary_c, 200) : p.on_variant);
    }
    else {
      c.text_center(0, 0, b.w, b.h, b.text, 15, 1, fg);
    }
    if (st.focus) {
      c.stroke_r(-3, -3, b.w + 6, b.h + 6, radius + 3, p.hc ? p.on_surface : p.primary, 3);
    }
  }

  // ---------------------------------------------------------------- text
  struct fact {
    ic icon;
    Color bg, fg;
    std::wstring title, body;
  };

  inline std::wstring
  Z(const view &v, const wchar_t *zh, const wchar_t *en) { return v.zh ? zh : en; }

  inline std::wstring
  detail_text(const view &v, int d) {
    switch (d) {
      case d_worker:
      case d_engine:
        return Z(v, L"正在准备安装程序", L"Preparing the installer");
      case d_wizard:
        return Z(v, L"正在检查这台电脑", L"Checking this PC");
      case d_prepare:
        return Z(v, L"正在关闭旧版 Sunshine", L"Closing the old Sunshine");
      case d_files:
        return Z(v, L"正在复制程序和资源", L"Copying the program and its files");
      case d_permissions:
        return Z(v, L"正在设置文件权限", L"Setting file permissions");
      case d_path:
        return Z(v, L"正在更新 PATH", L"Updating PATH");
      case d_config:
        return Z(v, L"正在迁移你的设置", L"Moving your settings across");
      case d_vdd:
        return Z(v, L"正在安装基地虚拟显示器", L"Installing Foundation Virtual Display");
      case d_gamepad:
        return Z(v, L"正在安装虚拟手柄 (ViGEmBus)", L"Installing the virtual gamepad (ViGEmBus)");
      case d_usbip:
        return Z(v, L"正在安装 USB/IP (usbip-win2)", L"Installing USB/IP (usbip-win2)");
      case d_vmouse:
        return Z(v, L"正在安装虚拟鼠标", L"Installing the virtual mouse");
      case d_firewall:
        return Z(v, L"正在添加防火墙规则", L"Adding the firewall rules");
      case d_service:
        return Z(v, L"正在安装并验证后台服务", L"Installing and checking the background service");
      case d_autostart:
        return Z(v, L"正在设置开机自启", L"Setting up start with Windows");
      case d_reboot:
        return Z(v, L"正在确认是否需要重启", L"Checking whether a restart is needed");
    }
    return Z(v, L"正在安装", L"Installing");
  }
  inline std::wstring
  group_title(const view &v, int g) {
    switch (g) {
      case 0:
        return Z(v, L"检查电脑", L"Check this PC");
      case 1:
        return Z(v, L"放入程序", L"Copy the program");
      case 2:
        return Z(v, L"安装驱动", L"Install drivers");
      case 3:
        return Z(v, L"网络与服务", L"Network and service");
      default:
        return Z(v, L"开始使用", L"Get started");
    }
  }
  inline std::wstring
  group_sub(const view &v, int g) {
    switch (g) {
      case 0:
        return Z(v, L"安装包与系统", L"Package and system");
      case 1:
        return Z(v, L"程序、资源、管理界面", L"Program, files, panel");
      case 2:
        return Z(v, L"显示器、手柄、鼠标、USB/IP", L"Display, gamepad, mouse, USB/IP");
      case 3:
        return Z(v, L"防火墙、服务、开机自启", L"Firewall, service, autostart");
      default:
        return Z(v, L"首次设置引导", L"First-run guide");
    }
  }
  inline const wchar_t *
  tile_name(const view &v, int i) {
    static const wchar_t *zh[] = { L"基地虚拟显示器 (HDR)", L"虚拟手柄", L"USB/IP", L"虚拟鼠标", L"防火墙规则", L"开机自启", L"诊断工具", L"桌面快捷方式" };
    static const wchar_t *en[] = { L"Foundation Virtual Display (HDR)", L"Virtual gamepad", L"USB/IP", L"Virtual mouse", L"Firewall rules", L"Start with Windows", L"Diagnostic tools", L"Desktop shortcut" };
    return v.zh ? zh[i] : en[i];
  }
  inline const wchar_t *
  tile_desc(const view &v, int i) {
    static const wchar_t *zh[] = { L"没有实体显示器也能串流，支持 HDR", L"Xbox 360 与 DS4 手柄，游戏直接识别", L"usbip-win2，原生 DualSense 需要", L"更稳的绝对定位鼠标", L"让客户端发现并连上这台电脑", L"开机后自动提供串流", L"显示器与音频排查工具", L"在桌面放一个入口" };
    static const wchar_t *en[] = { L"Stream without a monitor, with HDR", L"Xbox 360 and DS4 pads, seen by games", L"usbip-win2, needed for native DualSense", L"Steadier absolute mouse", L"Lets clients find and reach this PC", L"Streaming is ready after boot", L"Display and audio checks", L"An entry on the desktop" };
    return v.zh ? zh[i] : en[i];
  }
  inline bool
  isascii_end(const std::wstring &s) { return !s.empty() && s.back() < 128; }
  // ---------------------------------------------------------------- the page
  struct frame_result {
    std::vector<button_spec> buttons;
    std::wstring title;
    std::wstring status;
    float content_bottom = 0, buttons_top = 0;
    float max_scroll = 0, view_h = 0, row_h = 0, row_y[8] = {};
  };  // the components list

  inline float
  banner(canvas &c, const view &v, float x, float y, float w, ic glyph, Color bg, Color fg, const std::wstring &text) {
    (void) v;
    const float tw = w - 56;
    const float th = c.measure_h(tw, text, 13.5f, 0);
    const float h = max(44.f, th + 24);
    c.container(x, y, w, h, 16, bg);
    icon(c, glyph, x + 28, y + h / 2, 22, fg);
    c.text(x + 48, y + (h - th) / 2, tw, text, 13.5f, 0, fg);
    return h;
  }

  inline float
  draw_facts(canvas &c, float x, float y, float w, const std::vector<fact> &facts) {
    float cy = y;
    for (const auto &f : facts) {
      c.container(x, cy, 40, 40, 20, f.bg);
      icon(c, f.icon, x + 20, cy + 20, 22, f.fg);
      const float tw = w - 56;
      const float h1 = c.text(x + 56, cy + 1, tw, f.title, 15, 1, c.p.on_surface);
      const float h2 = c.text(x + 56, cy + 1 + h1 + 2, tw, f.body, 13.5f, 0, c.p.on_variant);
      cy += max(40.f, h1 + h2 + 4) + 14;
    }
    return cy - y;
  }

  inline void
  draw_rail(canvas &c, const view &v, ID2D1Bitmap *brand) {
    const palette &p = c.p;
    c.container(kMargin, kMargin, kRailW, kH - 2 * kMargin, 28, p.primary_c);
    if (brand) c.draw_brand(brand, kMargin + 24, kMargin + 26, 56, 56);
    c.text(kMargin + 24, kMargin + 94, kRailW - 40, L"Foundation Sunshine", 19, 2, p.on_primary_c);
    c.text(kMargin + 24, kMargin + 122, kRailW - 40, Z(v, L"安装向导", L"Setup"), 13.5f, 0, alpha(p.on_primary_c, p.hc ? 255 : 190));

    const int cur = current_group(v);
    const bool installing = v.page == pg::installing || v.page == pg::failed || v.page == pg::done;
    const float x0 = kMargin + 24, y0 = kMargin + 176, step = 66;
    for (int i = 0; i <= kGroups; ++i) {
      const float y = y0 + i * step;
      sstate st = sstate::pending;
      if (i < kGroups) {
        if (v.page == pg::installing)
          st = group_state(v, i);
        else if (v.page == pg::done)
          st = sstate::done;
        else if (v.page == pg::failed) {
          st = group_state(v, i);
          if (st == sstate::running) st = sstate::failed;
        }
        else if (v.page == pg::uac && i == 0)
          st = sstate::running;
      }
      else {
        if (v.page == pg::done)
          st = v.opening ? sstate::running : sstate::done;
      }
      (void) installing;
      (void) cur;
      // connector to the next item
      if (i < kGroups) {
        const Color line = (st == sstate::done) ? p.primary : alpha(p.on_primary_c, p.hc ? 255 : 70);
        c.fill_r(x0 + 14, y + 34, 2, step - 38, 1, line);
      }
      const float cx = x0 + 15, cy = y + 15;
      switch (st) {
        case sstate::done:
          c.disc(cx, cy, 30, p.primary);
          icon(c, ic::check, cx, cy, 18, p.on_primary, 2.4f);
          break;
        case sstate::running:
          c.disc(cx, cy, 30, p.primary);
          c.spinner(cx, cy, 18, p.on_primary, 2.4f, v.t, v.animated);
          break;
        case sstate::failed:
          c.disc(cx, cy, 30, p.error);
          icon(c, ic::exclam, cx, cy, 18, p.on_error, 2.4f);
          break;
        case sstate::reboot:
          c.disc(cx, cy, 30, p.warning);
          icon(c, ic::restart, cx, cy, 18, p.hc ? p.on_primary : rgb(0xffffff), 2.2f);
          break;
        default: {
          c.ring(cx, cy, 30, alpha(p.on_primary_c, p.hc ? 255 : 130), 2);
          c.text_center(cx - 15, cy - 15, 30, 30, std::to_wstring(i + 1), 13.5f, 1, alpha(p.on_primary_c, p.hc ? 255 : 200));
        }
      }
      const float tx = x0 + 30 + 14, tw = kRailW - 24 - 30 - 14 - 20;
      const bool active = st == sstate::running || st == sstate::failed || st == sstate::reboot;
      const float h1 = c.text(tx, y + 1, tw, group_title(v, i), 14.5f, active ? 2 : 1, p.on_primary_c);
      std::wstring sub = group_sub(v, i);
      if (st == sstate::running && i < kGroups && v.page == pg::installing)
        sub = detail_text(v, v.last_detail);
      else if (st == sstate::done && i < kGroups)
        sub = Z(v, L"已完成", L"Done");
      else if (st == sstate::failed)
        sub = Z(v, L"没有完成", L"Not finished");
      else if (st == sstate::reboot)
        sub = Z(v, L"等待重启", L"Needs a restart");
      c.text(tx, y + 3 + h1, tw, sub, 12.5f, 0, alpha(p.on_primary_c, p.hc ? 255 : 200));
    }
  }

  inline std::wstring
  percent_step(const view &v, int cur) {
    return v.zh ? (L"第 " + std::to_wstring(cur + 1) + L" 步，共 " + std::to_wstring(kGroups) + L" 步") : (L"Step " + std::to_wstring(cur + 1) + L" of " + std::to_wstring(kGroups));
  }

  inline void
  draw_progress(canvas &c, const view &v, float x, float y, float w) {
    const palette &p = c.p;
    const float gap = 6, sw = (w - gap * (kGroups - 1)) / kGroups;
    for (int i = 0; i < kGroups; ++i) {
      const float sx = x + i * (sw + gap);
      const sstate st = group_state(v, i);
      if (p.hc)
        c.stroke_r(sx, y, sw, 8, 4, p.outline, 1.5f);
      else
        c.fill_r(sx, y, sw, 8, 4, p.outline_var);
      if (st == sstate::done)
        c.fill_r(sx, y, sw, 8, 4, p.primary);
      else if (st == sstate::failed)
        c.fill_r(sx, y, sw, 8, 4, p.error);
      else if (st == sstate::reboot)
        c.fill_r(sx, y, sw, 8, 4, p.warning);
      else if (st == sstate::running) {
        c.fill_r(sx, y, sw, 8, 4, alpha(p.primary, 90));
        const double ph = v.animated ? std::fmod(v.t * 0.8, 1.0) : 0.35;
        const float bw = sw * 0.42f, bx = sx + static_cast<float>(ph) * (sw - bw);
        c.fill_r(bx, y, bw, 8, 4, p.primary);
      }
    }
  }

  // Draws the whole window for `v` and returns the buttons the page has.
  inline frame_result
  draw_page(canvas &c, const view &v, ID2D1Bitmap *brand) {
    frame_result out;
    const palette &p = c.p;
    {
      c.clear(p.page);
    }
    const bool plain = v.page == pg::done;  // the success page is one card, no step rail
    float cardX = kCardX, cardW = kCardW;
    if (plain) {
      cardX = kMargin;
      cardW = kW - 2 * kMargin;
    }
    else
      draw_rail(c, v, brand);
    c.container(cardX, kCardY, cardW, kCardH, 28, p.card);
    if (p.hc) c.stroke_r(cardX, kCardY, cardW, kCardH, 28, p.outline, 2);

    const float x = cardX + kPad, w = cardW - 2 * kPad;
    const float bottom = kCardY + kCardH - kPad;
    float y = kCardY + kPad;
    std::wstring title, lead;
    ic glyph = ic::monitor;
    Color gbg = p.primary_c, gfg = p.on_primary_c;
    bool compact = false, spin_glyph = false;
    auto add_button = [&](int id, const std::wstring &text, bkind kind, bool enabled = true) {
      button_spec b;
      b.id = id;
      b.text = text;
      b.kind = kind;
      b.enabled = enabled;
      b.w = max(kind == bkind::text ? 88.f : 104.f, c.measure_w(text, 15, 1) + 52);
      b.h = 44;
      out.buttons.push_back(b);
    };
    std::vector<fact> facts;
    std::wstring ban;
    ic ban_glyph = ic::info;
    Color ban_bg = p.secondary_c, ban_fg = p.on_secondary_c;
    switch (v.page) {
      case pg::welcome:
        title = Z(v, L"安装 Foundation Sunshine", L"Install Foundation Sunshine");
        lead = Z(v, L"串流主机所需的程序、驱动和后台服务。", L"The host program, drivers and background service.");
        if (v.uac_cancelled) {
          ban = Z(v, L"已取消授权，电脑未改动。", L"Permission cancelled. Nothing on this PC changed.");
          ban_glyph = ic::exclam;
          ban_bg = p.warning_c;
          ban_fg = p.on_warning_c;
        }
        else if (v.upgrade) { ban = Z(v, L"检测到已安装的版本。将原地升级，设置保留。", L"Existing install found. It is upgraded in place; settings are kept."); }
        // A pending restart reaches the shell as 3010 (installer-ms-success-reboot-required-20261109)
        // or code 14 (installer-ms-cm-prob-need-restart-20261109). Only the USB/IP install is
        // expected to ask for one (installer-usbip-win2-readme-20261109).
        facts = {
          { ic::shield, p.tertiary_c, p.on_tertiary_c, Z(v, L"管理员权限", L"Administrator permission"), Z(v, L"安装时 Windows 会请求一次。", L"Windows asks once during setup.") },
          { ic::monitor, p.primary_c, p.on_primary_c, Z(v, L"组件", L"Components"), Z(v, L"默认全部安装，可在“自定义功能”中调整。", L"All are installed by default. Change them in Customize.") },
        };
        add_button(101, Z(v, L"安装", L"Install"), bkind::filled);
        add_button(102, Z(v, L"自定义功能", L"Customize"), bkind::tonal);
        add_button(103, Z(v, L"退出", L"Exit"), bkind::text);
        break;
      case pg::maintenance:
        glyph = ic::restart;
        title = Z(v, L"已安装 Foundation Sunshine", L"Foundation Sunshine is already installed");
        lead = v.zh ? (L"已安装版本 " + v.installed_version + L"，安装包版本 " + v.package_version + L"。")
                   : (L"Installed: " + v.installed_version + L". This package: " + v.package_version + L".");
        if (v.maintenance_downgrade) {
          ban = Z(v, L"安装包版本低于已安装版本，继续将降级。",
                  L"This package is older than the installed version. Continuing downgrades it.");
          ban_glyph = ic::exclam;
          ban_bg = p.warning_c;
          ban_fg = p.on_warning_c;
        }
        else if (v.maintenance_same) {
          ban = Z(v, L"版本相同，继续将重新安装。设置和已配对客户端保留。",
                  L"Same version. Continuing reinstalls; settings and paired clients are kept.");
          ban_glyph = ic::restart;
          ban_bg = p.secondary_c;
          ban_fg = p.on_secondary_c;
        }
        facts = {
          { ic::folder, p.primary_c, p.on_primary_c, Z(v, L"原地更新或修复", L"Update or repair in place"),
            Z(v, L"安装目录、设置、已配对客户端和组件选择保留。", L"The install folder, settings, paired clients and component choices are kept.") },
          { ic::wrench, p.tertiary_c, p.on_tertiary_c, Z(v, L"修改组件", L"Modify components"),
            Z(v, L"取消勾选会卸载对应组件，程序和设置不受影响。", L"Clearing a component uninstalls it. The program and settings are unaffected.") },
          { ic::shield, p.secondary_c, p.on_secondary_c, Z(v, L"离线更新", L"Offline update"),
            Z(v, L"驱动和资源已包含在安装包内，无需联网。", L"Drivers and resources are bundled. No network needed.") },
        };
        if (!ban.empty()) facts.pop_back();
        {
          const std::wstring action = v.maintenance_downgrade ? Z(v, L"降级到 ", L"Downgrade to ") :
                                       v.maintenance_same ? Z(v, L"重新安装 ", L"Reinstall ") : Z(v, L"更新到 ", L"Update to ");
          add_button(101, action + v.package_version, bkind::filled);
        }
        add_button(102, Z(v, L"修改组件", L"Modify components"), bkind::tonal);
        add_button(103, Z(v, L"卸载", L"Uninstall"), bkind::text, v.has_uninstaller);
        break;
      case pg::components: {
        compact = true;
        title = Z(v, L"选择要安装的功能", L"Choose what to install");
        lead = v.maintenance ? Z(v, L"已勾选当前组件。取消勾选会卸载对应驱动，程序、设置和已配对客户端保留。",
                                  L"Current components are ticked. Clearing one uninstalls its driver. The program, settings and paired clients are kept.")
                             : Z(v, L"默认勾选推荐项。", L"Recommended components are ticked by default.");
        add_button(101, Z(v, L"开始安装", L"Install"), bkind::filled);
        add_button(102, Z(v, L"返回", L"Back"), bkind::tonal);
        break;
      }
      case pg::uac:
        glyph = ic::shield;
        gbg = p.tertiary_c;
        gfg = p.on_tertiary_c;
        title = Z(v, L"等待授权", L"Waiting for permission");
        lead = Z(v, L"请在 Windows 弹出的窗口中选“是”。看不到窗口时，请查看任务栏。", L"Choose Yes in the Windows prompt. If it is not visible, check the taskbar.");
        facts = {
          { ic::info, p.secondary_c, p.on_secondary_c, Z(v, L"为什么要授权", L"Why it asks"), Z(v, L"安装服务和驱动需要管理员权限，仅请求一次。", L"Installing the service and drivers needs administrator rights. Windows asks once.") },
          { ic::close, p.secondary_c, p.on_secondary_c, Z(v, L"取消安装", L"Cancel setup"), Z(v, L"选“否”，电脑不会有任何改动。", L"Choose No. Nothing on this PC changes.") },
        };
        add_button(101, Z(v, L"等待确认…", L"Waiting…"), bkind::filled, false);
        break;
      case pg::installing: {
        const int g = current_group(v);
        spin_glyph = true;
        static const wchar_t *tz[] = { L"检查电脑", L"放入程序", L"安装驱动", L"配置网络与服务" };
        static const wchar_t *te[] = { L"Checking this PC", L"Copying the program", L"Installing drivers", L"Setting up network and service" };
        static const wchar_t *lz[] = { L"检查安装包，并关闭正在运行的旧版 Sunshine。", L"复制程序、资源和管理界面，设置保留。", L"安装已勾选的驱动：基地虚拟显示器、虚拟手柄、USB/IP、虚拟鼠标。", L"添加防火墙规则，注册并启动后台服务，设置开机自启。" };
        static const wchar_t *le[] = { L"Checking the package and closing the old Sunshine.", L"Copying the program, files and panel. Settings are kept.", L"Installing the selected drivers: Foundation Virtual Display, virtual gamepad, USB/IP, virtual mouse.", L"Adding firewall rules, starting the background service, setting up autostart." };
        title = v.zh ? tz[g] : te[g];
        lead = v.zh ? lz[g] : le[g];
        break;
      }
      case pg::failed: {
        glyph = ic::exclam;
        gbg = p.error_c;
        gfg = p.on_error_c;
        const std::wstring code = std::to_wstring(v.exit_code);
        if (v.failure == fail_kind::driver) {
          title = Z(v, L"基地虚拟显示器安装失败", L"Foundation Virtual Display failed to install");
          lead = Z(v, L"程序文件已安装。基地虚拟显示器未通过验证，后台服务未启动。", L"Program files are installed. Foundation Virtual Display failed its check, so the background service did not start.");
          ban = Z(v, (L"错误码 " + code + L"：基地虚拟显示器验证失败").c_str(), (L"Error " + code + L": the Foundation Virtual Display check failed").c_str());
          facts = {
            { ic::wrench, p.primary_c, p.on_primary_c, Z(v, L"修复安装", L"Repair"), Z(v, L"重新运行驱动和服务步骤，设置保留。", L"Runs the driver and service steps again. Settings are kept.") },
            { ic::restart, p.warning_c, p.on_warning_c, Z(v, L"仍然失败", L"Still failing"), Z(v, L"重启 Windows 后再点“修复安装”。", L"Restart Windows, then press Repair again.") },
          };
        }
        else if (v.failure == fail_kind::service) {
          title = Z(v, L"后台服务安装失败", L"The background service failed to install");
          lead = Z(v, L"程序文件已安装，后台服务未通过验证。", L"Program files are installed. The background service failed its check.");
          ban = Z(v, (L"错误码 " + code + L"：服务验证失败").c_str(), (L"Error " + code + L": the service check failed").c_str());
          facts = {
            { ic::wrench, p.primary_c, p.on_primary_c, Z(v, L"修复安装", L"Repair"), Z(v, L"重新注册并验证服务，设置保留。", L"Registers and checks the service again. Settings are kept.") },
            { ic::monitor, p.secondary_c, p.on_secondary_c, Z(v, L"在管理界面修复", L"Repair from the panel"), Z(v, L"打开管理界面，使用“修复”。", L"Open the panel and use Repair.") },
          };
        }
        else {
          title = Z(v, L"安装没有完成", L"Setup did not finish");
          lead = Z(v, L"安装中途停止，设置不会被自动删除。", L"Setup stopped part-way. Settings are not deleted.");
          ban = Z(v, (L"错误码 " + code + L"：安装引擎退出").c_str(), (L"Error " + code + L": the installer engine stopped").c_str());
          facts = {
            { ic::restart, p.primary_c, p.on_primary_c, Z(v, L"重试", L"Try again"), Z(v, L"重新运行安装，已安装的部分会更新。", L"Runs the install again. Installed parts are updated.") },
            { ic::doc, p.secondary_c, p.on_secondary_c, Z(v, L"提交诊断信息", L"Report the problem"), Z(v, L"点“复制诊断信息”，粘贴到 Issue。", L"Press Copy details and paste them into an Issue.") },
          };
        }
        ban_glyph = ic::exclam;
        ban_bg = p.error_c;
        ban_fg = p.on_error_c;
        add_button(101, v.failure == fail_kind::generic ? Z(v, L"重试", L"Try again") : Z(v, L"修复安装", L"Repair"), bkind::filled);
        add_button(102, Z(v, L"复制诊断信息", L"Copy details"), bkind::tonal);
        add_button(103, Z(v, L"打开日志", L"Open log"), bkind::text);
        break;
      }
      case pg::done:
        glyph = ic::check;
        gbg = p.success_c;
        gfg = p.on_success_c;
        title = Z(v, L"安装完成", L"Setup complete");
        lead = v.panel_failed ? Z(v, L"管理界面未能打开。点“开启串流旅程”重试。", L"The panel did not open. Press Start your streaming journey to retry.") : std::wstring();
        if (v.webview2_missing) {
          // installed, but the Panel cannot run yet: no WebView2 Runtime (the bootstrapper needs the network)
          lead = Z(v, L"管理界面需要 Microsoft Edge WebView2 Runtime，本次未能安装（可能没有联网）。可从 go.microsoft.com/fwlink/p/?LinkId=2124703 下载；现在可先用网页管理 https://localhost:47990。",
            L"The panel needs the Microsoft Edge WebView2 Runtime, which could not be installed (no network?). Get it from go.microsoft.com/fwlink/p/?LinkId=2124703; meanwhile use the Web UI at https://localhost:47990.");
          add_button(101, Z(v, L"打开网页管理", L"Open the Web UI"), bkind::filled);
          add_button(102, Z(v, L"重启电脑", L"Restart PC"), bkind::text);
          break;
        }
        add_button(101, v.opening ? Z(v, L"正在打开…", L"Opening…") : Z(v, L"开启串流旅程", L"Start your streaming journey"), bkind::filled, !v.opening);
        // The restart is optional and weak: pressing nothing is "later". The next step stays the primary action.
        add_button(102, Z(v, L"重启电脑", L"Restart PC"), bkind::text);
        break;
    }

    if (plain) {
      // one centred success card: disc, title, buttons
      const float cy = kCardY + kCardH / 2 - 60;
      c.container(kW / 2 - 48, cy - 48, 96, 96, 48, gbg);
      if (v.opening)
        c.spinner(kW / 2, cy, 52, gfg, 4.f, v.t, v.animated);
      else
        icon(c, glyph, kW / 2, cy, 52, gfg, 2.4f);
      const float th = c.text(x, cy + 72, w, title, 32, 2, p.on_surface, 1);
      float ly = cy + 72 + th + 8;
      if (!lead.empty()) ly += c.text(x, ly, w, lead, 15, 0, p.on_variant, 1);
      out.title = title;
      out.content_bottom = ly;
      float total = 0;
      for (const auto &b : out.buttons) total += b.w;
      total += 12 * (out.buttons.size() - 1);
      // primary (first added) sits on the right
      float bx = kW / 2 + total / 2;
      for (auto &b : out.buttons) {
        bx -= b.w;
        b.x = bx;
        b.y = ly + 40;
        bx -= 12;
      }
      out.buttons_top = ly + 40;
      out.status = title;
      return out;
    }

    // header
    if (!compact) {
      c.container(x, y, 52, 52, 26, gbg);
      if (spin_glyph)
        c.spinner(x + 26, y + 26, 30, gfg, 3.2f, v.t, v.animated);
      else
        icon(c, glyph, x + 26, y + 26, 28, gfg, 2.2f);
      y += 52 + 18;
    }
    out.title = title;
    y += c.text(x, y, w, title, compact ? 24 : 27, 2, p.on_surface) + 8;
    y += c.text(x, y, w, lead, 15, 0, p.on_variant) + 18;

    if (!ban.empty() && v.page != pg::components) y += banner(c, v, x, y, w, ban_glyph, ban_bg, ban_fg, ban) + 16;

    // content
    if (v.page == pg::installing) {
      const int g = current_group(v);
      c.text(x, y, w, percent_step(v, g), 13, 1, p.on_variant);
      y += 24;
      draw_progress(c, v, x, y, w);
      y += 8 + 22;
      c.container(x, y, w, 56, 18, p.tile);
      c.spinner(x + 28, y + 28, 22, p.primary, 2.8f, v.t, v.animated);
      c.text(x + 52, y + 8, w - 68, detail_text(v, v.last_detail), 15, 1, p.on_surface);
      c.text(x + 52, y + 30, w - 68, Z(v, L"完成后自动进入下一步", L"Continues automatically"), 12.5f, 0, p.on_variant);
      y += 56 + 14;
      const std::wstring hint = g == 2 ? Z(v, L"屏幕可能闪烁。如果 Windows 询问是否安装设备软件，选“安装”。", L"The screen may flicker. If Windows asks to install device software, choose Install.") : Z(v, L"无需操作，请保持电脑开机。", L"Nothing to do. Keep the PC on.");
      y += banner(c, v, x, y, w, g == 2 ? ic::info : ic::check, p.secondary_c, p.on_secondary_c, hint);
      out.status = title + L": " + detail_text(v, v.last_detail);
      add_button(101, Z(v, L"最小化窗口", L"Minimise"), bkind::tonal);
    }
    else if (v.page == pg::components) {
      // a vertical, scrolling list in two groups: recommended (the Inno recommended type) and optional
      const float top = y - 6;
      c.text(x, top + 7, w - 130, Z(v, L"程序、管理界面和后台服务必装", L"The program, panel and service are always installed"), 13, 0, p.on_variant);
      bool all = true;
      for (int i = 0; i < 8; ++i) all = all && v.tiles[i];
      {
        button_spec b;
        b.id = 130;
        b.kind = bkind::text;
        b.h = 32;
        b.text = all ? Z(v, L"取消全选", L"Clear all") : Z(v, L"全选", L"Select all");
        b.w = max(80.f, c.measure_w(b.text, 15, 1) + 32);
        b.x = x + w - b.w + 8;
        b.y = top;
        out.buttons.push_back(b);
      }
      const float vy = top + 42, vh = (bottom - 44 - 12) - vy;
      const float rh = 52, pitch = 56, hh = 30, listw = w - 14;
      float off = 0, row_off[8], hdr_off[2];
      hdr_off[0] = off;
      off += hh;
      for (int i = 0; i < 7; ++i) {
        row_off[i] = off;
        off += pitch;
      }
      off += 10;
      hdr_off[1] = off;
      off += hh;
      row_off[7] = off;
      off += pitch;
      const float total = off - (pitch - rh);
      out.max_scroll = max(0.f, total - vh);
      out.view_h = vh;
      out.row_h = rh;
      for (int i = 0; i < 8; ++i) out.row_y[i] = i == 0 ? 0 : i == 7 ? hdr_off[1] :
                                                                       row_off[i];
      const float sc = std::floor(min(max(v.scroll, 0.f), out.max_scroll) * c.s + 0.5f) / c.s;  // whole device pixels: crisp text while it scrolls
      c.clip(x - 6, vy, listw + 12, vh);
      const std::wstring groups[2] = { Z(v, L"推荐安装", L"Recommended"), Z(v, L"可选", L"Optional") };
      for (int g = 0; g < 2; ++g) c.text(x + 4, vy + hdr_off[g] - sc + 6, listw, groups[g], 13, 2, p.primary);
      for (int i = 0; i < 8; ++i) {
        button_spec b;
        b.id = 120 + i;
        b.kind = bkind::row;
        b.text = tile_name(v, i);
        b.desc = tile_desc(v, i);
        b.checked = v.tiles[i];
        b.x = x;
        b.y = vy + row_off[i] - sc;
        b.w = listw;
        b.h = rh;
        b.visible = b.y >= vy - 0.5f && b.y + b.h <= vy + vh + 0.5f;
        c.save();
        c.translate(b.x * c.s, b.y * c.s);
        btn_state row_state;
        row_state.hover = v.row_hover[i];
        row_state.press = v.row_press[i];
        row_state.focus = v.focus_row == i;
        draw_button(c, b, row_state, p.card);
        c.restore();
        out.buttons.push_back(b);
      }
      c.reset_clip();
      if (out.max_scroll > 0) {
        const float th = max(28.f, vh * vh / total), tp = (vh - th) * (out.max_scroll > 0 ? sc / out.max_scroll : 0);
        c.fill_r(x + w - 5, vy, 4, vh, 2, alpha(p.outline_var, p.hc ? 255 : 120));
        c.fill_r(x + w - 5, vy + tp, 4, th, 2, p.hc ? p.on_surface : alpha(p.primary, 190));
      }
      y = vy + vh;
    }
    else if (!facts.empty()) {
      y += draw_facts(c, x, y, w, facts);
    }
    out.content_bottom = y;

    float rx = x + w;
    const float by = bottom - 44;
    for (auto &b : out.buttons) {
      if (b.kind == bkind::tile || b.kind == bkind::row || b.id == 130) continue;
      b.y = by;
    }
    for (auto &b : out.buttons) {
      if (b.kind == bkind::tile || b.kind == bkind::row || b.kind == bkind::text) continue;
      rx -= b.w;
      b.x = rx;
      rx -= 12;
    }
    for (auto &b : out.buttons)
      if (b.kind == bkind::text && b.id != 130) b.x = x - 8;
    out.buttons_top = by;
    out.status = out.status.empty() ? title : out.status;
    return out;
  }

  // The layout of the buttons only, for tests of the live window's controls.
  inline Color
  button_backdrop(const palette &p) { return p.card; }
}  // namespace ui

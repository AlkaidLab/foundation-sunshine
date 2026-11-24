/**
 * @file src/confighttp/web_install.h
 * @brief Is the Web UI folder of this install there, and what to say when it is not.
 *
 * An update that failed halfway used to leave the install without `assets\web`: the host then
 * answered with its built-in English sign-in page and every page inside the Control Panel stayed
 * empty, which looks like a foreign site rather than a broken install. The installer now moves the
 * old folder aside and puts it back when it does not finish (`assets\web.previous`); this header is
 * the host's side: restore that folder at start when the live one is gone, and say plainly, in
 * Chinese and English, that the install is incomplete and how to repair it.
 */
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace confighttp::web_install {
  namespace fs = std::filesystem;

  /** The file the installer writes after the last Web UI file: its presence says the installer got to its end. */
  inline constexpr std::string_view complete_marker = ".install-complete";

  /** True when `dir` holds the pages the host serves first and at least one script they load. */
  inline bool
  has_pages(const fs::path &web_dir) {
    std::error_code error;
    if (!fs::is_regular_file(web_dir / "index.html", error) || !fs::is_regular_file(web_dir / "login.html", error)) return false;
    // Older builds put plain scripts directly in assets; assets now ship
    // precompressed, so a script is either a plain .js of an old layout or
    // the single .js.br copy of a current one.
    for (const auto &directory : { web_dir / "assets", web_dir / "assets" / "generated" }) {
      error.clear();
      for (fs::directory_iterator it { directory, error }, end; !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().string();
        if ((name.ends_with(".js") || name.ends_with(".js.br")) && it->is_regular_file(error)) return true;
      }
    }
    return false;
  }

  /** What the host can tell about a live folder by itself: the pages and a script are there. A folder missing them shows nothing. */
  inline bool
  is_complete(const fs::path &web_dir) {
    return has_pages(web_dir);
  }

  /** The folder the installer leaves beside the Web UI folder while an update runs. */
  inline fs::path
  previous_dir_of(const fs::path &web_dir) {
    auto previous = web_dir;
    if (previous.filename().empty()) previous = previous.parent_path();  // "web/" -> "web"
    previous += ".previous";
    return previous;
  }

  /**
   * Put the folder an interrupted update left aside back when the live folder has nothing to show.
   * The old folder exists only while an update runs or after one was stopped. The host acts only when
   * the live folder has no pages at all: while the installer copies, pages arrive first and a new
   * host started by the installer must not take the folder away from it. A copy that stopped after
   * its first files is put right by the next run of the installer, which tells it by the marker it
   * writes after the last file. Returns true when the old folder was put back.
   */
  inline bool
  recover_previous(const fs::path &web_dir) {
    std::error_code error;
    const auto previous = previous_dir_of(web_dir);
    if (!has_pages(previous)) return false;
    if (fs::exists(web_dir / "index.html", error)) return false;
    if (fs::exists(web_dir, error)) {
      fs::remove_all(web_dir, error);
      if (error) return false;
    }
    fs::rename(previous, web_dir, error);
    return !error && has_pages(web_dir);
  }

  /** The words shown where the pages should be. Static text: nothing of the machine is in it. */
  inline std::string
  notice_html() {
    return std::string {
      "<div class=\"notice\" role=\"alert\">"
      "<strong>Sunshine 安装不完整</strong>"
      "<p>网页界面的文件缺失（assets\\web），所以这里只能显示登录页。请打开 Sunshine 控制面板，点「修复」；"
      "或重新运行 Sunshine 安装程序，设置和已配对的设备会保留。</p>"
      "<p lang=\"en\">The installation is incomplete: the web interface files are missing. "
      "Repair it from the Sunshine Control Panel, or run the Sunshine installer again; settings and paired devices stay.</p>"
      "</div>"
    };
  }

  /** The notice's own style, for pages that have none. */
  inline std::string_view
  notice_style() {
    return ".notice{background:#fff4e5;border:1px solid #e0a040;border-radius:9px;padding:10px 12px;margin:0 0 16px;font-size:14px;line-height:1.5}"
           ".notice p{margin:6px 0 0}";
  }

  /** A whole page for a route whose file is missing. */
  inline std::string
  incomplete_page() {
    std::string page = "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
                       "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                       "<title>Sunshine 安装不完整 / Installation incomplete</title><style>"
                       "body{font-family:system-ui,sans-serif;margin:0;min-height:100vh;display:grid;place-items:center;background:#f4f6f8;color:#1d1d1f}"
                       "main{width:min(92vw,520px)}";
    page += notice_style();
    page += "</style></head><body><main>";
    page += notice_html();
    page += "</main></body></html>";
    return page;
  }
}  // namespace confighttp::web_install

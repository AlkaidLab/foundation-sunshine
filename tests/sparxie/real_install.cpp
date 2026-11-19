#include "src/sparxie/art.h"
#include "src/sparxie/host.h"
#include "src/sparxie/icon_file.h"
#include "src/sparxie/install.h"
#include "src/sparxie/platform.h"
#include "src/sparxie/sha256.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
  std::string
  flag(int argc, char **argv, const std::string &key, const std::string &fallback = {}) {
    for (int i = 1; i + 1 < argc; ++i)
      if (argv[i] == key) return argv[i + 1];
    return fallback;
  }

  bool
  has(int argc, char **argv, const std::string &key) {
    for (int i = 1; i < argc; ++i)
      if (argv[i] == key) return true;
    return false;
  }

  std::string
  file_hash(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return sparxie::sha256::hash(bytes.data(), bytes.size());
  }

  std::string
  read_text(const fs::path &path) {
    std::ifstream in(path);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }

  sparxie::json
  tree_of(const fs::path &root) {
    sparxie::json files = sparxie::json::array();
    std::error_code ec;
    if (!fs::exists(root, ec)) return files;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); ++it) {
      if (!it->is_regular_file(ec)) continue;
      const std::string rel = fs::relative(it->path(), root, ec).generic_string();
      if (ec || rel == "component.json") continue;
      files.push_back({
        { "path", rel },
        { "bytes", fs::file_size(it->path(), ec) },
        { "sha256", file_hash(it->path()) },
      });
    }
    std::sort(files.begin(), files.end(), [](const sparxie::json &a, const sparxie::json &b) {
      return a.value("path", std::string()) < b.value("path", std::string());
    });
    return files;
  }

  sparxie::json
  probes_json(const sparxie::Probes &probes) {
    return {
      { "osBuild", probes.os_build },
      { "osX64", probes.os_x64 },
      { "freeBytes", probes.free_bytes },
      { "webview2", probes.webview2 },
      { "webview2Version", probes.webview2_version },
      { "vcruntime", probes.vcruntime },
      { "vcruntimeVersion", probes.vcruntime_version },
      { "canWrite", probes.can_write },
      { "canElevate", probes.can_elevate },
    };
  }

  sparxie::HostEnv
  env_for(const fs::path &dest, const fs::path &host_data) {
    sparxie::HostEnv env;
    env.host_data = host_data;
    env.data_dir = dest;
    env.component_dir = dest;
    env.install_root = dest;
    std::error_code ec;
    fs::create_directories(host_data, ec);
    return env;
  }

  void
  emit(const sparxie::json &body) {
    std::cout << body.dump(2) << std::endl;
  }
}  // namespace

int
main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "sparxie_real_install install|uninstall|status|tree|scan|refresh|icon|hero|decode\n";
    return 2;
  }
  const std::string command = argv[1];
  if (command == "decode") {
    // decodes picture files (comma separated, in order, each --times times) with the host's own decoder and reports each answer's size and mean colour
    sparxie::json runs = sparxie::json::array();
    std::stringstream names(flag(argc, argv, "--files", flag(argc, argv, "--file")));
    std::string name;
    while (std::getline(names, name, ',')) {
      std::ifstream in(name, std::ios::binary);
      const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      for (int i = 0; i < std::stoi(flag(argc, argv, "--times", "1")); ++i) {
        const auto image = sparxie::host_decode_image(bytes);
        unsigned long long sum[4] = { 0, 0, 0, 0 };
        for (std::size_t at = 0; at + 3 < image.px.size(); at += 4)
          for (int c = 0; c < 4; ++c) sum[c] += image.px[at + c];
        const unsigned long long count = std::max<std::size_t>(1, image.px.size() / 4);
        runs.push_back({ { "file", name }, { "w", image.w }, { "h", image.h }, { "meanRGBA", sparxie::json::array({ sum[0] / count, sum[1] / count, sum[2] / count, sum[3] / count }) } });
      }
    }
    emit({ { "command", "decode" }, { "runs", runs } });
    return 0;
  }
  if (command == "icon") {
    // the program icon of an executable (or the best picture of an .ico) with the host's own parser and decoder: written as the 256 px PNG the host publishes; read-only on the input
    const std::string file = flag(argc, argv, "--file");
    const auto decode = [](std::string_view bytes) { return sparxie::host_decode_image(bytes); };
    sparxie::Rgba raw;
    const std::string lower = [&] {
      std::string text = file;
      for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return text;
    }();
    if (lower.size() > 4 && (lower.compare(lower.size() - 4, 4, ".exe") == 0 || lower.compare(lower.size() - 4, 4, ".dll") == 0)) {
      raw = sparxie::icon_from_pe(fs::path(file), decode);
    }
    else {
      std::ifstream in(file, std::ios::binary);
      const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      raw = sparxie::icon_from_file_bytes(bytes, decode);
    }
    const auto icon = sparxie::icon_square(raw);
    const auto png = sparxie::encode_png(icon);
    const std::string out_path = flag(argc, argv, "--out");
    if (!out_path.empty() && !png.empty()) {
      std::ofstream out(out_path, std::ios::binary);
      out.write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
    }
    emit({ { "command", "icon" }, { "file", file }, { "source", { raw.w, raw.h } }, { "icon", { icon.w, icon.h } }, { "usable", sparxie::icon_is_usable(raw) }, { "pngBytes", png.size() } });
    return raw.w > 0 ? 0 : 1;
  }
  if (command == "hero") {
    // composes the landscape hero of a key art (and a logo) with the host's own decoder and composer, writes the JPEG and reports its mean colour
    const auto read = [](const std::string &path) {
      std::ifstream in(path, std::ios::binary);
      return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const auto background = sparxie::host_decode_image(read(flag(argc, argv, "--bg")));
    const std::string logo_path = flag(argc, argv, "--logo");
    const auto logo = logo_path.empty() ? sparxie::Rgba {} : sparxie::host_decode_image(read(logo_path));
    const auto hero = sparxie::compose_hero(background, logo);
    unsigned long long sum = 0;
    for (std::size_t at = 0; at + 3 < hero.px.size(); at += 4) sum += hero.px[at];
    const auto jpeg = sparxie::encode_jpeg(hero, sparxie::kHeroJpegQuality);
    std::ofstream out(flag(argc, argv, "--out"), std::ios::binary);
    out.write(reinterpret_cast<const char *>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
    emit({ { "bg", { background.w, background.h } }, { "logo", { logo.w, logo.h } }, { "hero", { hero.w, hero.h } }, { "meanR", sum / std::max<std::size_t>(1, hero.px.size() / 4) }, { "jpegBytes", jpeg.size() } });
    return 0;
  }
  if (command == "scan" || command == "refresh") {
    // The real machine's own answers: the library scan (what the host finds installed) and the official-art refresh into a scratch host-data folder (nothing of the host's own is touched).
    const fs::path scratch = flag(argc, argv, "--scratch", "scratch");
    sparxie::HostEnv env = env_for(scratch / "sparxie-data", scratch / "host-data");
    env.apps_file = scratch / "apps.json";
    env.covers_dir = scratch / "covers";
    env.registry = &sparxie::host_registry();
    env.process_running = sparxie::host_process_running;
    for (const char *name : { "ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA" })
      if (const char *dir = std::getenv(name)) env.hypergryph_roots.push_back(fs::path(dir) / "Hypergryph Launcher");
    {
      std::ofstream apps(env.apps_file);
      apps << "{\"apps\":[]}";
    }
    if (command == "scan") {
      const auto listed = sparxie::list_games(env);
      emit({ { "command", "scan" }, { "detected", listed.body.value("detected", sparxie::json::array()) } });
      return listed.status.ok ? 0 : 1;
    }
    const auto refreshed = sparxie::refresh_official(env, sparxie::host_http_get, [](std::string_view bytes) { return sparxie::host_decode_image(bytes); }, sparxie::host_http_post_json);
    emit({ { "command", "refresh" }, { "report", refreshed.body } });
    return refreshed.status.ok ? 0 : 1;
  }
  const fs::path dest = flag(argc, argv, "--dest");
  const fs::path host_data = flag(argc, argv, "--host-data", (dest.empty() ? fs::path("host-data") : dest.parent_path() / "host-data").string());
  if (command == "tree") {
    const fs::path root = flag(argc, argv, "--root");
    emit({ { "root", root.string() }, { "files", tree_of(root) } });
    return 0;
  }
  if (dest.empty()) {
    std::cerr << "--dest is required\n";
    return 2;
  }
  auto env = env_for(dest, host_data);
  if (command == "status") {
    const auto status = sparxie::component_status(env);
    emit({ { "command", "status" }, { "body", status.body } });
    return status.status.ok ? 0 : 1;
  }
  if (command == "uninstall") {
    sparxie::UninstallRequest req;
    req.dest = dest;
    req.data_dir = dest;
    const std::string config = flag(argc, argv, "--delete-config");
    if (!config.empty()) {
      req.delete_config_set = true;
      req.delete_config = config == "delete";
    }
    const auto out = sparxie::uninstall_component(req);
    emit({
      { "command", "uninstall" },
      { "http", out.status.http },
      { "error_code", out.status.error_code },
      { "body", out.body },
    });
    return out.status.ok ? 0 : 1;
  }
  if (command != "install") {
    std::cerr << "unknown command\n";
    return 2;
  }
  sparxie::InstallRequest req;
  req.zip = flag(argc, argv, "--zip");
  req.dest = dest;
  req.acknowledged = has(argc, argv, "--ack");
  req.confirm_unverified = has(argc, argv, "--confirm");
  req.probes = sparxie::current_probes(dest);
  const auto out = sparxie::install_from_file(env, req);
  const std::string app = out.body.value("app", std::string());
  const fs::path desktop = app.empty() ? fs::path() : dest / app / "Sparxie.Desktop.exe";
  std::error_code ec;
  const auto files = tree_of(dest);
  sparxie::json body = {
    { "command", "install" },
    { "http", out.status.http },
    { "error_code", out.status.error_code },
    { "ok", out.status.ok },
    { "probes", probes_json(req.probes) },
    { "outcome", out.body },
    { "fileCount", files.size() },
    { "versionIni", read_text(dest / "version.ini") },
    { "desktopExe", desktop.string() },
    { "desktopExists", !desktop.empty() && fs::is_regular_file(desktop, ec) },
  };
  if (out.status.ok) {
    auto again = env_for(dest, host_data);
    body["status"] = sparxie::component_status(again).body;
  }
  emit(body);
  return out.status.ok ? 0 : 1;
}

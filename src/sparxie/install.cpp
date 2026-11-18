#include "src/sparxie/install.h"

#include "src/sparxie/sha256.h"
#include "src/sparxie/zip_extract.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <thread>

namespace sparxie {
  namespace {
    constexpr int kMinBuild = 19041;
    constexpr std::uint64_t kSlack = 64ull * 1024ull * 1024ull;
    const char kSep = static_cast<char>(92);

    json
    fail_body(const std::string &code, const std::string &message, json extra = json::object()) {
      extra["ok"] = false;
      extra["error"] = message;
      extra["error_code"] = code;
      return extra;
    }

    InstallOutcome
    stop(int http, std::string code, std::string message, json extra = json::object()) {
      InstallOutcome out;
      out.status = Status::fail(http, code, message);
      out.body = fail_body(code, message, std::move(extra));
      return out;
    }

    std::string
    hash_file(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) return {};
      std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      return sha256::hash(bytes.data(), bytes.size());
    }

    std::string
    utc_now() {
      const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
      std::tm tm {};
#if defined(_WIN32)
      gmtime_s(&tm, &now);
#else
      gmtime_r(&now, &tm);
#endif
      char buf[32];
      if (!std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm)) return {};
      return buf;
    }

    std::string
    slashes(std::string text) {
      for (char &ch : text)
        if (ch == kSep) ch = '/';
      return text;
    }

    json
    dep(const char *id, const char *result, std::string detail, bool required) {
      return json { { "id", id }, { "result", result }, { "detail", std::move(detail) }, { "required", required } };
    }

    json
    dependencies_of(const Probes &probes, std::uint64_t need, bool after_extract, bool files_ok) {
      json list = json::array();
      if (!probes.os_known)
        list.push_back(dep("os", "skip", "not checked on this system", true));
      else if (probes.os_build < kMinBuild || !probes.os_x64)
        list.push_back(dep("os", "error", "need Windows 10 2004 x64 or newer (build " + std::to_string(kMinBuild) + ")", true));
      else
        list.push_back(dep("os", "ok", std::to_string(probes.os_build), true));

      if (!probes.free_known || need == 0)
        list.push_back(dep("disk", "skip", need ? "free space was not read" : "size not known yet", true));
      else if (probes.free_bytes < need)
        list.push_back(dep("disk", "error", std::to_string(probes.free_bytes), true));
      else
        list.push_back(dep("disk", "ok", std::to_string(probes.free_bytes), true));

      if (!probes.webview2_known)
        list.push_back(dep("webview2", "skip", "not checked on this system", true));
      else if (!probes.webview2)
        list.push_back(dep("webview2", "error", "WebView2 Runtime is not installed", true));
      else
        list.push_back(dep("webview2", "ok", probes.webview2_version, true));

      if (!probes.vcruntime_known)
        list.push_back(dep("vcruntime", "skip", "not checked on this system", true));
      else if (!probes.vcruntime)
        list.push_back(dep("vcruntime", "error", "The x64 Visual C++ runtime is not installed", true));
      else
        list.push_back(dep("vcruntime", "ok", probes.vcruntime_version, true));

      if (!probes.write_known)
        list.push_back(dep("admin", "skip", "not checked on this system", true));
      else if (probes.can_write)
        list.push_back(dep("admin", "ok", "the install directory is writable; elevation is not required", true));
      else
        list.push_back(dep("admin", "error", probes.can_elevate ? "the install directory is not writable until the process is elevated" : "this account cannot write the install directory", true));

      if (!after_extract)
        list.push_back(dep("quarantine", "skip", "checked after the files are in place", true));
      else if (files_ok)
        list.push_back(dep("quarantine", "ok", "the installed files are present", true));
      else
        list.push_back(dep("quarantine", "error", "a file written by the install is missing; it may have been quarantined", true));
      return list;
    }

    std::string
    code_for(const std::string &id) {
      if (id == "os") return "os_too_old";
      if (id == "disk") return "disk_full";
      if (id == "webview2") return "webview2_missing";
      if (id == "vcruntime") return "vcruntime_missing";
      if (id == "admin") return "no_admin";
      if (id == "quarantine") return "quarantined";
      return "dependency";
    }

    void
    note(InstallOutcome &out, const std::function<void(const Progress &)> &progress, std::string stage, int percent, std::string detail = {}) {
      Progress row;
      row.stage = stage;
      row.percent = percent;
      row.detail = detail;
      out.progress.push_back(row);
      if (progress) progress(row);
    }

    const Pin *
    match_hash(const std::vector<Pin> &pins, const std::string &hash) {
      for (const auto &pin : pins)
        if (pin.sha256 == hash) return &pin;
      return nullptr;
    }

    bool
    size_without_hash(const std::vector<Pin> &pins, std::uint64_t bytes, const std::string &hash) {
      for (const auto &pin : pins)
        if (pin.bytes == bytes && pin.sha256 != hash) return true;
      return false;
    }

    fs::path
    content_root(const fs::path &staging) {
      std::error_code ec;
      std::vector<fs::directory_entry> tops;
      for (const auto &entry : fs::directory_iterator(staging, ec)) tops.push_back(entry);
      if (tops.size() == 1 && tops[0].is_directory() && tops[0].path().filename() == "Sparxie") return tops[0].path();
      return staging;
    }

    std::string
    app_dir_name(const fs::path &root) {
      std::ifstream ini(root / "version.ini");
      std::string line;
      while (std::getline(ini, line)) {
        if (line.rfind("exe_path=", 0) != 0) continue;
        std::string value = slashes(line.substr(std::string("exe_path=").size()));
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
        const auto slash = value.find('/');
        const std::string top = slash == std::string::npos ? value : value.substr(0, slash);
        if (top.rfind("app-", 0) == 0) return top;
      }
      std::error_code ec;
      for (const auto &entry : fs::directory_iterator(root, ec)) {
        const auto name = entry.path().filename().string();
        if (entry.is_directory() && name.rfind("app-", 0) == 0) return name;
      }
      return {};
    }

    bool
    launcher_present(const fs::path &root) {
      std::error_code ec;
      const char *names[] = { "Sparxie.exe", "Sparxie.Launcher.exe", "Sparxie.Desktop.exe" };
      for (const char *name : names)
        if (fs::is_regular_file(root / name, ec)) return true;
      const std::string app = app_dir_name(root);
      if (!app.empty() && fs::is_regular_file(root / app / "Sparxie.Desktop.exe", ec)) return true;
      return false;
    }

    void
    overlay(const fs::path &from, const fs::path &dest) {
      std::error_code ec;
      fs::create_directories(dest, ec);
      for (auto it = fs::recursive_directory_iterator(from, ec); it != fs::recursive_directory_iterator(); ++it) {
        const fs::path rel = fs::relative(it->path(), from, ec);
        if (ec) continue;
        const fs::path target = dest / rel;
        if (it->is_directory()) {
          fs::create_directories(target, ec);
          continue;
        }
        if (rel.generic_string() == "config.json" && fs::exists(dest / "config.json")) continue;
        fs::create_directories(target.parent_path(), ec);
        fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
      }
    }

    bool
    files_healthy(const fs::path &root, const Probes &probes) {
      std::error_code ec;
      const fs::path exe = fs::is_regular_file(root / "Sparxie.exe", ec) ? root / "Sparxie.exe" : root / "Sparxie.Desktop.exe";
      if (!fs::is_regular_file(exe, ec) || fs::file_size(exe, ec) == 0) return false;
      if (probes.isolated && probes.isolated(exe)) return false;
      return true;
    }

    struct RunningJob {
      std::string stage = "dependencies";
      int percent = 0;
      bool done = false;
      int http = 200;
      json body = json::object();
    };

    std::mutex g_jobs_mu;
    std::map<std::string, RunningJob> g_jobs;

    bool
    job_busy() {
      for (const auto &item : g_jobs)
        if (!item.second.done) return true;
      return false;
    }
  }  // namespace

  std::vector<Pin>
  builtin_pins() {
    return {
      { "0.1.0-preview.9", "Sparxie-desktop-0.1.0-preview.9-win-x64.zip", "3e5591d7126aeb63972f30aaaf3f6649d695271312803454f7d217d79e903c7c", 100832503ull, "collaborator package" },
      { "ci-4438b663e", "Sparxie-desktop-ci-4438b663e-win-x64.zip", "5053db25f803c1637eda1ccdb156266210d38c185e1180dea7e976be7799db77", 103911941ull, "build-test artifact main 4438b663e run 42796922646; not a download address" },
    };
  }

  json
  manifest_json() {
    json assets = json::array();
    for (const auto &pin : builtin_pins()) {
      assets.push_back({
        { "version", pin.version },
        { "file", pin.file_name },
        { "sha256", pin.sha256 },
        { "bytes", pin.bytes },
        { "note", pin.note },
      });
    }
    return {
      { "name", "sparxie" },
      { "publisher", "暗檬" },
      { "online", false },
      { "minOsBuild", kMinBuild },
      { "arch", "x64" },
      { "requiresAdmin", false },
      { "hint", "请自行取得 Sparxie 的安装包，然后在这里选择它。" },
      { "hintEn", "Get the Sparxie package yourself, then choose it here." },
      { "licence", "The package stays the author's. This host does not download it and does not redistribute it. A known checksum marks that ZIP as verified. Any other ZIP installs only after an explicit confirmation." },
      { "assets", std::move(assets) },
    };
  }

  bool
  notice_recorded(const HostEnv &env) {
    std::ifstream in(env.host_data / "warning.json");
    if (!in) return false;
    try {
      json body;
      in >> body;
      return body.value("acknowledged", false);
    }
    catch (const std::exception &) {
      return false;
    }
  }

  json
  install_status_fields(const HostEnv &env) {
    const fs::path dest = env.component_dir.empty() ? env.host_data / "components" / "sparxie" : env.component_dir;
    const Probes probes = current_probes(dest);
    json body;
    body["manifest"] = manifest_json();
    body["noticeAck"] = notice_recorded(env);
    body["installHint"] = body["manifest"].value("hint", "");
    body["installHintEn"] = body["manifest"].value("hintEn", "");
    body["dependencies"] = dependencies_of(probes, 0, false, true);
    body["componentDir"] = dest.string();
    return body;
  }

  InstallOutcome
  install_from_file(HostEnv &env, InstallRequest req, const std::function<void(const Progress &)> &progress) {
    InstallOutcome out;
    if (req.running) return stop(409, "sparxie_running", "Sparxie is open. Close it before installing.");
    if (req.dest.empty()) return stop(400, "invalid_input", "The install directory is missing");
    if (!req.probes.supplied) req.probes = current_probes(req.dest);
    if (req.pins.empty()) req.pins = builtin_pins();
    if (!notice_recorded(env) && !req.acknowledged) {
      json extra = { { "statement", kStatement }, { "statement_en", kStatementEn } };
      return stop(409, "notice", "The statement has to be acknowledged before the first install", std::move(extra));
    }
    if (req.acknowledged && !notice_recorded(env)) acknowledge_warning(env);
    note(out, progress, "dependencies", 5);
    const json early = dependencies_of(req.probes, 0, false, true);
    for (const char *id : { "os", "webview2", "vcruntime", "admin" }) {
      for (const auto &item : early) {
        if (item.value("id", std::string()) == id && item.value("result", std::string()) == "error") {
          out = stop(409, code_for(id), item.value("detail", std::string()));
          out.body["dependencies"] = early;
          return out;
        }
      }
    }
    std::error_code ec;
    if (!fs::is_regular_file(req.zip, ec)) return stop(404, "not_found", "The package file is not there");
    note(out, progress, "verify", 15);
    const std::string hash = hash_file(req.zip);
    if (hash.empty()) return stop(404, "not_found", "The package file could not be read");
    const auto bytes = fs::file_size(req.zip, ec);
    const Pin *pin = match_hash(req.pins, hash);
    if (!pin && !req.confirm_unverified) {
      json extra = { { "sha256", hash }, { "bytes", bytes }, { "confirm", "未经验证的版本" } };
      if (size_without_hash(req.pins, bytes, hash))
        return stop(400, "hash_mismatch", "The package size matches a known version but the checksum does not", std::move(extra));
      return stop(409, "unverified", "This package is not a known version", std::move(extra));
    }
    const zip::Outcome inspected = zip::inspect(req.zip);
    if (!inspected.ok) return stop(400, inspected.error_code.empty() ? "bad_zip" : inspected.error_code, inspected.error);
    const std::uint64_t need = inspected.uncompressed + kSlack;
    if (req.probes.free_known && req.probes.free_bytes < need) {
      json extra = { { "need", need }, { "free", req.probes.free_bytes } };
      return stop(409, "disk_full", "There is not enough free space for this package", std::move(extra));
    }
    note(out, progress, "extract", 40);
    const fs::path staging = req.dest / ".incoming";
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) return stop(409, "not_writable", "The install directory could not be created");
    struct Cleanup {
      fs::path path;
      ~Cleanup() {
        std::error_code ignore;
        fs::remove_all(path, ignore);
      }
    } cleanup { staging };
    const zip::Outcome extracted = zip::extract(req.zip, staging, [&](int percent) {
      note(out, progress, "extract", 40 + percent / 2);
    });
    if (!extracted.ok) return stop(extracted.error_code == "hash_mismatch" ? 400 : 400, extracted.error_code.empty() ? "bad_zip" : extracted.error_code, extracted.error);
    const fs::path content = content_root(staging);
    if (!launcher_present(content)) return stop(400, "bad_zip", "The package has no Sparxie executable");
    note(out, progress, "probe", 90);
    fs::create_directories(req.dest, ec);
    overlay(content, req.dest);
    const bool healthy = files_healthy(req.dest, req.probes);
    const json deps = dependencies_of(req.probes, need, true, healthy);
    if (!healthy) {
      out = stop(409, "quarantined", "A file written by the install is missing. It may have been quarantined. This host does not add an exclusion.");
      out.body["dependencies"] = deps;
      return out;
    }
    const std::string app = app_dir_name(req.dest);
    const std::string version = pin ? pin->version : std::string("未经验证的版本");
    json component = {
      { "name", "sparxie" },
      { "publisher", "暗檬" },
      { "version", version },
      { "sha256", hash },
      { "verified", pin != nullptr },
      { "bytes", bytes },
      { "app", app },
      { "installedAt", utc_now() },
    };
    {
      std::ofstream out_file(req.dest / "component.json");
      if (!out_file) return stop(409, "not_writable", "component.json could not be written");
      out_file << component.dump(2);
    }
    note(out, progress, "done", 100);
    out.body = {
      { "ok", true },
      { "installed", true },
      { "version", version },
      { "verified", pin != nullptr },
      { "sha256", hash },
      { "app", app },
      { "dest", req.dest.string() },
      { "dependencies", deps },
    };
    json steps = json::array();
    for (const auto &row : out.progress) steps.push_back({ { "stage", row.stage }, { "percent", row.percent } });
    out.body["progress"] = std::move(steps);
    if (!pin) out.body["mark"] = "未经验证的版本";
    return out;
  }

  InstallOutcome
  uninstall_component(const UninstallRequest &req) {
    if (req.running) return stop(409, "sparxie_running", "Sparxie is open. Close it before uninstalling.");
    std::error_code ec;
    if (req.dest.empty() || !fs::exists(req.dest, ec)) {
      InstallOutcome out;
      out.body = { { "ok", true }, { "installed", false } };
      return out;
    }
    const fs::path config = req.dest / "config.json";
    const bool has_config = fs::is_regular_file(config, ec);
    if (has_config && !req.delete_config_set) return stop(409, "confirm_config", "config.json is in the component directory. Say whether to delete it.");
    if (req.delete_config || !has_config) {
      fs::remove_all(req.dest, ec);
      if (ec) return stop(500, "write_failed", "The component directory could not be removed");
    }
    else {
      for (const auto &entry : fs::directory_iterator(req.dest, ec)) {
        if (entry.path().filename() == "config.json") continue;
        fs::remove_all(entry.path(), ec);
      }
    }
    if (req.delete_config && !req.data_dir.empty() && req.data_dir != req.dest) fs::remove(req.data_dir / "config.json", ec);
    InstallOutcome out;
    out.body = { { "ok", true }, { "installed", false }, { "configKept", has_config && !req.delete_config } };
    return out;
  }

  std::string
  start_install_job(HostEnv env, InstallRequest req) {
    std::lock_guard<std::mutex> lock(g_jobs_mu);
    if (job_busy()) return {};
    std::string id(16, '0');
    std::mt19937 rng { std::random_device {}() };
    std::uniform_int_distribution<int> hex(0, 15);
    for (char &ch : id) ch = "0123456789abcdef"[hex(rng)];
    g_jobs[id] = {};
    std::thread([id, env = std::move(env), req = std::move(req)]() mutable {
      try {
        InstallOutcome outcome = install_from_file(env, std::move(req), [&](const Progress &row) {
          std::lock_guard<std::mutex> guard(g_jobs_mu);
          auto found = g_jobs.find(id);
          if (found == g_jobs.end()) return;
          found->second.stage = row.stage;
          found->second.percent = row.percent;
        });
        std::lock_guard<std::mutex> guard(g_jobs_mu);
        auto found = g_jobs.find(id);
        if (found == g_jobs.end()) return;
        found->second.done = true;
        found->second.http = outcome.status.http;
        found->second.body = std::move(outcome.body);
        found->second.stage = "done";
        found->second.percent = 100;
      }
      catch (const std::exception &ex) {
        std::lock_guard<std::mutex> guard(g_jobs_mu);
        auto found = g_jobs.find(id);
        if (found == g_jobs.end()) return;
        found->second.done = true;
        found->second.http = 500;
        found->second.body = fail_body("write_failed", ex.what());
      }
    }).detach();
    return id;
  }

  json
  install_job(const std::string &id) {
    std::lock_guard<std::mutex> lock(g_jobs_mu);
    const auto found = g_jobs.find(id);
    if (found == g_jobs.end()) return fail_body("not_found", "That install is not in progress");
    json body = found->second.body;
    if (!found->second.done) body = json::object();
    body["ok"] = found->second.done ? found->second.body.value("ok", false) : true;
    body["job"] = id;
    body["done"] = found->second.done;
    body["stage"] = found->second.stage;
    body["percent"] = found->second.percent;
    if (found->second.done) body["http"] = found->second.http;
    return body;
  }
}  // namespace sparxie

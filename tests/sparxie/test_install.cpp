#include "src/sparxie/install.h"
#include "src/sparxie/sha256.h"
#include "src/sparxie/zip_extract.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <thread>
namespace fs = std::filesystem;

namespace {
  void
  put16(std::vector<std::uint8_t> &b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
  }
  void
  put32(std::vector<std::uint8_t> &b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  std::uint32_t
  crc32_of(const std::uint8_t *data, std::size_t size) {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
      crc ^= data[i];
      for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
  }
  struct Built {
    std::vector<std::uint8_t> local;
    std::vector<std::uint8_t> central;
    std::uint16_t count = 0;
  };
  void
  add_entry(Built &zip, const std::string &name, const std::string &data, std::uint16_t method, const std::vector<std::uint8_t> &stored) {
    const auto crc = crc32_of(reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
    const auto local_at = static_cast<std::uint32_t>(zip.local.size());
    put32(zip.local, 0x04034b50u);
    put16(zip.local, 20);
    put16(zip.local, 0);
    put16(zip.local, method);
    put16(zip.local, 0);
    put16(zip.local, 0);
    put32(zip.local, crc);
    put32(zip.local, static_cast<std::uint32_t>(stored.size()));
    put32(zip.local, static_cast<std::uint32_t>(data.size()));
    put16(zip.local, static_cast<std::uint16_t>(name.size()));
    put16(zip.local, 0);
    zip.local.insert(zip.local.end(), name.begin(), name.end());
    zip.local.insert(zip.local.end(), stored.begin(), stored.end());
    put32(zip.central, 0x02014b50u);
    put16(zip.central, 20);
    put16(zip.central, 20);
    put16(zip.central, 0);
    put16(zip.central, method);
    put16(zip.central, 0);
    put16(zip.central, 0);
    put32(zip.central, crc);
    put32(zip.central, static_cast<std::uint32_t>(stored.size()));
    put32(zip.central, static_cast<std::uint32_t>(data.size()));
    put16(zip.central, static_cast<std::uint16_t>(name.size()));
    put16(zip.central, 0);
    put16(zip.central, 0);
    put16(zip.central, 0);
    put16(zip.central, 0);
    put32(zip.central, 0);
    put32(zip.central, local_at);
    zip.central.insert(zip.central.end(), name.begin(), name.end());
    zip.count++;
  }
  void
  add_store(Built &zip, const std::string &name, const std::string &data) {
    add_entry(zip, name, data, 0, std::vector<std::uint8_t>(data.begin(), data.end()));
  }
  std::vector<std::uint8_t>
  finish(Built zip) {
    const auto cd_at = static_cast<std::uint32_t>(zip.local.size());
    zip.local.insert(zip.local.end(), zip.central.begin(), zip.central.end());
    put32(zip.local, 0x06054b50u);
    put16(zip.local, 0);
    put16(zip.local, 0);
    put16(zip.local, zip.count);
    put16(zip.local, zip.count);
    put32(zip.local, static_cast<std::uint32_t>(zip.central.size()));
    put32(zip.local, cd_at);
    put16(zip.local, 0);
    return zip.local;
  }
  fs::path
  write_zip(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
  }
  const char kSep = static_cast<char>(92);
  std::vector<std::uint8_t>
  package_zip(const std::string &version, const std::string &exe) {
    Built zip;
    const std::string root = std::string("Sparxie") + kSep;
    add_store(zip, root + "Sparxie.exe", exe);
    add_store(zip, root + "version.ini", "exe_path=app-" + version + kSep + "Sparxie.Desktop.exe\n");
    add_store(zip, root + "app-" + version + kSep + "Sparxie.Desktop.exe", "desktop");
    return finish(zip);
  }
  sparxie::Probes
  passing() {
    sparxie::Probes probes;
    probes.supplied = true;
    probes.os_known = true;
    probes.os_build = 26300;
    probes.os_x64 = true;
    probes.free_known = true;
    probes.free_bytes = 5ull << 30;
    probes.webview2_known = true;
    probes.webview2 = true;
    probes.webview2_version = "157.0.4341.42";
    probes.vcruntime_known = true;
    probes.vcruntime = true;
    probes.vcruntime_version = "14.51";
    probes.write_known = true;
    probes.can_write = true;
    probes.elevation_known = true;
    return probes;
  }
  sparxie::HostEnv
  env_at(const fs::path &root) {
    sparxie::HostEnv env;
    env.host_data = root / "host";
    env.data_dir = root / "data";
    fs::create_directories(env.host_data);
    return env;
  }
  std::string
  file_hash(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return sparxie::sha256::hash(bytes.data(), bytes.size());
  }
}  // namespace

TEST(InstallManifest, PinsTheKnownPackagesAndHasNoDownload) {
  const auto manifest = sparxie::manifest_json();
  EXPECT_FALSE(manifest["online"].get<bool>());
  EXPECT_EQ(manifest["publisher"], "暗檬");
  EXPECT_EQ(manifest["minOsBuild"], 19041);
  EXPECT_FALSE(manifest["requiresAdmin"].get<bool>());
  ASSERT_EQ(manifest["assets"].size(), 2u);
  EXPECT_EQ(manifest["assets"][0]["version"], "0.1.0-preview.9");
  EXPECT_EQ(manifest["assets"][0]["sha256"], "3e5591d7126aeb63972f30aaaf3f6649d695271312803454f7d217d79e903c7c");
  EXPECT_EQ(manifest["assets"][0]["bytes"], 100832503u);
  EXPECT_EQ(manifest["assets"][1]["sha256"], "5053db25f803c1637eda1ccdb156266210d38c185e1180dea7e976be7799db77");
  EXPECT_EQ(manifest["assets"][1]["bytes"], 103911941u);
  for (const auto &asset : manifest["assets"]) {
    EXPECT_FALSE(asset.contains("url"));
    EXPECT_FALSE(asset.contains("download"));
  }
  EXPECT_NE(manifest["hint"].get<std::string>().find("选择"), std::string::npos);
}

TEST(InstallZip, NormalisesBackslashesAndRejectsSlip) {
  const fs::path root = fs::temp_directory_path() / "sparxie-zip-slip";
  fs::remove_all(root);
  Built zip;
  add_store(zip, std::string("Sparxie") + kSep + "Sparxie.exe", "exe");
  add_store(zip, std::string("Sparxie") + kSep + ".." + kSep + "outside.txt", "nope");
  const auto bad = write_zip(root / "bad.zip", finish(zip));
  const auto slipped = sparxie::zip::extract(bad, root / "out");
  EXPECT_FALSE(slipped.ok);
  EXPECT_EQ(slipped.error_code, "zip_slip");
  EXPECT_FALSE(fs::exists(root / "outside.txt"));
  Built drive;
  add_store(drive, "C:evil.txt", "x");
  EXPECT_EQ(sparxie::zip::extract(write_zip(root / "drive.zip", finish(drive)), root / "out2").error_code, "zip_slip");
  const std::string plain = "hello sparxie\n";
  const std::uint8_t deflated[] = { 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0x28, 0x2e, 0x48, 0x2c, 0xaa, 0xc8, 0x4c, 0xe5, 0x02, 0x00 };
  Built deflate;
  add_entry(deflate, std::string("Sparxie") + kSep + "Sparxie.exe", plain, 8, std::vector<std::uint8_t>(deflated, deflated + sizeof(deflated)));
  const auto extracted = sparxie::zip::extract(write_zip(root / "deflate.zip", finish(deflate)), root / "deflated");
  EXPECT_TRUE(extracted.ok) << extracted.error;
  std::ifstream in(root / "deflated" / "Sparxie" / "Sparxie.exe");
  std::string got((std::istreambuf_iterator<char>(in)), {});
  EXPECT_EQ(got, plain);
}

TEST(InstallFlow, VerifiesPinsAsksForUnknownVersionsAndKeepsAppsSideBySide) {
  const fs::path root = fs::temp_directory_path() / "sparxie-install-flow";
  fs::remove_all(root);
  const auto bytes = package_zip("1.0.0", "one");
  const auto zip = write_zip(root / "one.zip", bytes);
  auto env = env_at(root);
  sparxie::InstallRequest req;
  req.zip = zip;
  req.dest = root / "components" / "sparxie";
  req.probes = passing();
  auto blocked = sparxie::install_from_file(env, req);
  EXPECT_EQ(blocked.status.error_code, "notice");
  EXPECT_FALSE(fs::exists(req.dest / "component.json"));
  req.acknowledged = true;
  auto unknown = sparxie::install_from_file(env, req);
  EXPECT_EQ(unknown.status.error_code, "unverified");
  EXPECT_EQ(unknown.body["confirm"], "未经验证的版本");
  EXPECT_FALSE(fs::exists(req.dest / "Sparxie.exe"));
  req.confirm_unverified = true;
  auto installed = sparxie::install_from_file(env, req);
  EXPECT_TRUE(installed.status.ok) << installed.status.error;
  EXPECT_FALSE(installed.body["verified"].get<bool>());
  EXPECT_EQ(installed.body["version"], "未经验证的版本");
  EXPECT_EQ(installed.body["mark"], "未经验证的版本");
  EXPECT_EQ(installed.body["app"], "app-1.0.0");
  EXPECT_TRUE(fs::is_regular_file(req.dest / "Sparxie.exe"));
  EXPECT_TRUE(fs::is_regular_file(req.dest / "app-1.0.0" / "Sparxie.Desktop.exe"));
  {
    std::ifstream ini(req.dest / "version.ini");
    std::string line;
    std::getline(ini, line);
    EXPECT_NE(line.find("app-1.0.0"), std::string::npos);
  }

  const auto second = write_zip(root / "two.zip", package_zip("2.0.0", "two"));
  const std::string hash = file_hash(second);
  req.zip = second;
  req.confirm_unverified = false;
  req.pins = { { "2.0.0", "two.zip", hash, fs::file_size(second), "test" } };
  auto known = sparxie::install_from_file(env, req);
  EXPECT_TRUE(known.status.ok) << known.status.error;
  EXPECT_TRUE(known.body["verified"].get<bool>());
  EXPECT_TRUE(fs::is_directory(req.dest / "app-1.0.0"));
  EXPECT_TRUE(fs::is_directory(req.dest / "app-2.0.0"));
  {
    std::ifstream component(req.dest / "component.json");
    sparxie::json body;
    component >> body;
    EXPECT_EQ(body["version"], "2.0.0");
    EXPECT_TRUE(body["verified"].get<bool>());
  }

  req.pins = { { "2.0.0", "two.zip", std::string(64, 'a'), fs::file_size(second), "bad" } };
  auto mismatch = sparxie::install_from_file(env, req);
  EXPECT_EQ(mismatch.status.error_code, "hash_mismatch");
  EXPECT_EQ(mismatch.body["confirm"], "未经验证的版本");
  req.confirm_unverified = true;
  auto forced = sparxie::install_from_file(env, req);
  EXPECT_TRUE(forced.status.ok) << forced.status.error;
  EXPECT_FALSE(forced.body["verified"].get<bool>());
  EXPECT_EQ(forced.body["version"], "未经验证的版本");
  EXPECT_EQ(forced.body["mark"], "未经验证的版本");
  req.confirm_unverified = false;

  std::ofstream config(req.dest / "config.json");
  config << "{\"schemaVersion\":1}";
  config.close();
  sparxie::UninstallRequest remove;
  remove.dest = req.dest;
  auto ask = sparxie::uninstall_component(remove);
  EXPECT_EQ(ask.status.error_code, "confirm_config");
  EXPECT_TRUE(fs::exists(req.dest / "Sparxie.exe"));
  remove.delete_config_set = true;
  remove.delete_config = false;
  auto kept = sparxie::uninstall_component(remove);
  EXPECT_TRUE(kept.status.ok) << kept.status.error;
  EXPECT_FALSE(fs::exists(req.dest / "Sparxie.exe"));
  EXPECT_FALSE(fs::exists(req.dest / "app-2.0.0"));
  EXPECT_TRUE(fs::is_regular_file(req.dest / "config.json"));
  remove.delete_config = true;
  auto gone = sparxie::uninstall_component(remove);
  EXPECT_TRUE(gone.status.ok);
  EXPECT_FALSE(fs::exists(req.dest));
}

TEST(InstallFlow, StopsForTheDependencyFailures) {
  const fs::path root = fs::temp_directory_path() / "sparxie-install-deps";
  fs::remove_all(root);
  const auto zip = write_zip(root / "pkg.zip", package_zip("9.0.0", "exe"));
  auto env = env_at(root);
  sparxie::InstallRequest req;
  req.zip = zip;
  req.dest = root / "dest";
  req.acknowledged = true;
  req.confirm_unverified = true;
  req.probes = passing();
  req.probes.os_build = 10240;
  EXPECT_EQ(sparxie::install_from_file(env, req).status.error_code, "os_too_old");
  req.probes = passing();
  req.probes.os_x64 = false;
  EXPECT_EQ(sparxie::install_from_file(env, req).status.error_code, "os_too_old");
  req.probes = passing();
  req.probes.webview2 = false;
  EXPECT_EQ(sparxie::install_from_file(env, req).status.error_code, "webview2_missing");
  req.probes = passing();
  req.probes.vcruntime = false;
  EXPECT_EQ(sparxie::install_from_file(env, req).status.error_code, "vcruntime_missing");
  req.probes = passing();
  req.probes.can_write = false;
  req.probes.can_elevate = true;
  EXPECT_EQ(sparxie::install_from_file(env, req).status.error_code, "no_admin");
  req.probes = passing();
  req.probes.free_bytes = 16;
  EXPECT_EQ(sparxie::install_from_file(env, req).status.error_code, "disk_full");
  req.probes = passing();
  req.probes.isolated = [](const fs::path &) { return true; };
  EXPECT_EQ(sparxie::install_from_file(env, req).status.error_code, "quarantined");
  EXPECT_FALSE(fs::exists(req.dest / "component.json"));
  req.probes = passing();
  req.running = true;
  EXPECT_EQ(sparxie::install_from_file(env, req).status.error_code, "sparxie_running");
}

TEST(InstallFlow, FindsTheCopyUnderTheDataRootBeforeTheInstallRoot) {
  const fs::path root = fs::temp_directory_path() / "sparxie-install-search";
  fs::remove_all(root);
  fs::create_directories(root / "program" / "components" / "sparxie");
  fs::create_directories(root / "appdata" / "components" / "sparxie");
  {
    std::ofstream out(root / "program" / "components" / "sparxie" / "Sparxie.exe");
    out << "program";
    std::ofstream meta(root / "program" / "components" / "sparxie" / "component.json");
    meta << "{\"version\":\"from-program\"}";
  }
  {
    std::ofstream out(root / "appdata" / "components" / "sparxie" / "Sparxie.exe");
    out << "data";
    std::ofstream meta(root / "appdata" / "components" / "sparxie" / "component.json");
    meta << "{\"version\":\"from-data\"}";
  }
  sparxie::HostEnv env = env_at(root);
  env.install_root = root / "program";
  env.component_dir = root / "appdata" / "components" / "sparxie";
  const auto status = sparxie::component_status(env);
  EXPECT_TRUE(status.body["installed"].get<bool>());
  EXPECT_EQ(status.body["version"], "from-data");
  EXPECT_TRUE(status.body.contains("dependencies"));
  EXPECT_TRUE(status.body.contains("installHint"));
}

TEST(InstallFlow, AJobReportsDone) {
  const fs::path root = fs::temp_directory_path() / "sparxie-install-job";
  fs::remove_all(root);
  const auto zip = write_zip(root / "pkg.zip", package_zip("3.0.0", "job"));
  auto env = env_at(root);
  sparxie::InstallRequest req;
  req.zip = zip;
  req.dest = root / "dest";
  req.acknowledged = true;
  req.confirm_unverified = true;
  req.probes = passing();
  const std::string id = sparxie::start_install_job(env, req);
  ASSERT_FALSE(id.empty());
  bool done = false;
  sparxie::json body;
  for (int i = 0; i < 200 && !done; ++i) {
    body = sparxie::install_job(id);
    done = body.value("done", false);
    if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_TRUE(done);
  EXPECT_TRUE(body["ok"].get<bool>()) << body.dump();
  EXPECT_TRUE(fs::is_regular_file(req.dest / "Sparxie.exe"));
}

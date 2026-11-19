#include "src/sparxie/settings.h"
#include "src/sparxie/zzz_file.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
namespace fs = std::filesystem;

namespace {
  const char *kFixture =
    R"({
    "$Type" : "MoleMole.GeneralLocalDataItem",
    "deviceUUID" : "",
    "userLocalDataVersionId" : "0.0.1",
    "curAccountName"         : "fixture-name",
    "uid"                    : 0,
    "PlayerPrefs_StringContainer" : {
        "__fixture__" : "keep"
    },
    "SystemSettingDataMap"     : {
        "3" : {
            "$Type" : "MoleMole.SystemSettingLocalData",
            "Version" : 0,
            "Data" : 3
        },
        "14" : {
            "$Type" : "MoleMole.SystemSettingLocalData",
            "Version" : 0,
            "Data" : 1
        },
        "110" : {
            "$Type" : "MoleMole.SystemSettingLocalData",
            "Version" : 0,
            "Data" : 2
        }
    },
    "KeyboardBindingMap" : {
    },
    "HDRSettingRecordState"   : 0,
    "LastVHSStoreOpenTime"    : "11/09/2026 07:02:28",
    "DisableBattleUIOptimization"       : false
}
)";

  std::vector<std::uint8_t>
  hex_bytes(const char *hex) {
    std::vector<std::uint8_t> out;
    const auto digit = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    for (const char *p = hex; p[0] && p[1]; p += 2)
      out.push_back(static_cast<std::uint8_t>(digit(p[0]) << 4 | digit(p[1])));
    return out;
  }

  // The key shipped in assets/sparxie/zzz-magic.bin.
  const std::vector<std::uint8_t> kShippedMagic = hex_bytes("556ed19674d183ce956e67696ed0b52e47d0b06d65ce9f626a65d18174");
  const std::vector<std::uint8_t> kOtherMagic { 0x01, 0xC0, 0x22, 0x05, 0x7F };

  std::vector<std::uint8_t>
  read_bytes(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }

  void
  write_bytes(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  fs::path
  fixture_file(const fs::path &dir, const std::vector<std::uint8_t> &magic = kShippedMagic) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / "GENERAL_DATA.bin";
    write_bytes(file, sparxie::sleepy_write(kFixture, magic));
    return file;
  }

  sparxie::json
  parse_file(const fs::path &file, const std::vector<std::uint8_t> &magic) {
    return sparxie::json::parse(sparxie::sleepy_read(read_bytes(file), magic));
  }
}  // namespace

TEST(ZzzFile, ReadsTheFiveValuesAndTheGameDefaults) {
  const fs::path root = fs::temp_directory_path() / "sparxie-zzz-read";
  fs::remove_all(root);
  const fs::path file = fixture_file(root);
  sparxie::GameValues values;
  values.fps = 999;
  ASSERT_TRUE(sparxie::zzz_read_general_data(file, { kShippedMagic }, values));
  EXPECT_EQ(values.fps, 0) << "the fixture stores the unlimited tier";
  EXPECT_TRUE(values.vsync) << "an entry the person never changed in-game keeps the game default";
  EXPECT_DOUBLE_EQ(values.render_scale, 1.0);
  EXPECT_EQ(values.preset, "custom");
  EXPECT_FALSE(values.hdr);
}

TEST(ZzzFile, NoEditWriteIsByteExact) {
  const fs::path root = fs::temp_directory_path() / "sparxie-zzz-byte-exact";
  fs::remove_all(root);
  const fs::path file = fixture_file(root);
  const std::vector<std::uint8_t> before = read_bytes(file);
  sparxie::GameValues values;
  std::string error;
  // An empty field set means every field; a registry-only choice writes none of the file fields.
  ASSERT_TRUE(sparxie::zzz_write_general_data(file, { kShippedMagic }, values, { "resolution" }, error)) << error;
  EXPECT_EQ(read_bytes(file), before) << "writing no field must put the same bytes back";
}

TEST(ZzzFile, WriteChangesOnlyTheStoredNumbers) {
  const fs::path root = fs::temp_directory_path() / "sparxie-zzz-write";
  fs::remove_all(root);
  const fs::path file = fixture_file(root);
  const sparxie::json before = parse_file(file, kShippedMagic);
  sparxie::GameValues values;
  values.fps = 60;
  values.vsync = false;
  values.render_scale = 1.2;
  values.preset = "medium";
  values.hdr = true;
  std::string error;
  ASSERT_TRUE(sparxie::zzz_write_general_data(file, { kShippedMagic }, values, { "fps", "vsync", "renderScale", "preset", "hdr" }, error)) << error;
  const sparxie::json after = parse_file(file, kShippedMagic);
  EXPECT_EQ(after["SystemSettingDataMap"]["110"]["Data"], 1);
  EXPECT_EQ(after["SystemSettingDataMap"]["8"]["Data"], 0) << "vsync was absent and is added";
  EXPECT_EQ(after["SystemSettingDataMap"]["9"]["Data"], 2) << "render scale was absent and is added";
  EXPECT_EQ(after["SystemSettingDataMap"]["3"]["Data"], 1);
  EXPECT_EQ(after["HDRSettingRecordState"], 1);
  EXPECT_EQ(after["SystemSettingDataMap"]["14"], before["SystemSettingDataMap"]["14"]) << "an untouched entry keeps its value";
  EXPECT_EQ(after["curAccountName"], "fixture-name");
  const std::string text = sparxie::sleepy_read(read_bytes(file), kShippedMagic);
  EXPECT_NE(text.find("\"curAccountName\"         : \"fixture-name\","), std::string::npos) << "the game's own padding survives the write";
  EXPECT_NE(text.find("    \"KeyboardBindingMap\" : {\n    },"), std::string::npos) << "the empty binding map keeps its bytes";
}

TEST(ZzzFile, TheValueDomainsOfEveryTier) {
  const fs::path root = fs::temp_directory_path() / "sparxie-zzz-domains";
  fs::remove_all(root);
  const fs::path file = fixture_file(root);
  auto stored = [&](const char *key) {
    return parse_file(file, kShippedMagic)["SystemSettingDataMap"][key]["Data"].get<int>();
  };
  sparxie::GameValues values;
  std::string error;
  for (const auto &[fps, tier] : std::vector<std::pair<int, int>> { { 30, 0 }, { 60, 1 }, { 0, 2 } }) {
    values.fps = fps;
    ASSERT_TRUE(sparxie::zzz_write_general_data(file, { kShippedMagic }, values, { "fps" }, error)) << error;
    EXPECT_EQ(stored("110"), tier);
  }
  for (const auto &[scale, tier] : std::vector<std::pair<double, int>> { { 0.8, 0 }, { 1.0, 1 }, { 1.2, 2 } }) {
    values.render_scale = scale;
    ASSERT_TRUE(sparxie::zzz_write_general_data(file, { kShippedMagic }, values, { "renderScale" }, error)) << error;
    EXPECT_EQ(stored("9"), tier);
  }
  for (const auto &[preset, tier] : std::vector<std::pair<const char *, int>> { { "high", 0 }, { "medium", 1 }, { "low", 2 }, { "custom", 3 } }) {
    values.preset = preset;
    ASSERT_TRUE(sparxie::zzz_write_general_data(file, { kShippedMagic }, values, { "preset" }, error)) << error;
    EXPECT_EQ(stored("3"), tier);
  }
  values.vsync = false;
  ASSERT_TRUE(sparxie::zzz_write_general_data(file, { kShippedMagic }, values, { "vsync" }, error)) << error;
  EXPECT_EQ(stored("8"), 0);
  values.hdr = true;
  ASSERT_TRUE(sparxie::zzz_write_general_data(file, { kShippedMagic }, values, { "hdr" }, error)) << error;
  EXPECT_EQ(parse_file(file, kShippedMagic)["HDRSettingRecordState"].get<int>(), 1);
}

TEST(ZzzFile, AFileTheKeyDoesNotDecodeIsNeverTouched) {
  const fs::path root = fs::temp_directory_path() / "sparxie-zzz-locked";
  fs::remove_all(root);
  const fs::path file = fixture_file(root, kOtherMagic);
  const std::vector<std::uint8_t> before = read_bytes(file);
  sparxie::GameValues values;
  EXPECT_FALSE(sparxie::zzz_read_general_data(file, { kShippedMagic }, values));
  std::string error;
  EXPECT_FALSE(sparxie::zzz_write_general_data(file, { kShippedMagic }, values, { "fps" }, error));
  EXPECT_FALSE(error.empty()) << "the refusal carries a reason for the read-only state";
  EXPECT_EQ(read_bytes(file), before);
  EXPECT_FALSE(fs::exists(file.parent_path() / "GENERAL_DATA.bin.sparxie-tmp")) << "no temporary file is left behind";
}

TEST(ZzzFile, AMissingFileIsRefused) {
  sparxie::GameValues values;
  std::string error;
  EXPECT_FALSE(sparxie::zzz_write_general_data(fs::temp_directory_path() / "sparxie-zzz-absent" / "GENERAL_DATA.bin", { kShippedMagic }, values, { "fps" }, error));
  EXPECT_NE(error.find("has not written"), std::string::npos);
}

TEST(ZzzMagics, OverrideAndShippedKeys) {
  const fs::path root = fs::temp_directory_path() / "sparxie-zzz-magics";
  fs::remove_all(root);
  const fs::path host_data = root / "host";
  const fs::path assets = root / "assets";
  fs::create_directories(assets / "sparxie");
  write_bytes(assets / "sparxie" / "zzz-magic.bin", kShippedMagic);
  auto loaded = sparxie::load_zzz_magics(host_data, { assets });
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0], kShippedMagic);
  write_bytes(assets / "sparxie" / "zzz-magic.cn.bin", kOtherMagic);
  loaded = sparxie::load_zzz_magics(host_data, { assets });
  ASSERT_EQ(loaded.size(), 2u) << "a future per-region key ships as a file";
  fs::create_directories(host_data);
  write_bytes(host_data / "zzz-magic.bin", kOtherMagic);
  loaded = sparxie::load_zzz_magics(host_data, { assets });
  ASSERT_EQ(loaded.size(), 1u) << "one explicit key replaces the shipped set";
  EXPECT_EQ(loaded[0], kOtherMagic);
}

TEST(Settings, ZzzWritesAndReadsThroughTheHostFlow) {
  const fs::path root = fs::temp_directory_path() / "sparxie-zzz-flow";
  fs::remove_all(root);
  fixture_file(root / "game" / "ZenlessZoneZero_Data" / "Persistent" / "LocalStorage");
  sparxie::MemoryRegistry reg;
  sparxie::Install install;
  install.game = "zzz";
  install.region = "global";
  install.id = "zzz-global";
  install.dir = (root / "game").string();
  const std::vector<std::vector<std::uint8_t>> magics { kShippedMagic };
  sparxie::GameValues values;
  values.fps = 30;
  values.vsync = false;
  values.render_scale = 0.8;
  values.preset = "low";
  values.hdr = true;
  const fs::path file = root / "game" / "ZenlessZoneZero_Data" / "Persistent" / "LocalStorage" / "GENERAL_DATA.bin";
  const std::vector<std::uint8_t> original = read_bytes(file);
  const fs::path backup = root / "backup";
  auto refused = sparxie::write_game_settings(install, reg, values, magics, [] { return true; }, backup);
  EXPECT_EQ(refused.status.error_code, "game_running");
  EXPECT_EQ(read_bytes(file), original) << "a running game means no write at all";
  const std::set<std::string> fields { "fps", "vsync", "renderScale", "preset", "hdr" };
  auto wrote = sparxie::write_game_settings(install, reg, values, magics, [] { return false; }, backup, "restore", fields);
  ASSERT_TRUE(wrote.status.ok) << wrote.status.error;
  EXPECT_EQ(wrote.applied, (std::vector<std::string> { "fps", "vsync", "renderScale", "preset", "hdr" }));
  const sparxie::json stored = parse_file(file, kShippedMagic);
  EXPECT_EQ(stored["SystemSettingDataMap"]["110"]["Data"], 0);
  EXPECT_EQ(stored["SystemSettingDataMap"]["8"]["Data"], 0);
  EXPECT_EQ(stored["SystemSettingDataMap"]["9"]["Data"], 0);
  EXPECT_EQ(stored["SystemSettingDataMap"]["3"]["Data"], 2);
  EXPECT_EQ(stored["HDRSettingRecordState"], 1);
  EXPECT_TRUE(fs::exists(backup / "zzz-global.general-data.before")) << "the previous bytes are backed up";
  auto read = sparxie::read_game_settings(install, reg, magics, [] { return false; });
  EXPECT_EQ(read.values.fps, 30);
  EXPECT_FALSE(read.values.vsync);
  EXPECT_DOUBLE_EQ(read.values.render_scale, 0.8);
  EXPECT_EQ(read.values.preset, "low");
  EXPECT_TRUE(read.values.hdr);
  EXPECT_TRUE(read.unavailable.empty());
  sparxie::reconcile_pending_settings(install, reg, backup, magics);
  EXPECT_EQ(read_bytes(file), original) << "a restore policy puts the previous bytes back while the values are unchanged";
}

TEST(Settings, AnUndecodableFileReportsTheItemsUnavailable) {
  const fs::path root = fs::temp_directory_path() / "sparxie-zzz-unreadable";
  fs::remove_all(root);
  fixture_file(root / "game" / "ZenlessZoneZero_Data" / "Persistent" / "LocalStorage", kOtherMagic);
  sparxie::MemoryRegistry reg;
  sparxie::Install install;
  install.game = "zzz";
  install.region = "global";
  install.id = "zzz-global";
  install.dir = (root / "game").string();
  const fs::path file = root / "game" / "ZenlessZoneZero_Data" / "Persistent" / "LocalStorage" / "GENERAL_DATA.bin";
  const std::vector<std::uint8_t> original = read_bytes(file);
  const std::vector<std::vector<std::uint8_t>> magics { kShippedMagic };
  auto read = sparxie::read_game_settings(install, reg, magics, [] { return false; });
  EXPECT_TRUE(read.status.ok);
  for (const char *field : { "fps", "vsync", "renderScale", "preset", "hdr" })
    EXPECT_NE(std::find(read.unavailable.begin(), read.unavailable.end(), std::string(field)), read.unavailable.end()) << field;
  sparxie::GameValues values;
  values.fps = 60;
  auto wrote = sparxie::write_game_settings(install, reg, values, magics, [] { return false; }, {}, "restore", { "fps" });
  EXPECT_TRUE(wrote.status.ok) << wrote.status.error;
  EXPECT_EQ(wrote.applied.size(), 0u);
  EXPECT_NE(std::find(wrote.unavailable.begin(), wrote.unavailable.end(), std::string("fps")), wrote.unavailable.end());
  EXPECT_EQ(read_bytes(file), original) << "the file of a newer game version is never corrupted";
}

TEST(ZzzRealFile, ByteExactRoundTrip) {
  const char *path = std::getenv("SPARXIE_ZZZ_REAL_FILE");
  if (path == nullptr || *path == '\0') {
    std::cout << "SPARXIE_ZZZ_REAL_FILE is not set; the real-file round trip did not run\n";
    return;
  }
  const fs::path dir = fs::temp_directory_path() / "sparxie-zzz-real";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path file = dir / "GENERAL_DATA.bin";
  fs::copy_file(path, file, fs::copy_options::overwrite_existing);
  const std::vector<std::uint8_t> before = read_bytes(file);
  const std::vector<std::vector<std::uint8_t>> magics { kShippedMagic };
  sparxie::GameValues values;
  ASSERT_TRUE(sparxie::zzz_read_general_data(file, magics, values));
  std::string error;
  ASSERT_TRUE(sparxie::zzz_write_general_data(file, magics, values, { "resolution" }, error)) << error;
  EXPECT_EQ(read_bytes(file), before) << "a no-edit write must reproduce the game's own bytes";
  const sparxie::json before_doc = parse_file(file, kShippedMagic);
  values.fps = 60;
  ASSERT_TRUE(sparxie::zzz_write_general_data(file, magics, values, { "fps" }, error)) << error;
  const sparxie::json after_doc = parse_file(file, kShippedMagic);
  sparxie::json expected = before_doc;
  expected["SystemSettingDataMap"]["110"]["Data"] = 1;
  EXPECT_EQ(after_doc, expected) << "only the frame-rate number changed";
}

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include "src/sparxie/store.h"
namespace fs = std::filesystem;
using sparxie::json;

// The contract between the host's writer (src/sparxie/store.cpp) and Sparxie's reader (AppConfigValidator.cs, Sparxie.Infrastructure/Configuration).
// 2026-11-12: the host wrote endfieldWidth/Height 0 for "follow the stream", Sparxie rejected the file and moved all seven profiles of a test host aside.
// The checker below is written from AppConfigValidator on purpose and does not call sparxie::validate_document; Sparxie's Config.Tests read the same
// fixture (host-written-config-20261112.json) with the real validator.
namespace {
  std::string
  lower(std::string text) {
    for (char &c : text)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
  }
  bool
  exe_allowed(const std::string &game, const std::string &path) {
    const std::size_t cut = path.find_last_of("/\\");  // Path.GetFileName on Windows
    const std::string name = lower(cut == std::string::npos ? path : path.substr(cut + 1));
    if (game == "genshin") return name == "yuanshen.exe" || name == "genshinimpact.exe";
    if (game == "starRail") return name == "starrail.exe";
    if (game == "zenlessZoneZero") return name == "zenlesszonezero.exe" || name == "zenlesszonezerobeta.exe";
    if (game == "endfield") return name == "endfield.exe";
    return false;
  }
  // AppConfigValidator.Validate plus what System.Text.Json needs to read the types (a wrong type makes the whole file unreadable)
  std::vector<std::string>
  sparxie_rejects(const json &config) {
    std::vector<std::string> errors;
    if (!config.is_object() || !config.contains("schemaVersion") || config["schemaVersion"] != 1) {
      errors.push_back("schemaVersion is not 1");
      return errors;
    }
    if (!config.contains("profiles") || !config["profiles"].is_array()) {
      errors.push_back("profiles is not a list");
      return errors;
    }
    std::set<std::string> ids;
    for (const auto &profile : config["profiles"]) {
      const std::string id = profile.value("id", std::string());
      auto bad = [&](const std::string &why) { errors.push_back(id + ": " + why); };
      if (id.empty()) bad("empty id");
      if (!ids.insert(id).second) bad("duplicate id");
      if (profile.value("displayName", std::string()).empty()) bad("empty displayName");
      const std::string game = profile.value("game", std::string());
      if (game != "genshin" && game != "starRail" && game != "zenlessZoneZero" && game != "endfield") bad("game " + game);
      const std::string exe = profile.value("executablePath", std::string());
      if (!exe.empty() && !exe_allowed(game, exe)) bad("exe not on the whitelist: " + exe);
      if (game == "zenlessZoneZero") {
        if (profile.contains("hoyo") && !profile["hoyo"].is_null()) bad("zzz has hoyo");
        continue;
      }
      if (!profile.contains("hoyo") || profile["hoyo"].is_null()) continue;
      const json &hoyo = profile["hoyo"];
      if (!hoyo.is_object()) {
        bad("hoyo is not an object");
        continue;
      }
      for (const char *field : { "touchUiEnabled", "fpsUnlockEnabled", "backgroundFpsLimitEnabled", "genshinFollowInGamePreset", "genshinTouchUiScaleOverrideEnabled", "endfieldAutoResolution" })
        if (hoyo.contains(field) && !hoyo[field].is_boolean()) bad(std::string(field) + " is not a boolean");
      for (const char *field : { "targetFps", "backgroundFps", "genshinPreset30Fps", "genshinPreset60Fps", "genshinTouchUiScalePercent", "endfieldWidth", "endfieldHeight", "endfieldRefreshRate" })
        if (hoyo.contains(field) && !hoyo[field].is_number_integer()) bad(std::string(field) + " is not an integer");
      for (const char *field : { "processPriority", "endfieldInputMode" })
        if (hoyo.contains(field) && !hoyo[field].is_string()) bad(std::string(field) + " is not text");
      for (const char *field : { "targetFps", "backgroundFps", "genshinPreset30Fps", "genshinPreset60Fps" }) {
        const int fps = hoyo.value(field, 120);
        if (fps < 10 || fps > 1000) bad(std::string(field) + " outside 10-1000");
      }
      const int scale = hoyo.value("genshinTouchUiScalePercent", 400);
      if (scale < 100 || scale > 500) bad("genshinTouchUiScalePercent outside 100-500");
      const std::string priority = hoyo.value("processPriority", std::string("normal"));
      if (priority != "normal" && priority != "belowNormal" && priority != "aboveNormal" && priority != "high") bad("processPriority " + priority);
      if (game == "endfield") {
        const std::string input = hoyo.value("endfieldInputMode", std::string("touch"));
        if (input != "touch" && input != "pc") bad("endfieldInputMode " + input);
        // newer Sparxie builds: the size is only checked while it is fixed
        if (!hoyo.value("endfieldAutoResolution", true)) {
          const int w = hoyo.value("endfieldWidth", 1920), h = hoyo.value("endfieldHeight", 1080);
          if (w < 320 || w > 7680 || h < 240 || h > 4320) bad("endfield size outside 320x240-7680x4320");
        }
        const int refresh = hoyo.value("endfieldRefreshRate", 60);
        if (refresh < 24 || refresh > 1000) bad("endfieldRefreshRate outside 24-1000");
      }
    }
    return errors;
  }
  // A host writer must keep a stricter promise than Sparxie's: the Endfield size is valid even while it follows the stream (an older Sparxie checks it always)
  std::vector<std::string>
  host_extra_rejects(const json &config) {
    std::vector<std::string> errors;
    for (const auto &profile : config["profiles"]) {
      if (!profile.contains("hoyo") || !profile["hoyo"].is_object()) continue;
      const json &hoyo = profile["hoyo"];
      if (!hoyo.contains("endfieldWidth")) continue;
      const int w = hoyo.value("endfieldWidth", 0), h = hoyo.value("endfieldHeight", 0);
      if (w < 320 || w > 7680 || h < 240 || h > 4320) errors.push_back(profile.value("id", std::string()) + ": size " + std::to_string(w) + "x" + std::to_string(h) + " would be rejected by older Sparxie builds");
    }
    return errors;
  }
  std::string
  read_text(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
  }
  fs::path
  fixture(const char *name) { return fs::path(SPARXIE_FIXTURES_DIR) / name; }
  struct Case {
    const char *name;
    json options;
    int display_fps;
  };
  std::vector<Case>
  cases() {
    return {
      { "defaults", json::object(), 0 },
      { "auto-144", json { { "resolution", "auto" } }, 144 },
      { "empty-resolution", json { { "resolution", "" } }, 60 },
      { "fixed-2560", json { { "resolution", "2560\xC3\x97" "1440" } }, 120 },
      { "fixed-ascii", json { { "resolution", "1920x1080" } }, 60 },
      { "fixed-zero", json { { "resolution", "0\xC3\x97" "0" } }, 60 },
      { "fixed-huge", json { { "resolution", "99999\xC3\x97" "99999" } }, 60 },
      { "fixed-tiny", json { { "resolution", "100x100" } }, 60 },
      { "garbage-resolution", json { { "resolution", "garbage" } }, 60 },
      { "kbm-low", json { { "input", "kbm" }, { "fps", 5 }, { "scale", 50 }, { "priority", "turbo" } }, 0 },
      { "high-values", json { { "fps", 5000 }, { "scale", 900 }, { "bgFps", 0 }, { "preset30", -1 }, { "preset60", 99999 }, { "priority", "high" } }, 0 },
      { "fps-display", json { { "fps", "display" } }, 2000 },
      { "refresh-tiny", json::object(), 3 },
      { "priorities", json { { "priority", "belowNormal" }, { "followPreset", true }, { "scaleOverride", true }, { "scale", 250 }, { "bgLimit", false }, { "fpsUnlock", true } }, 240 },
      { "above-normal", json { { "priority", "aboveNormal" }, { "input", "touch" }, { "preset30", 30 }, { "preset60", 60 } }, 90 },
    };
  }
  const std::vector<std::pair<std::string, std::string>> kGames = {
    { "genshin", "YuanShen.exe" }, { "starrail", "StarRail.exe" }, { "zzz", "ZenlessZoneZero.exe" }, { "endfield", "Endfield.exe" },
  };
  sparxie::StoreHooks
  quiet_hooks() {
    sparxie::StoreHooks hooks;
    hooks.utc_stamp = [] { return std::string("20261112T000000Z"); };
    return hooks;
  }
}  // namespace

// Every game and option the host can write, one profile each, through add_profile into one file: Sparxie must accept the file as it is.
TEST(SparxieContract, EveryHostWrittenProfileIsAcceptedBySparxie) {
  const fs::path dir = fs::temp_directory_path() / "sparxie-contract-1";
  fs::remove_all(dir);
  fs::create_directories(dir);
  for (const auto &[game, exe] : kGames) {
    for (const Case &c : cases()) {
      sparxie::ProfileDraft draft;
      draft.id = game + "-" + c.name;
      draft.display_name = draft.id;
      draft.game = game;
      draft.executable = "C:\\Games\\" + exe;
      draft.options = c.options;
      draft.display_fps = c.display_fps;
      auto wrote = sparxie::add_profile(dir, draft, quiet_hooks());
      ASSERT_TRUE(wrote.status.ok) << draft.id << ": " << wrote.status.error;
      const auto view = sparxie::read_store(dir);
      const auto rejected = sparxie_rejects(view.document);
      ASSERT_TRUE(rejected.empty()) << draft.id << ": " << rejected.front();
      const auto strict = host_extra_rejects(view.document);
      ASSERT_TRUE(strict.empty()) << draft.id << ": " << strict.front();
    }
  }
  const auto view = sparxie::read_store(dir);
  EXPECT_EQ(view.document["profiles"].size(), kGames.size() * cases().size());
  EXPECT_TRUE(sparxie::validate_document(view.document).ok);
  // the shape of auto: the flag is true and the size is a value Sparxie accepts, not 0
  for (const auto &profile : view.document["profiles"]) {
    if (profile["game"] == "zenlessZoneZero") {
      EXPECT_FALSE(profile.contains("hoyo"));
      continue;
    }
    const json &hoyo = profile["hoyo"];
    if (hoyo["endfieldAutoResolution"].get<bool>()) {
      EXPECT_EQ(hoyo["endfieldWidth"], 1920) << profile["id"];
      EXPECT_EQ(hoyo["endfieldHeight"], 1080) << profile["id"];
    }
  }
  // the same document is the fixture Sparxie's Config.Tests load with the real AppConfigValidator (SPARXIE_UPDATE_FIXTURES=1 rewrites it)
  const fs::path path = fixture("host-written-config-20261112.json");
  if (std::getenv("SPARXIE_UPDATE_FIXTURES")) {
    std::ofstream out(path, std::ios::binary);
    out << view.document.dump(2) << "\n";
  }
  ASSERT_TRUE(fs::exists(path)) << "run once with SPARXIE_UPDATE_FIXTURES=1";
  EXPECT_EQ(json::parse(read_text(path)), view.document) << "the host writes something else than the committed fixture";
}

// Auto keeps the size the profile already holds when Sparxie accepts it, and Sparxie's default otherwise.
TEST(SparxieContract, AutoKeepsTheExistingValidSizeAndNeverWritesZero) {
  const fs::path dir = fs::temp_directory_path() / "sparxie-contract-2";
  fs::remove_all(dir);
  fs::create_directories(dir);
  sparxie::ProfileDraft draft;
  draft.id = "endfield-cn-1";
  draft.display_name = "Endfield";
  draft.game = "endfield";
  draft.executable = "C:\\Games\\Endfield.exe";
  draft.options = json { { "resolution", "2560\xC3\x97" "1440" } };
  ASSERT_TRUE(sparxie::add_profile(dir, draft, quiet_hooks()).status.ok);
  draft.options = json { { "resolution", "auto" } };
  auto hooks = quiet_hooks();
  hooks.utc_stamp = [] { return std::string("20261112T000001Z"); };
  ASSERT_TRUE(sparxie::update_profile(dir, "endfield-cn-1", draft, hooks).status.ok);
  auto view = sparxie::read_store(dir);
  EXPECT_EQ(view.document["profiles"][0]["hoyo"]["endfieldAutoResolution"], true);
  EXPECT_EQ(view.document["profiles"][0]["hoyo"]["endfieldWidth"], 2560);
  EXPECT_EQ(view.document["profiles"][0]["hoyo"]["endfieldHeight"], 1440);
  EXPECT_TRUE(sparxie_rejects(view.document).empty());
  // a profile that already has the bad 0 x 0 gets Sparxie's default back
  view.document["profiles"][0]["hoyo"]["endfieldWidth"] = 0;
  view.document["profiles"][0]["hoyo"]["endfieldHeight"] = 0;
  {
    std::ofstream out(dir / "config.json", std::ios::binary);
    out << view.document.dump(2);
  }
  hooks.utc_stamp = [] { return std::string("20261112T000002Z"); };
  auto again = sparxie::update_profile(dir, "endfield-cn-1", draft, hooks);
  ASSERT_TRUE(again.status.ok) << again.status.error;
  view = sparxie::read_store(dir);
  EXPECT_EQ(view.document["profiles"][0]["hoyo"]["endfieldWidth"], 1920);
  EXPECT_EQ(view.document["profiles"][0]["hoyo"]["endfieldHeight"], 1080);
  EXPECT_TRUE(sparxie_rejects(view.document).empty());
}

// The captured config that Sparxie moved aside (config.invalid-20261112095137-d8b536fa.json): the host can write to it again, and every profile survives.
TEST(SparxieContract, TheRejectedFileWith0x0SizesIsRepairedByTheNextWrite) {
  const fs::path dir = fs::temp_directory_path() / "sparxie-contract-3";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const json captured = json::parse(read_text(fixture("sample-config-endfield-0x0-20261112.json")));
  ASSERT_EQ(captured["profiles"].size(), 7u);
  EXPECT_FALSE(host_extra_rejects(captured).empty()) << "the fixture must hold the 0x0 sizes that older Sparxie builds rejected";
  {
    std::ofstream out(dir / "config.json", std::ios::binary);
    out << captured.dump(2);
  }
  sparxie::ProfileDraft draft;
  draft.id = "endfield-cn-1";
  draft.display_name = "Endfield";
  draft.game = "endfield";
  draft.executable = "C:\\Games\\Endfield\\Endfield.exe";
  draft.options = json { { "resolution", "auto" }, { "input", "touch" } };
  draft.display_fps = 120;
  auto wrote = sparxie::update_profile(dir, "endfield-cn-1", draft, quiet_hooks());
  ASSERT_TRUE(wrote.status.ok) << wrote.status.error;
  const auto view = sparxie::read_store(dir);
  EXPECT_EQ(view.document["profiles"].size(), 7u);
  EXPECT_TRUE(sparxie_rejects(view.document).empty()) << sparxie_rejects(view.document).front();
  EXPECT_TRUE(host_extra_rejects(view.document).empty());
  // the original is next to it as a backup
  EXPECT_TRUE(fs::exists(dir / wrote.backup));
}

TEST(SparxieContract, TheHostValidatorKnowsTheSameRulesAsSparxie) {
  json doc = { { "schemaVersion", 1 },
               { "selectedProfileId", "" },
               { "profiles", json::array({ json { { "id", "e" }, { "displayName", "E" }, { "game", "endfield" }, { "executablePath", "C:/Endfield.exe" },
                                                  { "hoyo", { { "endfieldAutoResolution", false }, { "endfieldWidth", 0 }, { "endfieldHeight", 0 } } } } }) } };
  EXPECT_FALSE(sparxie_rejects(doc).empty());
  EXPECT_FALSE(sparxie::validate_document(doc).ok);
  doc["profiles"][0]["hoyo"]["endfieldAutoResolution"] = true;  // newer Sparxie builds: not looked at while the size follows the stream
  EXPECT_TRUE(sparxie_rejects(doc).empty());
  EXPECT_TRUE(sparxie::validate_document(doc).ok);
  doc["profiles"][0]["hoyo"]["endfieldInputMode"] = "auto";
  EXPECT_FALSE(sparxie_rejects(doc).empty());
  EXPECT_FALSE(sparxie::validate_document(doc).ok);
  doc["profiles"][0]["hoyo"]["endfieldInputMode"] = "pc";
  doc["profiles"][0]["hoyo"]["endfieldRefreshRate"] = 20;
  EXPECT_FALSE(sparxie_rejects(doc).empty());
  EXPECT_FALSE(sparxie::validate_document(doc).ok);
  doc["profiles"][0]["hoyo"]["endfieldRefreshRate"] = 60;
  doc["profiles"][0]["executablePath"] = "C:/Other.exe";
  EXPECT_FALSE(sparxie_rejects(doc).empty());
  EXPECT_FALSE(sparxie::validate_document(doc).ok);
}

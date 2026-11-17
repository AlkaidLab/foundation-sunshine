#pragma once
#include "src/sparxie/model.h"
#include "src/sparxie/storefront.h"

namespace sparxie {
  // One row of the region table: how a game's region is named and detected.
  // Biz tokens, channel pairs and the Bilibili SDK marker are the launcher
  // behaviour: starward-config-ini-df8ae07, starward-gamebiz-df8ae07,
  // collapse-channel-map-d478585, collapse-bilibili-sdk-d478585.
  struct RegionRow {
    const char *game;
    const char *region;
    const char *biz;  // HoYoPlay biz of the region (hk4e_bilibili, nap_cn, …)
    const char *name;  // 国服 / B 服 / 国际服
    const char *name_en;  // CN / Bilibili / Global
    int channel;
    int sub_channel;
    const char *cps;  // config.ini cps value the launcher writes
  };

  // One row of the title table: the names a game is called by and what this
  // host can do with it. Sources for every row are in docs/sparxie-games.md.
  struct TitleRow {
    const char *game;
    const char *name;  // 原神, 鸣潮, …
    const char *name_en;  // Genshin Impact, Wuthering Waves, …
    std::vector<std::string> aliases;  // what people type to find it
    bool touch;  // Sparxie's touch launch supports the title
    std::vector<std::string> settings;  // the preset fields this host can write
  };

  const TitleRow *
  title_row(std::string_view game);
  // The preset fields a game's settings adapter can write today (empty for a
  // title whose storage this host has no sourced writer for; 自动 never
  // touches the game either way).
  std::vector<std::string>
  settings_fields(std::string_view game);
  std::vector<std::string>
  launch_fields(std::string_view game);

  const RegionRow *
  region_row(std::string_view game, std::string_view region);
  std::string
  region_display(std::string_view region);
  std::string
  region_display_en(std::string_view region);
  // The HoYoPlay registry biz token of a region (bilibili is hk4e_bilibili),
  // unlike biz_for(), which stays the art token the launcher API answers with.
  std::string
  hyp_biz(std::string_view game, std::string_view region);

  // One HoYoPlay GameInstallPath key: where it is and what it means.
  struct HypKey {
    std::string key;
    std::string biz;
    std::string game;
    std::string region;
    // "hoyoplay" for the official launcher, "bili" for the Bilibili launcher.
    std::string source;
  };

  // Every HoYoPlay install key this host understands, from the keys it knows
  // and, when the registry can enumerate, from every Software\{miHoYo,
  // Cognosphere}\HYP\* key that names a known biz (collapse-hyp-detect-d478585).
  std::vector<HypKey>
  hyp_registry_keys(const Registry &registry);

  // Region identification. Order and markers follow the host contract
  // (docs/sparxie-api.md): executable, HoYoPlay GameInstallPath, Steam or
  // Epic records, PCGameSDK.dll, then config.ini (game_biz, channel, cps).
  // Registry layout: starward-hyp-keys-df8ae07.
  // Executable whitelist: sparxie-game-executables-8f907cb.
  IdentifyResult
  identify_install(const fs::path &input, const Registry &registry, const StorefrontEnv &stores = {}, std::string_view source_fallback = "manual");

  // The game a known executable file name belongs to ("" when the name is not
  // one of the whitelisted game executables). Case-insensitive.
  std::string
  game_of_executable(std::string_view file);
  // The executable names of a game, in the order they are shown to the person.
  // With a region, only the names that region ships: Genshin Impact's
  // international client is GenshinImpact.exe, the national and bilibili
  // clients are YuanShen.exe.
  std::vector<std::string>
  executables_for(std::string_view game, std::string_view region = {});
  // Per game and region, the executables and the regions the game has, plus
  // the described settings and Sparxie items the page draws (docs/sparxie-api.md).
  // zzz_file_settings adds the items of Zenless Zone Zero that live in its
  // GENERAL_DATA.bin: they exist for a host only while it holds that file's magic.
  json
  catalog_json(bool zzz_file_settings = false);
}  // namespace sparxie

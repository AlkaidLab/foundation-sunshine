#pragma once
#include "src/sparxie/model.h"

namespace sparxie {
  // What a non-HoYo game's own launcher records about an install root.
  // Every check reads only files inside the root; the answers and the file
  // names come from the vendor launchers' observed behaviour
  // (docs/sparxie-games.md names the source of each fact).
  struct VendorAnswer {
    // The region the record proves ("" when the record says nothing).
    std::string region;
    // True when the vendor launcher's own file records the install, so the
    // install's source is the official launcher.
    bool launcher_record = false;
    std::vector<std::string> markers;
  };

  // Wuthering Waves: the launcher's launcherDownloadConfig.json names the
  // channel (appId 10003 mainland, 10004 bilibili, 50004 global); without it
  // the SDK folder under Client\Binaries\Win64\ThirdParty names the region
  // (KrPcSdk_Mainland, KrPcSdk_Global, and KRSDKRes\Bilibili inside the
  // mainland SDK for the bilibili channel).
  VendorAnswer
  wuwa_vendor(const fs::path &root);

  // NTE: the CN launcher's Config.ini ([UPDATE_CONFIG] Branch publish_ob is
  // the official channel, publish_ob_B the bilibili one) sits under
  // NTELauncher\Config; the global launcher keeps its own NTEGlobal folder
  // with the same file.
  VendorAnswer
  nte_vendor(const fs::path &root);

  // Arknights: the Yostar clients (EN/JP/KR) carry game-launcher-config.json
  // whose tag names the region; the folder name says the same when the file
  // is missing. The CN client writes no channel record this host knows.
  VendorAnswer
  arknights_vendor(const fs::path &root);

  // Endfield: the sdkdata folder's channel config names the channel
  // (sdk_hgsdk_config the CN one, sdk_glsdk_config the global one), and the
  // bilibili SDK files (PCGameSDK.dll, BLPlatform64*.dll) name the bilibili
  // one. A Steam build is narrowed by the store record instead.
  VendorAnswer
  endfield_vendor(const fs::path &root);

  // The official Hypergryph launcher's own exe (Launcher.exe) under one of
  // its install roots, newest version folder first; empty when it is not
  // there. The launcher checks the game's parent process, so a launcher
  // build of Arknights or Endfield starts through this program.
  fs::path
  hypergryph_launcher_exe(const std::vector<fs::path> &roots);

  // One official Hypergryph launcher on this machine: its install folder and
  // the channel of the games it manages (cn 鹰角启动器, global GRYPHLINK,
  // bili the bilibili edition of Endfield).
  struct LauncherInstall {
    fs::path dir;
    std::string channel;
  };

  // The uninstall key names of the three official Hypergryph launchers, in
  // the same order as the channels above (docs/sparxie-games.md).
  std::vector<std::string>
  hypergryph_uninstall_keys();

  // The launchers the uninstall records name: record i is the InstallLocation
  // answer of key i, kept only when that folder exists, each folder once.
  std::vector<LauncherInstall>
  hypergryph_launchers_from_records(const std::vector<fs::path> &locations);

  // One game install found under an official launcher: which game, where, and
  // the channel of the launcher that manages it.
  struct LauncherGame {
    std::string game;
    fs::path dir;
    std::string channel;
  };

  // Every Arknights and Endfield install in the official launchers' games
  // folders (games\<title folder>\Endfield.exe or Arknights.exe, one level
  // below the games folder). A store install of the same folder keeps its
  // store row: the caller gives these rows the lowest priority.
  std::vector<LauncherGame>
  hypergryph_launcher_games(const std::vector<LauncherInstall> &launchers);
}  // namespace sparxie

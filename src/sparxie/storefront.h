#pragma once
#include "src/sparxie/model.h"
#include "src/sparxie/scan.h"

namespace sparxie {
  // Zenless Zone Zero on Steam, app id from Valve's own store API answer
  // (steam-zzz-store-20261111): name "Zenless Zone Zero", release 2026-06-16.
  inline constexpr int kSteamZzzAppId = 4162040;
  // The Steam app id of a game's store build (0 when the game is not on
  // Steam). Every id is the one Valve's own store API answers with; the
  // builds are the international clients.
  int
  steam_app_id(std::string_view game);

  // Where the store libraries and manifests live on this machine. Both paths
  // are empty when the host could not resolve them (no Steam, no Epic).
  struct StorefrontEnv {
    fs::path steam_root;  // the Steam install root (HKCU SteamPath)
    fs::path epic_manifests;  // %ProgramData%\Epic\EpicGamesLauncher\Data\Manifests
  };

  // One install that came from a store instead of a game launcher.
  struct StorefrontInstall {
    std::string source;  // "steam" or "epic"
    std::string game;
    std::string region;  // store builds are the international client
    fs::path dir;
    std::string exe;
    std::string app;  // the Epic AppName; empty for a Steam install
  };

  // A parsed Valve Data File node (KeyValues): either a leaf with a value or a
  // block with children in file order. valve-keyvalues-760f379.
  struct VdfNode {
    std::string value;
    std::vector<std::pair<std::string, VdfNode>> children;
    const VdfNode *
    find(std::string_view key) const {
      for (const auto &child : children)
        if (child.first == key) return &child.second;
      return nullptr;
    }
  };

  // Parses one root node (for example "libraryfolders" or "AppState").
  // Returns nothing when the text is not a well-formed tree.
  std::optional<VdfNode>
  parse_vdf(std::string_view text);

  // The Steam install root and every extra library named in
  // steamapps/libraryfolders.vdf, each once.
  std::vector<fs::path>
  steam_libraries(const fs::path &steam_root);

  // One Epic Games Launcher manifest: where the install is and the AppName
  // the store's own launch link needs.
  struct EpicRecord {
    fs::path root;
    std::string app;
  };

  // Every Epic manifest's InstallLocation and AppName. Field names:
  // legendary-egl-manifest-46399df.
  std::vector<EpicRecord>
  epic_records(const fs::path &manifests_dir);

  // The folder of an installed Steam app: steamapps/appmanifest_<id>.acf names
  // the installdir under steamapps/common of the library that holds it.
  std::optional<fs::path>
  steam_install_dir(const fs::path &steam_root, int app_id);

  // Every InstallLocation of the Epic Games Launcher manifests (.item files).
  // Field names: legendary-egl-manifest-46399df.
  std::vector<fs::path>
  epic_install_roots(const fs::path &manifests_dir);

  // Every store install of the supported games. Steam ships Zenless Zone Zero
  // (kSteamZzzAppId); the Epic manifests are matched by the game executable
  // inside their InstallLocation, so a game that is not known here is skipped.
  std::vector<StorefrontInstall>
  find_storefront_installs(const StorefrontEnv &env);

  // True when dir is root itself or lies directly below root (store layouts
  // put the game executable at most one level under the recorded folder).
  bool
  dir_at_or_below(const fs::path &dir, const fs::path &root);
}  // namespace sparxie

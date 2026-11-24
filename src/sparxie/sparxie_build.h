#pragma once
#include "src/sparxie/model.h"
// What the installed Sparxie build can read, learned from its own files (never from a version string).
//
// Two generations exist. The first ships Endfield inside the build: config.json names it "endfield", the Broker carries the title's own fields, and
// capabilities.json (or EndfieldCore.dll + EndfieldRuntime.dll) says so. The second knows only the three Hoyo titles and loads every other game as a plugin
// (<data dir>\plugins\<id>\plugin.json + module.dll): config.json names a profile's game by the plugin's id (ext.<vendor>.<game>), keeps the plugin's own
// options in the profile's "settings" object, and treats an id of the first generation ("endfield") as invalid, which makes Sparxie put the whole config aside.
// The host writes only what the installed build reads (docs/sparxie-api.md, "Two Sparxie generations").
namespace sparxie {
  // The plugin that carries Endfield for the second generation (https://github.com/skyhua0224/endfiled-sparxie).
  inline constexpr const char *kEndfieldPluginId = "ext.skyhua0224.endfield";

  enum class Generation {
    unknown,  // no installed build to look at: nothing reads config.json yet
    legacy,   // first generation
    plugin,   // second generation: Sparxie.Contracts.dll has GameIds and the settings_json field
  };
  struct EndfieldPlugin {
    // plugin.json parses, its id is kEndfieldPluginId, Endfield.exe is on its executable list and module.dll is beside it
    bool installed = false;
    // plugin-trust.json holds a record of that id whose hashes match plugin.json and module.dll: Sparxie will load it
    bool trusted = false;
    std::string version;
    // the keys of its settingsSchema; the host writes no other key. A schema that declares none (or no schema at all) is `keys_known` with an empty list: nothing is written.
    // A build described by hand (tests) that never read a schema leaves keys_known false and takes every key the host manages.
    bool keys_known = false;
    std::vector<std::string> keys;
  };
  struct SparxieBuild {
    Generation generation = Generation::unknown;
    bool builtin_endfield = false;
    EndfieldPlugin endfield;
  };
  enum class EndfieldSupport {
    builtin,         // "endfield" in the build itself (also: no build to look at, which keeps the first generation's spelling)
    plugin,          // kEndfieldPluginId, its options in "settings"
    plugin_missing,  // second generation without the Endfield plugin: no Endfield profile may be written
    unsupported,     // first generation without Endfield: no Endfield profile may be written
  };

  // ext.<vendor>.<game> with the vendor and game of 2-32 characters from a-z, 0-9 and "-" (Sparxie's GameIds.IsValidId for a third-party game).
  bool
  is_plugin_game_id(std::string_view id);
  // Looks at the installed build under `dir` (the folder above app-*, which also holds config.json and plugins\).
  SparxieBuild
  detect_build(const fs::path &dir);
  EndfieldSupport
  endfield_support(const SparxieBuild &build);
  std::string_view
  endfield_support_name(EndfieldSupport support);
  // The game spelling a profile of Endfield gets in config.json; empty when the build cannot hold one.
  std::string
  endfield_game_id(const SparxieBuild &build);
  // Whether this build's config.json can hold a profile of this game id without the build rejecting it.
  bool
  build_reads_game(const SparxieBuild &build, std::string_view game);
  // The error of a write that would need an Endfield profile the build cannot hold: ok when it can.
  Status
  endfield_write_check(const SparxieBuild &build);
}  // namespace sparxie

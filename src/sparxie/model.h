#pragma once

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sparxie {
  namespace fs = std::filesystem;
  using json = nlohmann::json;

  // Game and region tokens used by this host. Sparxie's file uses a different
  // game spelling; see to_sparxie_game(). Region is never written into that file.
  inline constexpr std::string_view kGames[] = { "genshin", "starrail", "zzz", "hi3", "wuwa", "pgr", "arknights", "endfield", "nte" };
  inline constexpr std::string_view kRegions[] = { "cn", "bili", "global" };

  inline const char *kStatement =
    "Sparxie 会在游戏运行时向游戏进程注入代码并修改其内存，这可能违反游戏的用户协议，由此带来的账号风险由你自行承担。";
  inline const char *kStatementEn =
    "Sparxie injects code into the running game and changes its memory. This may break the game's terms of service, and any risk to your account is yours.";

  // [sparxie-broker-contract-031efe2] The Desktop shell does not consume
  // command-line profile arguments. The host therefore invokes the small
  // protocol launcher shipped with the host and passes the component path.
  // The optional Sparxie feature the host looks for in the installed build (see docs/sparxie-api.md, "touchUiSwitch").
  inline constexpr const char *kCapTouchUiSwitch = "touch-ui-switch";
  inline const char *kDefaultLaunchTemplate =
    "\"{sparxie-launch}\" --sparxie \"{sparxie}\" --profile {profile}";

  // A `sparxie` block that went through the generic apps writer (boost property_tree) has every scalar as a string ("false", "120") and its empty
  // arrays and objects as "". These put the types back, so the file heals the next time it is read or saved. Both return true when something changed.
  bool
  normalize_block(json &block);
  bool
  normalize_apps(json &apps);

  struct Status {
    bool ok = true;
    int http = 200;
    std::string error_code;
    std::string error;

    static Status
    success() { return {}; }
    static Status
    fail(int http, std::string code, std::string message) {
      Status s;
      s.ok = false;
      s.http = http;
      s.error_code = std::move(code);
      s.error = std::move(message);
      return s;
    }
  };

  struct RegValue {
    enum class Kind { none,
      dword,
      binary,
      text };
    Kind kind = Kind::none;
    std::uint32_t dword = 0;
    std::string text;
    std::vector<std::uint8_t> bytes;
  };

  class Registry {
  public:
    virtual ~Registry() = default;
    virtual RegValue
    get(std::string_view key, std::string_view name) const = 0;
    // The names of the sub-keys of a key; empty when the key cannot be
    // enumerated (then the caller falls back to the key names it knows).
    virtual std::vector<std::string>
    subkeys(std::string_view key) const { return {}; }
    virtual bool
    set_dword(std::string_view key, std::string_view name, std::uint32_t value) = 0;
    virtual bool
    set_binary(std::string_view key, std::string_view name, const std::vector<std::uint8_t> &bytes) = 0;
    // Removes one value (a value a launch created that the game did not have before). The default refuses.
    virtual bool
    remove(std::string_view key, std::string_view name) {
      (void) key;
      (void) name;
      return false;
    }
    // REG_SZ. The default refuses the write so a registry that only stores
    // numbers and binary documents is unchanged.
    virtual bool
    set_text(std::string_view key, std::string_view name, std::string_view text) {
      (void) key;
      (void) name;
      (void) text;
      return false;
    }
  };

  class MemoryRegistry: public Registry {
  public:
    RegValue
    get(std::string_view key, std::string_view name) const override;
    std::vector<std::string>
    subkeys(std::string_view key) const override;
    bool
    set_dword(std::string_view key, std::string_view name, std::uint32_t value) override;
    bool
    set_binary(std::string_view key, std::string_view name, const std::vector<std::uint8_t> &bytes) override;
    bool
    set_text(std::string_view key, std::string_view name, std::string_view text) override;
    bool
    remove(std::string_view key, std::string_view name) override;

  private:
    struct Row {
      std::string key;
      std::string name;
      RegValue value;
    };
    std::vector<Row> rows;
    Row *
    find(std::string_view key, std::string_view name);
    const Row *
    find(std::string_view key, std::string_view name) const;
  };

  struct Install {
    std::string id;
    std::string game;
    std::string region;
    std::vector<std::string> candidates;
    std::vector<std::string> markers;
    // Where the host found this install: hoyoplay (the launcher registry), steam, epic, bili (the bilibili client's SDK file). The person's own choice is marked by the page (manual).
    std::vector<std::string> sources;
    std::string dir;
    std::string exe;
    std::string exe_path;
    std::string version;
    std::string biz;
    std::string profile;
    // The Epic AppName of the manifest that records the install (empty when
    // no Epic manifest names it). It is the id the store's own launch link
    // needs, so it rides with the install.
    std::string epic_app;
    // Where the install was found: hoyoplay, bili, steam, epic, folder or
    // manual. Empty on an install that was only read back from a stored file.
    std::string source;
    int channel = -1;
    int sub_channel = -1;
    json art = json::object({ { "version", "" }, { "updated", "" } });
  };

  struct IdentifyResult {
    Status status;
    bool has_game = false;
    Install install;
  };

  inline bool
  is_game(std::string_view value) {
    for (auto g : kGames)
      if (g == value) return true;
    return false;
  }
  inline bool
  is_region(std::string_view value) {
    for (auto r : kRegions)
      if (r == value) return true;
    return false;
  }

  std::string
  ascii_lower(std::string_view text);
  std::string
  norm_path(const fs::path &path);
  bool
  paths_equal(const fs::path &a, const fs::path &b);
  std::string
  fnv8(std::string_view text);
  std::string
  to_sparxie_game(std::string_view game);
  std::string
  from_sparxie_game(std::string_view game);
  std::string
  biz_for(std::string_view game, std::string_view region);
  std::string
  quote_path(std::string_view path);
  std::string
  apply_template(std::string_view templ, const std::vector<std::pair<std::string, std::string>> &vars);

  json
  install_json(const Install &install);
  std::string
  resolution_text(int width, int height);
  bool
  parse_resolution(std::string_view text, int &width, int &height);
}  // namespace sparxie

#pragma once
#include "src/sparxie/art.h"
#include "src/sparxie/scan.h"
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace sparxie {
  Registry &
  host_registry();
  // A view of a registry with every key moved under `prefix` (for example "Software\\Tests\\"): tests run the real registry code against a subtree of their own, never a game's key.
  class PrefixedRegistry: public Registry {
  public:
    PrefixedRegistry(Registry &inner, std::string prefix):
        inner_(inner), prefix_(std::move(prefix)) {}
    RegValue
    get(std::string_view key, std::string_view name) const override { return inner_.get(prefixed(key), name); }
    std::vector<std::string>
    subkeys(std::string_view key) const override { return inner_.subkeys(prefixed(key)); }
    bool
    set_dword(std::string_view key, std::string_view name, std::uint32_t value) override { return inner_.set_dword(prefixed(key), name, value); }
    bool
    set_binary(std::string_view key, std::string_view name, const std::vector<std::uint8_t> &bytes) override { return inner_.set_binary(prefixed(key), name, bytes); }
    bool
    set_text(std::string_view key, std::string_view name, std::string_view text) override { return inner_.set_text(prefixed(key), name, text); }
    bool
    remove(std::string_view key, std::string_view name) override { return inner_.remove(prefixed(key), name); }

  private:
    std::string
    prefixed(std::string_view key) const { return prefix_ + std::string(key); }
    Registry &inner_;
    std::string prefix_;
  };
  bool
  host_process_running(const std::vector<std::string> &names);
  // The host runs as a service (SYSTEM) on Windows. Everything that depends on
  // the person (their registry hive, their drives, their folders) must be read
  // as that person. The runner is installed once by the host; it runs `call`
  // while impersonating the signed-in user and returns false when nobody is
  // signed in. Without a runner (tests, user mode) the call just runs.
  using UserCall = std::function<void()>;
  using UserRunner = std::function<bool(const UserCall &)>;
  void
  set_user_runner(UserRunner runner);
  bool
  host_as_user(const UserCall &call);
  // 提升游戏优先级: waits up to `wait` for a process whose image is `exe` (full path, either slash), raises its priority class to "above normal" and keeps
  // watching for 20 s after the first one is found (a game that sets its own priority at start-up, a second process of the same image). A class that is already
  // above normal is left alone. Returns how many processes were raised; 0 when none appeared. Blocks: call it from a thread of its own. No-op off Windows.
  int
  host_raise_game_priority(const fs::path &exe, std::chrono::milliseconds wait);
  // The drives of the signed-in user (mapped drives are per user).
  std::vector<BrowseEntry>
  host_list_roots();
  std::optional<std::string>
  host_http_get(const std::string &url);
  // Posts one JSON body and answers the response body (empty when the request
  // failed). Used by the art refresh for the launchers' own content APIs.
  std::optional<std::string>
  host_http_post_json(const std::string &url, const std::string &body);
  Rgba
  host_decode_image(std::string_view bytes);
  // The machine-wide ProgramData directory (the Epic Games Launcher keeps its
  // install manifests there). Empty when the host cannot resolve it.
  fs::path
  host_program_data();

  // The InstallLocation of the given uninstall key names, each key once, in
  // order: the machine's 64- and 32-bit HKLM views and the signed-in person's
  // HKCU (a per-user install writes there). Empty answers are skipped, so the
  // result can be shorter than the input.
  std::vector<fs::path>
  host_uninstall_locations(const std::vector<std::string> &key_names);
}  // namespace sparxie

/**
 * @file tests/unit/test_kscreen_backend.cpp
 * @brief When kscreen-doctor is allowed to be spawned at all.
 *
 * Regression guard for a field report: on a box whose session is niri (or whose
 * service started from SSH, before any desktop), every display query spawned
 * kscreen-doctor, which D-Bus-activates a KScreen that waits for a Plasma
 * session and only ends at the 10 s kill timeout. A session start issues dozens
 * of queries, so client connections blocked for a long time.
 */
#include <gtest/gtest.h>

#include <cstdlib>

#include "src/platform/linux/kscreen_backend.h"

#ifndef _WIN32
namespace {
  /// RAII: restore the two environment variables the gate reads.
  class scoped_env_t {
  public:
    scoped_env_t() {
      save("NIRI_SOCKET");
      save("XDG_CURRENT_DESKTOP");
    }

    ~scoped_env_t() {
      for (const auto &[name, value] : saved_) {
        if (value) {
          ::setenv(name.c_str(), value->c_str(), 1);
        }
        else {
          ::unsetenv(name.c_str());
        }
      }
    }

  private:
    void save(const char *name) {
      const char *value = ::getenv(name);
      saved_.emplace_back(name, value ? std::optional<std::string> { value } : std::nullopt);
    }

    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
  };
}  // namespace

TEST(KscreenBackend, NiriSessionNeverProbes) {
  scoped_env_t env;
  ::setenv("XDG_CURRENT_DESKTOP", "KDE", 1);
  ::setenv("NIRI_SOCKET", "/run/user/1000/niri.wayland-1.8.sock", 1);
  // NIRI_SOCKET wins: niri has no KScreen even if the desktop string says KDE.
  EXPECT_FALSE(platf::kscreen::session_supports_kscreen());
}

TEST(KscreenBackend, DesktopStringDecidesOtherwise) {
  scoped_env_t env;
  ::unsetenv("NIRI_SOCKET");

  ::unsetenv("XDG_CURRENT_DESKTOP");
  EXPECT_FALSE(platf::kscreen::session_supports_kscreen()) << "a service started before the desktop";

  ::setenv("XDG_CURRENT_DESKTOP", "", 1);
  EXPECT_FALSE(platf::kscreen::session_supports_kscreen());

  ::setenv("XDG_CURRENT_DESKTOP", "niri", 1);
  EXPECT_FALSE(platf::kscreen::session_supports_kscreen());

  ::setenv("XDG_CURRENT_DESKTOP", "sway", 1);
  EXPECT_FALSE(platf::kscreen::session_supports_kscreen());

  ::setenv("XDG_CURRENT_DESKTOP", "KDE", 1);
  EXPECT_TRUE(platf::kscreen::session_supports_kscreen());

  ::setenv("XDG_CURRENT_DESKTOP", "plasma", 1);
  EXPECT_TRUE(platf::kscreen::session_supports_kscreen());
}

TEST(KscreenBackend, FailureArmsACooldownAndSuccessClearsIt) {
  // Whatever the previous state, a success clears the gate and a failure arms it
  // for the cooldown window (the tests below run well inside it).
  platf::kscreen::note_available();
  EXPECT_TRUE(platf::kscreen::probe_allowed());

  platf::kscreen::note_unavailable("test");
  EXPECT_FALSE(platf::kscreen::probe_allowed());

  platf::kscreen::note_available();
  EXPECT_TRUE(platf::kscreen::probe_allowed());
}
#endif

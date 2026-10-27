/**
 * @file tests/unit/test_mutter_display.cpp
 * @brief Test the GNOME display-configuration codec.
 *
 * GNOME is not installable in CI, so the compositor cannot be asked anything
 * here. What *can* be checked is the contract with it: the D-Bus message shapes.
 * Mutter's `GetCurrentState` reply and `ApplyMonitorsConfig` arguments are both
 * fixed signatures, so the test builds a real sd-bus message carrying a
 * representative configuration, decodes it with the code that runs in
 * production, and re-encodes the result — a round trip through the exact
 * signature the compositor uses.
 *
 * The messages need a bus handle but never a peer: sd-bus messages are built
 * and read locally, so the session bus only has to exist.
 */
#include <src/platform/linux/mutter_display.h>
#include <src/platform/linux/sdbus_session.h>

#include "../tests_common.h"

#include <systemd/sd-bus.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {
  using namespace platf::mutter;

  /// Signature of a `GetCurrentState` reply (the out-argument tuple).
  constexpr const char *kStateSignature =
    "u((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv}";
  /// Signature of `ApplyMonitorsConfig`'s arguments.
  constexpr const char *kApplySignature = "uua(iiduba(ssa{sv}))a{sv}";

  /**
   * @brief A bus handle for building messages, plus message factories.
   */
  class codec_test_t : public ::testing::Test {
  protected:
    void
    SetUp() override {
      bus = platf::sdbus::open_user_bus();
      if (!bus) {
        GTEST_SKIP() << "no session bus to build sd-bus messages on";
      }
    }

    void
    TearDown() override {
      if (bus) {
        sd_bus_unref(bus);
      }
    }

  public:
    /**
     * @brief An empty method-return message whose body the test fills in.
     * @details sd-bus only answers a *sealed* call, so the artificial call has
     *          to be sealed with a serial before the reply can be built on it.
     */
    sd_bus_message *
    new_return_message() {
      sd_bus_message *call = nullptr;
      sd_bus_message *reply = nullptr;
      EXPECT_GE(sd_bus_message_new_method_call(bus, &call, "org.example.Fake", "/fake",
                  "org.example.Fake", "GetCurrentState"),
        0);
      if (call) {
        EXPECT_GE(sd_bus_message_seal(call, 1, 0), 0);
        EXPECT_GE(sd_bus_message_new_method_return(call, &reply), 0);
        sd_bus_message_unref(call);
      }
      return reply;
    }

    sd_bus_message *
    new_call_message() {
      sd_bus_message *call = nullptr;
      EXPECT_GE(sd_bus_message_new_method_call(bus, &call, kService, kPath, kInterface,
                  "ApplyMonitorsConfig"),
        0);
      return call;
    }

  public:
    /// Append the `a{sv}` mode flags.
    static void
    append_mode_flags(sd_bus_message *m, bool current, bool preferred) {
      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "{sv}"), 0);
      if (current) {
        ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv"), 0);
        ASSERT_GE(sd_bus_message_append(m, "s", "is-current"), 0);
        ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_VARIANT, "b"), 0);
        ASSERT_GE(sd_bus_message_append(m, "b", 1), 0);
        ASSERT_GE(sd_bus_message_close_container(m), 0);
        ASSERT_GE(sd_bus_message_close_container(m), 0);
      }
      if (preferred) {
        ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv"), 0);
        ASSERT_GE(sd_bus_message_append(m, "s", "is-preferred"), 0);
        ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_VARIANT, "b"), 0);
        ASSERT_GE(sd_bus_message_append(m, "b", 1), 0);
        ASSERT_GE(sd_bus_message_close_container(m), 0);
        ASSERT_GE(sd_bus_message_close_container(m), 0);
      }
      ASSERT_GE(sd_bus_message_close_container(m), 0);
    }

    /// Append the `a{sv}` monitor properties, including the colour-mode pair.
    static void
    append_monitor_properties(sd_bus_message *m, const std::string &display_name,
      unsigned int color_mode, const std::vector<unsigned int> &supported) {
      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "{sv}"), 0);

      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv"), 0);
      ASSERT_GE(sd_bus_message_append(m, "s", "display-name"), 0);
      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_VARIANT, "s"), 0);
      ASSERT_GE(sd_bus_message_append(m, "s", display_name.c_str()), 0);
      ASSERT_GE(sd_bus_message_close_container(m), 0);
      ASSERT_GE(sd_bus_message_close_container(m), 0);

      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv"), 0);
      ASSERT_GE(sd_bus_message_append(m, "s", "color-mode"), 0);
      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_VARIANT, "u"), 0);
      ASSERT_GE(sd_bus_message_append(m, "u", color_mode), 0);
      ASSERT_GE(sd_bus_message_close_container(m), 0);
      ASSERT_GE(sd_bus_message_close_container(m), 0);

      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv"), 0);
      ASSERT_GE(sd_bus_message_append(m, "s", "supported-color-modes"), 0);
      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_VARIANT, "au"), 0);
      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "u"), 0);
      for (const auto mode : supported) {
        ASSERT_GE(sd_bus_message_append(m, "u", mode), 0);
      }
      ASSERT_GE(sd_bus_message_close_container(m), 0);
      ASSERT_GE(sd_bus_message_close_container(m), 0);
      ASSERT_GE(sd_bus_message_close_container(m), 0);

      // An unknown property has to be tolerated: Mutter adds them over time.
      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv"), 0);
      ASSERT_GE(sd_bus_message_append(m, "s", "width-mm"), 0);
      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_VARIANT, "i"), 0);
      ASSERT_GE(sd_bus_message_append(m, "i", 600), 0);
      ASSERT_GE(sd_bus_message_close_container(m), 0);
      ASSERT_GE(sd_bus_message_close_container(m), 0);

      ASSERT_GE(sd_bus_message_close_container(m), 0);
    }

    /// Append one `a(siiddada{sv})` mode list with two modes.
    static void
    append_modes(sd_bus_message *m) {
      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "(siiddada{sv})"), 0);

      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "siiddada{sv}"), 0);
      ASSERT_GE(sd_bus_message_append(m, "s", "1920x1080@60.000"), 0);
      ASSERT_GE(sd_bus_message_append(m, "ii", 1920, 1080), 0);
      ASSERT_GE(sd_bus_message_append(m, "dd", 60.0, 1.0), 0);
      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "d"), 0);
      ASSERT_GE(sd_bus_message_append(m, "d", 1.0), 0);
      ASSERT_GE(sd_bus_message_close_container(m), 0);
      append_mode_flags(m, false, true);
      ASSERT_GE(sd_bus_message_close_container(m), 0);

      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "siiddada{sv}"), 0);
      ASSERT_GE(sd_bus_message_append(m, "s", "2560x1440@144.001"), 0);
      ASSERT_GE(sd_bus_message_append(m, "ii", 2560, 1440), 0);
      ASSERT_GE(sd_bus_message_append(m, "dd", 144.001, 1.0), 0);
      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "d"), 0);
      ASSERT_GE(sd_bus_message_append(m, "d", 1.0), 0);
      ASSERT_GE(sd_bus_message_close_container(m), 0);
      append_mode_flags(m, true, false);
      ASSERT_GE(sd_bus_message_close_container(m), 0);

      ASSERT_GE(sd_bus_message_close_container(m), 0);
    }

    /// Append one monitor entry.
    static void
    append_monitor(sd_bus_message *m, const char *connector, const char *display_name,
      unsigned int color_mode, const std::vector<unsigned int> &supported) {
      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "(ssss)a(siiddada{sv})a{sv}"), 0);
      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "ssss"), 0);
      ASSERT_GE(sd_bus_message_append(m, "ssss", connector, "GSM", "LG HDR", "0x0001"), 0);
      ASSERT_GE(sd_bus_message_close_container(m), 0);
      append_modes(m);
      append_monitor_properties(m, display_name, color_mode, supported);
      ASSERT_GE(sd_bus_message_close_container(m), 0);
    }

    /// Append one logical monitor entry.
    static void
    append_logical(sd_bus_message *m, int x, int y, double scale, bool primary,
      const std::vector<std::string> &connectors) {
      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "iiduba(ssss)a{sv}"), 0);
      ASSERT_GE(sd_bus_message_append(m, "iid", x, y, scale), 0);
      ASSERT_GE(sd_bus_message_append(m, "u", 0u), 0);
      ASSERT_GE(sd_bus_message_append(m, "b", static_cast<int>(primary)), 0);
      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "(ssss)"), 0);
      for (const auto &connector : connectors) {
        ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "ssss"), 0);
        ASSERT_GE(sd_bus_message_append(m, "ssss", connector.c_str(), "GSM", "LG HDR", "0x0001"), 0);
        ASSERT_GE(sd_bus_message_close_container(m), 0);
      }
      ASSERT_GE(sd_bus_message_close_container(m), 0);
      ASSERT_GE(sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "{sv}"), 0);
      ASSERT_GE(sd_bus_message_close_container(m), 0);
      ASSERT_GE(sd_bus_message_close_container(m), 0);
    }

    /// Seal a message and rewind it so it can be read back from the start.
    static void
    seal(sd_bus_message *m) {
      ASSERT_GE(sd_bus_message_seal(m, 1, 0), 0);
      ASSERT_GE(sd_bus_message_rewind(m, true), 0);
    }

    sd_bus *bus = nullptr;
  };

  /// The state the reply-based tests build: one HDR monitor, one replication.
  sd_bus_message *
  build_state_reply(codec_test_t &test) {
    sd_bus_message *reply = test.new_return_message();
    if (!reply) {
      return nullptr;
    }

    // A reply's body tuple is already open for writing, so the four members go
    // in directly; sealing fixes the signature.
    if (sd_bus_message_append(reply, "u", 4242u) < 0) {
      sd_bus_message_unref(reply);
      return nullptr;
    }

    if (sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "((ssss)a(siiddada{sv})a{sv})") < 0) {
      sd_bus_message_unref(reply);
      return nullptr;
    }
    codec_test_t::append_monitor(reply, "eDP-1", "Built-in display", kColorModeBt2100, { 0, 1, 2 });
    codec_test_t::append_monitor(reply, "HDMI-1", "LG HDR", kColorModeDefault, { 0 });
    if (sd_bus_message_close_container(reply) < 0) {
      sd_bus_message_unref(reply);
      return nullptr;
    }

    if (sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "(iiduba(ssss)a{sv})") < 0) {
      sd_bus_message_unref(reply);
      return nullptr;
    }
    // A replication group and a single-monitor group, with the second primary.
    codec_test_t::append_logical(reply, 0, 0, 1.0, false, { "eDP-1", "HDMI-1" });
    codec_test_t::append_logical(reply, 1920, 0, 1.5, true, { "DP-2" });
    if (sd_bus_message_close_container(reply) < 0) {
      sd_bus_message_unref(reply);
      return nullptr;
    }

    if (sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "{sv}") < 0) {
      sd_bus_message_unref(reply);
      return nullptr;
    }
    if (sd_bus_message_close_container(reply) < 0) {
      sd_bus_message_unref(reply);
      return nullptr;
    }

    codec_test_t::seal(reply);
    return reply;
  }
}  // namespace

TEST_F(codec_test_t, DecodesTheCurrentState) {
  sd_bus_message *reply = build_state_reply(*this);
  ASSERT_NE(reply, nullptr);

  state_t state;
  ASSERT_TRUE(read_state(reply, state));
  sd_bus_message_unref(reply);

  EXPECT_EQ(state.serial, 4242u);
  ASSERT_EQ(state.monitors.size(), 2u);

  const auto &built_in = state.monitors[0];
  EXPECT_EQ(built_in.connector, "eDP-1");
  EXPECT_EQ(built_in.vendor, "GSM");
  EXPECT_EQ(built_in.product, "LG HDR");
  EXPECT_EQ(built_in.serial, "0x0001");
  EXPECT_EQ(built_in.display_name, "Built-in display");
  ASSERT_EQ(built_in.modes.size(), 2u);
  EXPECT_EQ(built_in.modes[0].id, "1920x1080@60.000");
  EXPECT_EQ(built_in.modes[0].width, 1920);
  EXPECT_EQ(built_in.modes[0].height, 1080);
  EXPECT_DOUBLE_EQ(built_in.modes[0].refresh, 60.0);
  EXPECT_FALSE(built_in.modes[0].current);
  EXPECT_TRUE(built_in.modes[0].preferred);
  EXPECT_TRUE(built_in.modes[1].current);
  EXPECT_FALSE(built_in.modes[1].preferred);
  EXPECT_DOUBLE_EQ(built_in.modes[1].refresh, 144.001);
}

TEST_F(codec_test_t, DecodesColorModes) {
  sd_bus_message *reply = build_state_reply(*this);
  ASSERT_NE(reply, nullptr);

  state_t state;
  ASSERT_TRUE(read_state(reply, state));
  sd_bus_message_unref(reply);

  ASSERT_EQ(state.monitors.size(), 2u);
  EXPECT_EQ(state.monitors[0].color_mode, kColorModeBt2100);
  ASSERT_EQ(state.monitors[0].supported_color_modes.size(), 3u);
  EXPECT_EQ(state.monitors[0].supported_color_modes[1], kColorModeBt2100);
  EXPECT_EQ(state.monitors[1].color_mode, kColorModeDefault);
  ASSERT_EQ(state.monitors[1].supported_color_modes.size(), 1u);
}

TEST_F(codec_test_t, DecodesLogicalMonitorsIncludingReplication) {
  sd_bus_message *reply = build_state_reply(*this);
  ASSERT_NE(reply, nullptr);

  state_t state;
  ASSERT_TRUE(read_state(reply, state));
  sd_bus_message_unref(reply);

  ASSERT_EQ(state.logical_monitors.size(), 2u);

  const auto &replicated = state.logical_monitors[0];
  EXPECT_EQ(replicated.x, 0);
  EXPECT_EQ(replicated.y, 0);
  EXPECT_DOUBLE_EQ(replicated.scale, 1.0);
  EXPECT_FALSE(replicated.primary);
  ASSERT_EQ(replicated.connectors.size(), 2u);
  EXPECT_EQ(replicated.connectors[0], "eDP-1");
  EXPECT_EQ(replicated.connectors[1], "HDMI-1");

  const auto &primary = state.logical_monitors[1];
  EXPECT_EQ(primary.x, 1920);
  EXPECT_DOUBLE_EQ(primary.scale, 1.5);
  EXPECT_TRUE(primary.primary);
  ASSERT_EQ(primary.connectors.size(), 1u);
  EXPECT_EQ(primary.connectors[0], "DP-2");
}

TEST_F(codec_test_t, RejectsAMessageThatIsNotAState) {
  // A reply carrying an unrelated signature must be refused rather than
  // half-read into a plausible-looking state.
  sd_bus_message *reply = new_return_message();
  ASSERT_NE(reply, nullptr);
  ASSERT_GE(sd_bus_message_append(reply, "s", "not a state"), 0);
  seal(reply);

  state_t state;
  EXPECT_FALSE(read_state(reply, state));
  sd_bus_message_unref(reply);
}

TEST_F(codec_test_t, WritesApplyArgumentsForTheWholeConfiguration) {
  state_t state;
  state.serial = 4242u;

  monitor_t built_in;
  built_in.connector = "eDP-1";
  built_in.modes = { { "1920x1080@60.000", 1920, 1080, 60.0, false, true },
    { "2560x1440@144.001", 2560, 1440, 144.001, true, false } };
  built_in.color_mode = kColorModeBt2100;
  state.monitors.push_back(built_in);

  monitor_t external;
  external.connector = "HDMI-1";
  external.modes = { { "1920x1080@60.000", 1920, 1080, 60.0, true, true } };
  state.monitors.push_back(external);

  monitor_t displayport;
  displayport.connector = "DP-2";
  displayport.modes = { { "3840x2160@60.000", 3840, 2160, 60.0, true, true } };
  state.monitors.push_back(displayport);

  logical_monitor_t replicated;
  replicated.connectors = { "eDP-1", "HDMI-1" };
  state.logical_monitors.push_back(replicated);

  logical_monitor_t secondary;
  secondary.x = 2560;
  secondary.scale = 1.5;
  secondary.primary = true;
  secondary.connectors = { "DP-2" };
  state.logical_monitors.push_back(secondary);

  sd_bus_message *call = new_call_message();
  ASSERT_NE(call, nullptr);
  ASSERT_TRUE(append_apply_args(call, state));
  seal(call);

  EXPECT_STREQ(sd_bus_message_get_signature(call, true), kApplySignature);

  // Read the arguments back the way Mutter would. A message body is the
  // argument tuple itself, so its members are read without entering anything.

  std::uint32_t serial = 0;
  std::uint32_t method = 0;
  ASSERT_GE(sd_bus_message_read(call, "uu", &serial, &method), 0);
  EXPECT_EQ(serial, 4242u);
  // Persistent: Sunshine restores these settings after the session ends.
  EXPECT_EQ(method, kMethodPersistent);

  ASSERT_GE(sd_bus_message_enter_container(call, SD_BUS_TYPE_ARRAY, "(iiduba(ssa{sv}))"), 0);

  // First logical monitor: both connectors, eDP-1 in HDR.
  ASSERT_GT(sd_bus_message_enter_container(call, SD_BUS_TYPE_STRUCT, "iiduba(ssa{sv})"), 0);
  std::int32_t x = -1;
  std::int32_t y = -1;
  double scale = 0.0;
  std::uint32_t transform = 99;
  int primary = 1;
  ASSERT_GE(sd_bus_message_read(call, "iid", &x, &y, &scale), 0);
  ASSERT_GE(sd_bus_message_read(call, "u", &transform), 0);
  ASSERT_GE(sd_bus_message_read(call, "b", &primary), 0);
  EXPECT_EQ(x, 0);
  EXPECT_EQ(y, 0);
  EXPECT_DOUBLE_EQ(scale, 1.0);
  EXPECT_EQ(transform, 0u);
  EXPECT_EQ(primary, 0);

  ASSERT_GE(sd_bus_message_enter_container(call, SD_BUS_TYPE_ARRAY, "(ssa{sv})"), 0);

  ASSERT_GT(sd_bus_message_enter_container(call, SD_BUS_TYPE_STRUCT, "ssa{sv}"), 0);
  const char *connector = nullptr;
  const char *mode_id = nullptr;
  ASSERT_GE(sd_bus_message_read(call, "ss", &connector, &mode_id), 0);
  EXPECT_STREQ(connector, "eDP-1");
  // The current mode goes back verbatim: Mutter rejects ids it did not hand out.
  EXPECT_STREQ(mode_id, "2560x1440@144.001");

  ASSERT_GE(sd_bus_message_enter_container(call, SD_BUS_TYPE_ARRAY, "{sv}"), 0);
  ASSERT_GT(sd_bus_message_enter_container(call, SD_BUS_TYPE_DICT_ENTRY, "sv"), 0);
  const char *key = nullptr;
  ASSERT_GE(sd_bus_message_read(call, "s", &key), 0);
  EXPECT_STREQ(key, "color-mode");
  ASSERT_GE(sd_bus_message_enter_container(call, SD_BUS_TYPE_VARIANT, "u"), 0);
  std::uint32_t color_mode = 0;
  ASSERT_GE(sd_bus_message_read(call, "u", &color_mode), 0);
  EXPECT_EQ(color_mode, kColorModeBt2100);
  ASSERT_GE(sd_bus_message_exit_container(call), 0);  // variant
  ASSERT_GE(sd_bus_message_exit_container(call), 0);  // dict entry
  // Zero, not negative: entering past the end of an array means "no more
  // elements", which is how the one-entry map announces itself as exhausted.
  ASSERT_EQ(sd_bus_message_enter_container(call, SD_BUS_TYPE_DICT_ENTRY, "sv"), 0);
  ASSERT_GE(sd_bus_message_exit_container(call), 0);  // a{sv}
  ASSERT_GE(sd_bus_message_exit_container(call), 0);  // struct

  // HDMI-1 runs its only mode and carries no colour mode, so the property map
  // stays empty: absent means the compositor default.
  ASSERT_GT(sd_bus_message_enter_container(call, SD_BUS_TYPE_STRUCT, "ssa{sv}"), 0);
  ASSERT_GE(sd_bus_message_read(call, "ss", &connector, &mode_id), 0);
  EXPECT_STREQ(connector, "HDMI-1");
  EXPECT_STREQ(mode_id, "1920x1080@60.000");
  ASSERT_GE(sd_bus_message_enter_container(call, SD_BUS_TYPE_ARRAY, "{sv}"), 0);
  EXPECT_EQ(sd_bus_message_enter_container(call, SD_BUS_TYPE_DICT_ENTRY, "sv"), 0);
  ASSERT_GE(sd_bus_message_exit_container(call), 0);
  ASSERT_GE(sd_bus_message_exit_container(call), 0);

  ASSERT_GE(sd_bus_message_exit_container(call), 0);  // a(ssa{sv})
  ASSERT_GE(sd_bus_message_exit_container(call), 0);  // struct

  // Second logical monitor keeps its geometry, scale and primary flag.
  ASSERT_GT(sd_bus_message_enter_container(call, SD_BUS_TYPE_STRUCT, "iiduba(ssa{sv})"), 0);
  ASSERT_GE(sd_bus_message_read(call, "iid", &x, &y, &scale), 0);
  ASSERT_GE(sd_bus_message_read(call, "u", &transform), 0);
  ASSERT_GE(sd_bus_message_read(call, "b", &primary), 0);
  EXPECT_EQ(x, 2560);
  EXPECT_DOUBLE_EQ(scale, 1.5);
  EXPECT_EQ(primary, 1);

  sd_bus_message_unref(call);
}

TEST_F(codec_test_t, RefusesALogicalMonitorWithAnUnknownConnector) {
  state_t state;
  state.serial = 1;
  logical_monitor_t logical;
  logical.connectors = { "DP-9" };
  state.logical_monitors.push_back(logical);

  sd_bus_message *call = new_call_message();
  ASSERT_NE(call, nullptr);
  // Naming a connector that has no monitor entry would produce a config Mutter
  // cannot resolve; fail here instead of sending a malformed call.
  EXPECT_FALSE(append_apply_args(call, state));
  sd_bus_message_unref(call);
}

TEST(MutterStateHelpers, AnswerFromStateAlone) {
  // Helpers take no bus: they are pure state queries, so they need no fixture.
  state_t state;
  state.serial = 1;

  monitor_t monitor;
  monitor.connector = "eDP-1";
  monitor.modes = { { "a", 1920, 1080, 59.94, true, false },
    { "b", 2560, 1440, 144.0, false, true } };
  state.monitors.push_back(monitor);

  logical_monitor_t logical;
  logical.primary = true;
  logical.connectors = { "eDP-1" };
  state.logical_monitors.push_back(logical);

  ASSERT_NE(find_monitor(state, "eDP-1"), nullptr);
  EXPECT_EQ(find_monitor(state, "HDMI-9"), nullptr);

  const auto *current = current_mode(*find_monitor(state, "eDP-1"));
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->id, "a");

  // A rate within tolerance matches, so 59.94 does not need to be spelled the
  // way the compositor printed it.
  EXPECT_NE(matching_mode(*find_monitor(state, "eDP-1"), 1920, 1080, 59.94), nullptr);
  EXPECT_NE(matching_mode(*find_monitor(state, "eDP-1"), 1920, 1080, 59.95), nullptr);
  EXPECT_EQ(matching_mode(*find_monitor(state, "eDP-1"), 1920, 1080, 60.5), nullptr);

  const auto *logical_found = find_logical_monitor(state, "eDP-1");
  ASSERT_NE(logical_found, nullptr);
  EXPECT_TRUE(logical_found->primary);
  // An output nobody shows is disabled, which is how Mutter expresses "off".
  EXPECT_EQ(find_logical_monitor(state, "HDMI-9"), nullptr);
}

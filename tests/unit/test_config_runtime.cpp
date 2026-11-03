/** @file test_config_runtime.cpp
 * Standalone suite exercising the real config.cpp update paths.
 *
 * These tests mutate global config state and call config::parse, which other
 * aggregate-suite fixtures cannot tolerate, and the former death-test child
 * re-ran logging initialization against a log file the parent already held
 * (crashing with 0xC0000005 on CI). Running them in their own process keeps
 * the coverage without a subprocess or cross-suite state leakage.
 */
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "src/config.h"
#include "src/display_device/display_device.h"
#include "src/globals.h"
#include "src/platform/common.h"
// config.cpp and file_handler.cpp reference these severity loggers and a few
// platform entry points that the host build supplies; provide stand-ins.
boost::log::sources::severity_logger<int> debug;
boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> warning;
boost::log::sources::severity_logger<int> error;
boost::log::sources::severity_logger<int> fatal;

// Globals from globals.cpp that config.cpp references; the host values are
// irrelevant to these tests.
bool display_cursor = true;
bool is_running_as_system_user = false;

namespace platf {
  std::filesystem::path
  appdata() {
    static const auto directory = [] {
      const auto path = std::filesystem::temp_directory_path() / "sunshine_config_runtime_tests";
      std::filesystem::create_directories(path);
      return path;
    }();
    return directory;
  }

  std::string
  get_host_name() {
    return "config-runtime-tests";
  }

  // config.cpp reads the first option as the default gamepad during static
  // initialization, so the stand-in list must never be empty.
  std::vector<supported_gamepad_t> &
  supported_gamepads(input_t *input) {
    (void) input;
    static std::vector<supported_gamepad_t> options {
      { "auto", true, "" },
      { "x360", true, "" },
      { "ds4", true, "" },
      { "ds5", true, "" },
    };
    return options;
  }

  void
  set_global_gamepad_mode(std::string_view preference) {
    (void) preference;
  }
}  // namespace platf

#ifdef _WIN32
  namespace platf {
    std::wstring
    from_utf8(const std::string &string) {
      return std::filesystem::path { std::u8string { string.begin(), string.end() } }.wstring();
    }

    std::string
    to_utf8(const std::wstring &string) {
      const auto utf8 = std::filesystem::path { string }.u8string();
      return { utf8.begin(), utf8.end() };
    }
  }  // namespace platf

  void
  launch_ui() {}

  namespace service_ctrl {
    bool
    is_service_running() {
      return false;
    }

    bool
    start_service() {
      return false;
    }

    bool
    wait_for_ui_ready() {
      return false;
    }
  }  // namespace service_ctrl

  // globals.cpp and device_topology.cpp are not linked here; parsed_config.cpp
  // only needs the names and a benign empty enumeration for these paths.
  const std::string VDD_NAME = "ZakoHDR";
  const std::string ZAKO_NAME = "Zako HDR";

  namespace display_device {
    boost::optional<device_info_map_t>
    enum_available_devices_checked() {
      return device_info_map_t {};
    }
  }  // namespace display_device
#endif

namespace logging {
  void
  print_help(const char *name) {
    (void) name;
  }
}  // namespace logging

namespace {
  class temporary_clients_config_t {
  public:
    temporary_clients_config_t():
        original_path_(config::sunshine.config_file),
        original_clients_(config::get_clients_config()),
        path_(
          std::filesystem::temp_directory_path() /
          ("sunshine_clients_config_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".conf")
        ) {
      config::sunshine.config_file = path_.string();
      std::ofstream(path_, std::ios::binary | std::ios::trunc) << "sunshine_name = retained\n";
    }

    ~temporary_clients_config_t() {
      config::sunshine.config_file = original_path_;
      config::nvhttp.clients = original_clients_;
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }

    std::string
    read() const {
      std::ifstream file(path_, std::ios::binary);
      return {
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
      };
    }

    int
    parse(std::vector<std::string> arguments) const {
      arguments.insert(arguments.begin(), "sunshine-config-test");
      // Keep startup file paths inside the fixture for every parse and reload.
      for (const auto name : { "pkey", "cert", "log_path", "file_state", "credentials_file", "file_apps" }) {
        arguments.emplace_back(std::string { name } + "=" + config::sunshine.config_file);
      }
      std::vector<char *> argv;
      for (auto &argument : arguments) argv.push_back(argument.data());
      argv.push_back(nullptr);
      return config::parse(static_cast<int>(argv.size()) - 1, argv.data());
    }

  private:
    std::string original_path_;
    std::string original_clients_;
    std::filesystem::path path_;
  };
}  // namespace

TEST(ConfigRuntime, VpeSwitchPersistsAndCanBeDisabledWithoutChangingOtherAmdSettings) {
  temporary_clients_config_t temporary_config;
  const auto original_amd = config::video.amd;
  struct restore_t {
    decltype(config::video.amd) value;
    ~restore_t() { config::video.amd = value; }
  } restore { original_amd };
  config::video.amd.amd_vpe = false;
  ASSERT_TRUE(config::update_config({ { "amd_vpe", "enabled" } }));
  ASSERT_EQ(temporary_config.parse({ config::sunshine.config_file }), 0);
  EXPECT_TRUE(config::video.amd.amd_vpe);
  EXPECT_EQ(config::parse_config(temporary_config.read()).at("amd_vpe"), "enabled");
  EXPECT_EQ(config::video.amd.amd_avcodec_compat, original_amd.amd_avcodec_compat);
  ASSERT_TRUE(config::update_config({ { "sunshine_name", "vpe-persist" } }));
  EXPECT_TRUE(config::video.amd.amd_vpe);
  ASSERT_TRUE(config::update_config({ { "amd_vpe", "disabled" } }));
  ASSERT_EQ(temporary_config.parse({ config::sunshine.config_file }), 0);
  EXPECT_FALSE(config::video.amd.amd_vpe);
  EXPECT_EQ(config::parse_config(temporary_config.read()).at("amd_vpe"), "disabled");
}

TEST(ConfigRuntime, CalibrationSaveKeepsClientsFromFullAndPartialSettingsUpdates) {
  temporary_clients_config_t temporary_config;
  ASSERT_TRUE(config::save_clients_config(R"([{"uuid":"client-1","name":"old"}])"));
  const nlohmann::json calibration {
    { "version", 1 }, { "userSid", "user" }, { "vddId", "vdd-client-1" },
    { "profileName", "client.icc" }, { "sha256", std::string(64, 'a') }
  };
  // Settings saves must never honor a binding submitted by the client side.
  const nlohmann::json fake_binding {
    { "version", 1 }, { "userSid", "attacker" }, { "vddId", "vdd-client-1" },
    { "profileName", "evil.icc" }, { "sha256", std::string(64, 'f') }
  };
  for (const bool full : { true, false }) {
    SCOPED_TRACE(full ? "full settings" : "partial settings");
    const auto name = full ? "full" : "partial";
    const auto requested = nlohmann::json::array({ {
      { "uuid", "client-1" }, { "name", name }, { "hdrCalibration", fake_binding },
      { "touch", { { "enabled", full } } },
    }, nullptr }).dump();
    const std::map<std::string, std::string> updates { { "clients", requested }, { "gamepad", config::input.gamepad } };
    ASSERT_TRUE(full ? config::update_full_config(updates) : config::update_config(updates));
    EXPECT_EQ(nlohmann::json::parse(config::get_clients_config())[0]["name"], name);
    const auto runtime_clients = nlohmann::json::parse(config::get_clients_config());
    if (full) {
      // The fixture starts with no saved binding: the submitted one must be dropped.
      EXPECT_FALSE(runtime_clients[0].contains("hdrCalibration"));
    }
    else {
      // The saved binding survives while the resubmitted fake stays out.
      EXPECT_EQ(runtime_clients[0]["hdrCalibration"], calibration);
    }
    ASSERT_TRUE(config::save_client_hdr_calibration("client-1", calibration));
    const auto persisted = nlohmann::json::parse(config::parse_config(temporary_config.read()).at("clients"));
    EXPECT_EQ(persisted[0]["name"], name);
    EXPECT_EQ(persisted[0]["touch"]["enabled"], full);
    EXPECT_EQ(persisted[0]["hdrCalibration"], calibration);
    EXPECT_TRUE(persisted[1].is_null());
  }
}

TEST(ConfigRuntime, UnrelatedUpdatesPreserveTheRuntimeCommandLineOverride) {
  temporary_clients_config_t temporary_config;
  ASSERT_TRUE(config::save_clients_config(R"([{"uuid":"client-2","name":"kept"}])"));
  const auto saved_clients = config::get_clients_config();
  ASSERT_EQ(temporary_config.parse({ "clients=[]" }), 0);
  ASSERT_EQ(config::get_clients_config(), "[]");
  for (int attempt = 0; attempt < 2; ++attempt) {
    ASSERT_TRUE(config::update_config({ { "sunshine_name", "retained" } }));
    EXPECT_EQ(config::get_clients_config(), "[]");
    const auto effective = config::get_effective_config_snapshot();
    ASSERT_TRUE(effective);
    EXPECT_EQ(effective->at("clients"), "[]");
  }
  ASSERT_TRUE(config::update_full_config({ { "sunshine_name", "retained" }, { "gamepad", config::input.gamepad } }));
  EXPECT_EQ(config::get_clients_config(), "[]");
  EXPECT_EQ(config::parse_config(temporary_config.read()).at("clients"), saved_clients);
  // Reload without the clients command-line override.
  ASSERT_EQ(temporary_config.parse({}), 0);
  ASSERT_EQ(config::get_clients_config(), saved_clients);
  ASSERT_TRUE(config::update_full_config({ { "clients", "[]" }, { "gamepad", config::input.gamepad } }));
  EXPECT_EQ(config::get_clients_config(), "[]");
  EXPECT_EQ(config::parse_config(temporary_config.read()).at("clients"), "[]");
}

TEST(ConfigRuntime, ExplicitClientDeletionClearsTheRuntimeList) {
  temporary_clients_config_t temporary_config;
  ASSERT_TRUE(config::save_clients_config(R"([{"uuid":"client-3","name":"gone"}])"));
  // update_config treats empty/null as an explicit deletion: file key gone, runtime reset.
  ASSERT_TRUE(config::update_config({ { "clients", "" } }));
  EXPECT_EQ(config::get_clients_config(), "[]");
  EXPECT_FALSE(config::parse_config(temporary_config.read()).contains("clients"));
  // A full-settings save that only omits clients must NOT reset the runtime value.
  ASSERT_TRUE(config::update_full_config({ { "sunshine_name", "retained" }, { "clients", "" }, { "gamepad", config::input.gamepad } }));
  EXPECT_EQ(config::get_clients_config(), "[]");
  // A later calibration save starts from the cleared list instead of resurrecting it.
  ASSERT_TRUE(config::save_client_hdr_calibration("client-3", nlohmann::json {
    { "version", 1 }, { "userSid", "user" }, { "vddId", "vdd-client-3" },
    { "profileName", "client.icc" }, { "sha256", std::string(64, 'a') }
  }));
  const auto persisted = nlohmann::json::parse(config::parse_config(temporary_config.read()).at("clients"));
  ASSERT_EQ(persisted.size(), 1);
  EXPECT_EQ(persisted[0]["uuid"], "client-3");
  EXPECT_FALSE(persisted[0].contains("name"));
  EXPECT_TRUE(persisted[0].contains("hdrCalibration"));
}

TEST(ConfigRuntime, CommandLineOverrideKeepsSavedClientBindings) {
  temporary_clients_config_t temporary_config;
  ASSERT_TRUE(config::save_clients_config(R"([{"uuid":"client-5","name":"old"}])"));
  const nlohmann::json calibration {
    { "version", 1 }, { "userSid", "user" }, { "vddId", "vdd-client-5" },
    { "profileName", "client.icc" }, { "sha256", std::string(64, 'a') }
  };
  ASSERT_EQ(temporary_config.parse({ R"(clients=[{"uuid":"client-5","name":"override"}])" }), 0);
  ASSERT_EQ(config::get_clients_config(), R"([{"uuid":"client-5","name":"override"}])");
  ASSERT_TRUE(config::save_client_hdr_calibration("client-5", calibration));
  // A sync under the override must keep the saved binding while the override's
  // own client fields win.
  ASSERT_TRUE(config::update_config({ { "clients", R"([{"uuid":"client-5","name":"saved"}])" } }));
  const auto runtime_clients = nlohmann::json::parse(config::get_clients_config());
  EXPECT_EQ(runtime_clients[0]["name"], "override");
  EXPECT_EQ(runtime_clients[0]["hdrCalibration"], calibration);
  const auto persisted = nlohmann::json::parse(config::parse_config(temporary_config.read()).at("clients"));
  EXPECT_EQ(persisted[0]["name"], "saved");
  EXPECT_EQ(persisted[0]["hdrCalibration"], calibration);
  // Reload without the override: the saved list wins again.
  ASSERT_EQ(temporary_config.parse({}), 0);
  const auto reloaded = nlohmann::json::parse(config::get_clients_config());
  EXPECT_EQ(reloaded[0]["name"], "saved");
  EXPECT_EQ(reloaded[0]["hdrCalibration"], calibration);
}

TEST(ConfigRuntime, EmptyClientsInFullSettingsAreTreatedAsOmitted) {
  temporary_clients_config_t temporary_config;
  ASSERT_TRUE(config::save_clients_config(R"([{"uuid":"client-6","name":"kept"}])"));
  const auto saved = config::get_clients_config();
  // An empty or "null" clients value in a full save keeps the saved list on
  // disk and at runtime; it must not tear the two apart across a restart.
  ASSERT_TRUE(config::update_full_config({ { "sunshine_name", "retained" }, { "clients", "" }, { "gamepad", config::input.gamepad } }));
  EXPECT_EQ(config::get_clients_config(), saved);
  EXPECT_EQ(config::parse_config(temporary_config.read()).at("clients"), saved);
  ASSERT_TRUE(config::update_full_config({ { "sunshine_name", "retained" }, { "clients", "null" }, { "gamepad", config::input.gamepad } }));
  EXPECT_EQ(config::get_clients_config(), saved);
  EXPECT_EQ(config::parse_config(temporary_config.read()).at("clients"), saved);
  // An explicit empty array still clears file and runtime.
  ASSERT_TRUE(config::update_full_config({ { "clients", "[]" }, { "gamepad", config::input.gamepad } }));
  EXPECT_EQ(config::get_clients_config(), "[]");
  EXPECT_EQ(config::parse_config(temporary_config.read()).at("clients"), "[]");
}

TEST(ConfigRuntime, CalibrationSaveUnderOverrideKeepsThePersistedList) {
  temporary_clients_config_t temporary_config;
  ASSERT_TRUE(config::save_clients_config(R"([{"uuid":"client-7","name":"a"},{"uuid":"client-8","name":"b"}])"));
  const nlohmann::json calibration7 {
    { "version", 1 }, { "userSid", "user" }, { "vddId", "vdd-client-7" },
    { "profileName", "seven.icc" }, { "sha256", std::string(64, 'a') }
  };
  const nlohmann::json calibration8 {
    { "version", 1 }, { "userSid", "user" }, { "vddId", "vdd-client-8" },
    { "profileName", "eight.icc" }, { "sha256", std::string(64, 'b') }
  };
  ASSERT_TRUE(config::save_client_hdr_calibration("client-8", calibration8));
  ASSERT_EQ(temporary_config.parse({ R"(clients=[{"uuid":"client-7","name":"override"}])" }), 0);
  ASSERT_TRUE(config::save_client_hdr_calibration("client-7", calibration7));
  // The persisted list keeps both clients and both bindings.
  const auto persisted = nlohmann::json::parse(config::parse_config(temporary_config.read()).at("clients"));
  ASSERT_EQ(persisted.size(), 2);
  EXPECT_EQ(persisted[0]["name"], "a");
  EXPECT_EQ(persisted[0]["hdrCalibration"], calibration7);
  EXPECT_EQ(persisted[1]["name"], "b");
  EXPECT_EQ(persisted[1]["hdrCalibration"], calibration8);
  // The runtime keeps the override's shape with the fresh binding carried over.
  const auto runtime = nlohmann::json::parse(config::get_clients_config());
  ASSERT_EQ(runtime.size(), 1);
  EXPECT_EQ(runtime[0]["name"], "override");
  EXPECT_EQ(runtime[0]["hdrCalibration"], calibration7);
  // Reload without the override.
  ASSERT_EQ(temporary_config.parse({}), 0);
  EXPECT_EQ(nlohmann::json::parse(config::get_clients_config()), persisted);
}

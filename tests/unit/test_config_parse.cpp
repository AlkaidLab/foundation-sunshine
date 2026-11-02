#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "src/config.h"

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

    std::string read() const {
      std::ifstream file(path_, std::ios::binary);
      return {
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
      };
    }

  private:
    std::string original_path_;
    std::string original_clients_;
    std::filesystem::path path_;
  };
}  // namespace

TEST(ConfigParse, PreservesHashCharactersInsideJsonStrings) {
  const auto parsed = config::parse_config(
    R"(clients = [{"uuid":"client-1","name":"Display #1 ]","hdrProfile":"Custom#.icc"}] # trailing comment
encoder = nvenc# ordinary comment
)"
  );

  ASSERT_TRUE(parsed.contains("clients"));
  EXPECT_EQ(parsed.at("clients"), R"([{"uuid":"client-1","name":"Display #1 ]","hdrProfile":"Custom#.icc"}])");
  ASSERT_TRUE(parsed.contains("encoder"));
  EXPECT_EQ(parsed.at("encoder"), "nvenc");
}

TEST(ConfigParse, ClientSettingsAreNormalizedBeforePersistence) {
  temporary_clients_config_t temporary_config;
  constexpr auto expected = R"([{"name":"Display #1","uuid":"client-1"}])";

  ASSERT_TRUE(config::save_clients_config(
    R"(
      [{"uuid":"client-1", "name":"Display #1"}]
    )"
  ));

  const auto persisted = config::parse_config(temporary_config.read());
  ASSERT_TRUE(persisted.contains("clients"));
  EXPECT_EQ(persisted.at("clients"), expected);
  EXPECT_EQ(persisted.at("sunshine_name"), "retained");
  EXPECT_EQ(config::get_clients_config(), expected);
}

TEST(ConfigParseDeathTest, CalibrationSaveKeepsClientsFromFullAndPartialSettingsUpdates) {
  // Startup parsing changes global settings and CLI overrides; isolate them.
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  const auto verify = [] {
    temporary_clients_config_t temporary_config;
    ASSERT_TRUE(config::save_clients_config(R"([{"uuid":"client-1","name":"old"}])"));
    const nlohmann::json calibration {
      { "version", 1 }, { "userSid", "user" }, { "vddId", "vdd-client-1" },
      { "profileName", "client.icc" }, { "sha256", std::string(64, 'a') }
    };
    for (const bool full : { true, false }) {
      SCOPED_TRACE(full ? "full settings" : "partial settings");
      const auto name = full ? "full" : "partial";
      const auto requested = nlohmann::json::array({ { { "uuid", "client-1" }, { "name", name }, { "touch", { { "enabled", full } } } }, nullptr }).dump();
      const std::map<std::string, std::string> updates { { "clients", requested }, { "gamepad", config::input.gamepad } };
      ASSERT_TRUE(full ? config::update_full_config(updates) : config::update_config(updates));
      EXPECT_EQ(nlohmann::json::parse(config::get_clients_config())[0]["name"], name);
      ASSERT_TRUE(config::save_client_hdr_calibration("client-1", calibration));
      const auto persisted = nlohmann::json::parse(config::parse_config(temporary_config.read()).at("clients"));
      EXPECT_EQ(persisted[0]["name"], name);
      EXPECT_EQ(persisted[0]["touch"]["enabled"], full);
      EXPECT_EQ(persisted[0]["hdrCalibration"], calibration);
      EXPECT_TRUE(persisted[1].is_null());
    }
    // Unrelated partial updates must preserve a runtime command-line override.
    const auto saved_clients = config::get_clients_config();
    std::vector<std::string> arguments { "sunshine-test", "clients=[]" };
    // Keep startup file paths in the fixture's temporary location.
    for (const auto name : { "pkey", "cert", "log_path", "file_state", "credentials_file", "file_apps" }) {
      arguments.emplace_back(std::string { name } + "=" + config::sunshine.config_file);
    }
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    ASSERT_EQ(config::parse(static_cast<int>(argv.size()) - 1, argv.data()), 0);
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
    argv.erase(argv.begin() + 1);  // Reload without the clients command-line override.
    ASSERT_EQ(config::parse(static_cast<int>(argv.size()) - 1, argv.data()), 0);
    ASSERT_EQ(config::get_clients_config(), saved_clients);
    ASSERT_TRUE(config::update_full_config({ { "clients", "[]" }, { "gamepad", config::input.gamepad } }));
    EXPECT_EQ(config::get_clients_config(), "[]");
    EXPECT_EQ(config::parse_config(temporary_config.read()).at("clients"), "[]");
  };
  ASSERT_EXIT(
    {
      verify();
      std::exit(::testing::Test::HasFailure() ? EXIT_FAILURE : EXIT_SUCCESS);
    },
    ::testing::ExitedWithCode(EXIT_SUCCESS), "");
}

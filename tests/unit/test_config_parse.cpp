#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

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

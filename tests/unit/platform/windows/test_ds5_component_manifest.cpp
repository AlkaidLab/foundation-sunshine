/** @file test_ds5_component_manifest.cpp
 * Production manifest validation, independent of the fake Sidecar test hook.
 */
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <filesystem>
  #include <fstream>
  #include <iterator>
  #include <optional>
  #include <gtest/gtest.h>
  #include "src/platform/windows/ds5/component_manifest.h"

namespace {
  class Ds5ComponentManifest: public testing::Test {
  protected:
    const std::string exe_digest = std::string(64, 'a');
    const std::string core_digest = std::string(64, 'b');
    std::filesystem::path root;
    std::filesystem::path active;
    boost::property_tree::ptree manifest;
    std::string error;

    static void write(const std::filesystem::path &path, const std::string &contents) {
      std::ofstream(path, std::ios::binary) << contents;
    }

    void SetUp() override {
      static unsigned sequence = 0;
      root = std::filesystem::temp_directory_path() /
             ("sunshine-ds5-manifest-" + std::to_string(GetCurrentProcessId()) + "-" +
              std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++));
      active = root / "active";
      std::filesystem::create_directories(active);
      // Hashing is supplied by the caller. Fixture payloads stand for digests,
      // so the test exercises real JSON parsing and file paths, not OpenSSL.
      write(active / "Sunshine.Ds5Sidecar.exe", exe_digest);
      write(active / "HIDMaestro.Core.dll", core_digest);
      manifest.put("component_version", "1.4.1");
      manifest.put("protocol", 1);
      manifest.put("sidecar_file", "Sunshine.Ds5Sidecar.exe");
      manifest.put("sidecar_sha256", exe_digest);
      manifest.put("sha256", core_digest);
      manifest.put("sidecar_package_sha256", std::string(64, 'c'));
    }

    void TearDown() override {
      ASSERT_TRUE(std::filesystem::equivalent(root.parent_path(), std::filesystem::temp_directory_path()));
      ASSERT_TRUE(root.filename().string().starts_with("sunshine-ds5-manifest-"));
      std::filesystem::remove_all(root);
    }

    void save() { boost::property_tree::write_json((active / "component.json").string(), manifest); }

    bool validate() {
      return platf::ds5::validate_component_manifest(active,
        [](const std::filesystem::path &path) -> std::optional<std::string> {
          std::ifstream input(path, std::ios::binary);
          if (!input) return std::nullopt;
          return std::string(std::istreambuf_iterator<char>(input), {});
        }, error);
    }
  };

  TEST_F(Ds5ComponentManifest, AcceptsInstallerSchemaUsingDllDigestInsteadOfZipDigest) {
    save();
    EXPECT_TRUE(validate()) << error;
  }

  TEST_F(Ds5ComponentManifest, AcceptsUppercaseDigests) {
    manifest.put("sidecar_sha256", std::string(64, 'A'));
    manifest.put("sha256", std::string(64, 'B'));
    save();
    EXPECT_TRUE(validate()) << error;
  }

  TEST_F(Ds5ComponentManifest, RejectsOldVersionAndWrongProtocol) {
    for (const auto &version : { "1.2.0", "1.3.0", "1.4.0" }) {
      manifest.put("component_version", version);
      save();
      EXPECT_FALSE(validate());
    }
    manifest.put("component_version", "1.4.1");
    manifest.put("protocol", 2);
    save();
    EXPECT_FALSE(validate());
  }

  TEST_F(Ds5ComponentManifest, RejectsMissingMalformedAndMismatchedCoreDigest) {
    manifest.erase("sha256");
    manifest.put("core_sha256", core_digest);  // An obsolete field cannot substitute for sha256.
    save();
    EXPECT_FALSE(validate());
    for (const auto &digest : { std::string(63, 'b'), std::string(64, 'c') }) {
      manifest.put("sha256", digest);
      save();
      EXPECT_FALSE(validate());
    }
  }

  TEST_F(Ds5ComponentManifest, RequiresCoreAtTheActiveComponentPath) {
    save();
    std::filesystem::rename(active / "HIDMaestro.Core.dll", root / "HIDMaestro.Core.dll");
    EXPECT_FALSE(validate());
  }

  TEST_F(Ds5ComponentManifest, RejectsReplacedAndMissingExecutable) {
    save();
    write(active / "Sunshine.Ds5Sidecar.exe", std::string(64, 'd'));
    EXPECT_FALSE(validate());
    std::filesystem::remove(active / "Sunshine.Ds5Sidecar.exe");
    EXPECT_FALSE(validate());
  }

  TEST_F(Ds5ComponentManifest, RejectsAlternateExecutableName) {
    manifest.put("sidecar_file", "other.exe");
    save();
    EXPECT_FALSE(validate());
  }

  TEST_F(Ds5ComponentManifest, RejectsUnreadableMalformedAndInvalidTypedManifest) {
    EXPECT_FALSE(validate());
    write(active / "component.json", "{ broken JSON");
    EXPECT_FALSE(validate());
    manifest.put("protocol", "not-a-number");
    save();
    EXPECT_FALSE(validate());
  }
}  // namespace
#endif

/**
 * @file tests/unit/test_hdr_enhanced_config.cpp
 * @brief Test independent HDR settings and component ownership.
 */
#include "../tests_common.h"
#include "src/hdr_enhanced/config.h"

#include <boost/thread/barrier.hpp>
#include <boost/thread/thread.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
  #include <windows.h>
#endif

namespace {
  class HdrEnhancedConfigTest: public ::testing::Test {
  protected:
    void
    SetUp() override {
      root = std::filesystem::temp_directory_path() /
             ("sunshine-hdr-" + boost::uuids::to_string(boost::uuids::random_generator()()));
      std::filesystem::create_directories(root);
      store = std::make_unique<hdr_enhanced::manager_t>(root / "hdr.json", root / "tools", catalog());
      ASSERT_TRUE(store->initialize());
    }
    void
    TearDown() override {
      store.reset();
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
    std::unique_ptr<hdr_enhanced::manager_t> store;

    nlohmann::json
    catalog() {
      return { { "schema_version", 1 }, { "adapters", {
        { "alkaidlab.nvidia_rtx_video", {
          { "foundation_rtx_video_adapter.dll", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" }
        } }
      } }, { "components", {
        { "alkaidlab.nvidia_rtx_video", { { "fixture", {
          { "nvngx_truehdr.dll", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" }
        } } } }
      } } };
    }

    hdr_enhanced::settings_t
    trusted_fixture() {
      const auto directory = root / "tools" / "hdr_enhanced" / "nvidia_rtx_video";
      std::filesystem::create_directories(directory);
      std::ofstream(directory / "foundation_rtx_video_adapter.dll") << "abc";
      std::ofstream(directory / "nvngx_truehdr.dll") << "abc";
      return { "alkaidlab.nvidia_rtx_video", { { "alkaidlab.nvidia_rtx_video", "fixture" } } };
    }
  };
}  // namespace

TEST_F(HdrEnhancedConfigTest, MissingDefaultDoesNotCreateAFileOrLoadAComponent) {
  const auto state = store->query();
  ASSERT_EQ(state.status, 200);
  EXPECT_TRUE(state.settings.selected_backend.empty());
  EXPECT_FALSE(store->acquire_selected());
  const auto saved = store->update(state.settings, state.etag);
  EXPECT_EQ(saved.status, 200);
  EXPECT_FALSE(saved.changed);
  EXPECT_FALSE(std::filesystem::exists(root / "hdr.json"));
}

TEST_F(HdrEnhancedConfigTest, RuntimeUsesThePackagedAdapterAndEmbeddedTrustCatalog) {
  const auto settings = trusted_fixture();
  std::ofstream(root / "trusted.json") << "{forged catalog";
  ASSERT_EQ(store->update(settings, store->query().etag).status, 200);
  auto use = store->acquire_selected();
  ASSERT_TRUE(use);
  EXPECT_EQ(use->path.filename(), "foundation_rtx_video_adapter.dll");
  EXPECT_EQ(store->status()["trusted_components"], catalog());
}

TEST_F(HdrEnhancedConfigTest, CorruptConfigurationCannotBeOverwrittenBySave) {
  std::ofstream(root / "hdr.json") << "{broken";
  EXPECT_EQ(store->query().status, 500);
  const auto result = store->update({}, std::nullopt);
  EXPECT_EQ(result.status, 500);
  EXPECT_EQ(result.error, "hdr_config_invalid");
  std::ifstream input(root / "hdr.json");
  std::string content;
  std::getline(input, content);
  EXPECT_EQ(content, "{broken");
}

TEST_F(HdrEnhancedConfigTest, JsonNullIsNotAMissingConfigurationFile) {
  std::ofstream(root / "hdr.json") << "null";
  EXPECT_EQ(store->query().status, 500);
  EXPECT_EQ(store->update({}, std::nullopt).status, 500);
}

TEST_F(HdrEnhancedConfigTest, EnforcesConditionalUpdatesAndKnownIdentities) {
  const auto state = store->query();
  EXPECT_EQ(store->update({}, std::nullopt).status, 428);
  EXPECT_EQ(store->update({}, "*").status, 400);
  auto malformed = state.etag;
  malformed[8] = 'z';
  EXPECT_EQ(store->update({}, malformed).status, 400);
  auto stale = state.etag;
  stale[stale.size() - 2] = stale[stale.size() - 2] == 'a' ? 'b' : 'a';
  EXPECT_EQ(store->update({}, stale).status, 412);
  auto settings = state.settings;
  settings.selected_backend = "unregistered";
  EXPECT_EQ(store->update(settings, state.etag).status, 400);
}

TEST_F(HdrEnhancedConfigTest, MaintenanceSurvivesRestartAndRequiresItsOwnerToken) {
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  EXPECT_FALSE(operation.empty());
  EXPECT_TRUE(store->status()["maintenance"]);
  EXPECT_EQ(store->finish_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, "wrong").status, 409);
  store.reset();
  store = std::make_unique<hdr_enhanced::manager_t>(root / "hdr.json", root / "tools", catalog());
  ASSERT_TRUE(store->initialize());
  EXPECT_TRUE(store->status()["maintenance"]);
  EXPECT_EQ(store->finish_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  EXPECT_FALSE(store->status()["maintenance"]);
}

TEST_F(HdrEnhancedConfigTest, ExistingSessionRetainsItsVersionAfterSelectionIsDisabled) {
  const auto settings = trusted_fixture();
  ASSERT_EQ(store->update(settings, store->query().etag).status, 200);
  EXPECT_EQ(store->status().value("selected_backend", std::string {}), hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND);
  auto session = store->acquire_selected();
  ASSERT_TRUE(session);
  EXPECT_EQ(session->version, "fixture");
  auto disabled = settings;
  disabled.selected_backend.clear();
  ASSERT_EQ(store->update(disabled, store->query().etag).status, 200);
  EXPECT_EQ(store->status().value("selected_backend", std::string {}), "");
  EXPECT_FALSE(store->acquire_selected());
  std::string operation;
  EXPECT_EQ(store->begin_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 409);
  session.reset();
  ASSERT_EQ(store->begin_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  EXPECT_EQ(store->finish_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
}

TEST_F(HdrEnhancedConfigTest, ConcurrentWritersCannotOverwriteTheSameSnapshot) {
  const auto settings = trusted_fixture();
  const auto etag = store->query().etag;
  boost::barrier start(3);
  int first = 0, second = 0;
  boost::thread a([&] { start.wait(); first = store->update(settings, etag).status; });
  boost::thread b([&] { start.wait(); second = store->update(settings, etag).status; });
  start.wait();
  a.join();
  b.join();
  EXPECT_TRUE((first == 200 && second == 412) || (first == 412 && second == 200));
}

TEST_F(HdrEnhancedConfigTest, WriteFailureDoesNotPublishTheNewSelection) {
  const auto selected = trusted_fixture();
  const auto original = store->query();
  std::filesystem::create_directory(root / "hdr.json.tmp");
  EXPECT_EQ(store->update(selected, original.etag).status, 500);
  EXPECT_FALSE(store->acquire_selected());
  EXPECT_EQ(store->query().etag, original.etag);
  EXPECT_FALSE(std::filesystem::exists(root / "hdr.json"));
}

TEST_F(HdrEnhancedConfigTest, MaintenanceMustBeVerifiedByTheRunningManager) {
  std::string operation;
  EXPECT_EQ(store->verify_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, "invented").status, 409);
  ASSERT_EQ(store->begin_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  EXPECT_EQ(store->verify_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  std::string inspected;
  ASSERT_EQ(store->inspect_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, inspected).status, 200);
  EXPECT_EQ(operation, inspected);
  ASSERT_EQ(store->finish_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  EXPECT_EQ(store->verify_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 409);
}

TEST_F(HdrEnhancedConfigTest, MaintenanceCompletionPreservesConfigurationErrors) {
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  std::ofstream(root / "hdr.json") << "{broken";
  const auto result = store->finish_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation);
  EXPECT_EQ(result.status, 500);
  EXPECT_EQ(result.error, "hdr_config_invalid");
  EXPECT_TRUE(store->status()["maintenance"]);
}

TEST_F(HdrEnhancedConfigTest, ReplacedFilesCannotReuseThePreMaintenanceValidation) {
  const auto selected = trusted_fixture();
  ASSERT_EQ(store->update(selected, store->query().etag).status, 200);
  ASSERT_TRUE(store->acquire_selected());
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  EXPECT_FALSE(store->status()["selection_verified"]);
  std::ofstream(root / "tools/hdr_enhanced/nvidia_rtx_video/nvngx_truehdr.dll") << "different";
  const auto result = store->update(selected, store->query().etag, operation);
  EXPECT_EQ(result.status, 400);
  EXPECT_EQ(result.error, "hdr_component_untrusted");
  EXPECT_EQ(store->finish_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 409);
  EXPECT_TRUE(store->status()["maintenance"]);
  EXPECT_FALSE(store->acquire_selected());
  // 恢复允许用户进入修复流程，但不能加载与配置不匹配的文件。
  ASSERT_EQ(store->recover_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND).status, 200);
  EXPECT_FALSE(store->status()["maintenance"]);
  EXPECT_FALSE(store->acquire_selected());
}

TEST_F(HdrEnhancedConfigTest, DisabledInstallationStillValidatesThePublishedVersion) {
  auto installed = trusted_fixture();
  installed.selected_backend.clear();
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  std::ofstream(root / "tools/hdr_enhanced/nvidia_rtx_video/nvngx_truehdr.dll") << "different";
  EXPECT_EQ(store->update(installed, store->query().etag, operation).status, 400);
  EXPECT_TRUE(store->query().settings.versions.empty());
}

TEST_F(HdrEnhancedConfigTest, DamagedRuntimeCanStillBeDisabledForRemoval) {
  auto selected = trusted_fixture();
  ASSERT_EQ(store->update(selected, store->query().etag).status, 200);
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  std::filesystem::remove(root / "tools/hdr_enhanced/nvidia_rtx_video/nvngx_truehdr.dll");
  selected.selected_backend.clear();
  EXPECT_EQ(store->update(selected, store->query().etag, operation).status, 200);
  EXPECT_FALSE(store->acquire_selected());
}

#ifdef _WIN32
TEST_F(HdrEnhancedConfigTest, HelperCanCommitConfigurationBeforeReleasingItsFileLock) {
  const auto selected = trusted_fixture();
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  auto path = store->maintenance_path();
  path += ".lock";
  const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  EXPECT_EQ(store->update(selected, store->query().etag, operation).status, 200);
  EXPECT_FALSE(store->acquire_selected());
  EXPECT_EQ(store->finish_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 409);
  CloseHandle(handle);
  ASSERT_EQ(store->finish_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  EXPECT_TRUE(store->acquire_selected());
}

TEST_F(HdrEnhancedConfigTest, RecoveryCannotReleaseAnActiveHelperWriteLock) {
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  auto path = store->maintenance_path();
  path += ".lock";
  const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  EXPECT_EQ(store->recover_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND).status, 409);
  EXPECT_TRUE(store->status()["maintenance"]);
  CloseHandle(handle);
  EXPECT_EQ(store->recover_maintenance(hdr_enhanced::NVIDIA_RTX_VIDEO_BACKEND).status, 200);
  EXPECT_FALSE(store->status()["maintenance"]);
  EXPECT_EQ(store->update({}, store->query().etag, operation).status, 409);
}
#endif

TEST(HdrEnhancedSettingsTest, RejectsPathsAndUnregisteredBackends) {
  EXPECT_FALSE(hdr_enhanced::valid_version("../outside"));
  EXPECT_FALSE(hdr_enhanced::valid_version("C:\\outside"));
  EXPECT_TRUE(hdr_enhanced::valid_version("abcd-1234"));
  hdr_enhanced::settings_t settings;
  EXPECT_FALSE(hdr_enhanced::parse_settings(nlohmann::json {
                                                 { "schema_version", 1 }, { "selected_backend", "unknown" }, { "backends", nlohmann::json::object() } },
    settings));
}

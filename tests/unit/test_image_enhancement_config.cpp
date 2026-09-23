/**
 * @file tests/unit/test_image_enhancement_config.cpp
 * @brief Test independent HDR settings and component ownership.
 */
#include "../tests_common.h"
#include "src/image_enhancement/config.h"

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
  class ImageEnhancementConfigTest: public ::testing::Test {
  protected:
    void
    SetUp() override {
      root = std::filesystem::temp_directory_path() /
             ("sunshine-hdr-" + boost::uuids::to_string(boost::uuids::random_generator()()));
      std::filesystem::create_directories(root);
      store = std::make_unique<image_enhancement::manager_t>(root / "hdr.json", root / "tools", catalog());
      ASSERT_TRUE(store->initialize());
    }
    void
    TearDown() override {
      store.reset();
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
    std::unique_ptr<image_enhancement::manager_t> store;

    nlohmann::json
    catalog() {
      return { { "schema_version", 1 }, { "adapters", {
        { "alkaidlab.nvidia_rtx_video", {
          { "foundation_rtx_video_adapter.dll", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" }
        } },
        { "alkaidlab.nvidia_dlssnr", {
          { "foundation_dlssnr_adapter.dll", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" }
        } }
      } }, { "components", {
        { "alkaidlab.nvidia_rtx_video", { { "fixture", {
          { "nvngx_truehdr.dll", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" }
        } } } }
      } } };
    }

    image_enhancement::settings_t
    trusted_fixture() {
      const auto directory = root / "tools" / "hdr_enhanced" / "nvidia_rtx_video";
      std::filesystem::create_directories(directory);
      std::ofstream(directory / "foundation_rtx_video_adapter.dll") << "abc";
      std::ofstream(directory / "nvngx_truehdr.dll") << "abc";
      image_enhancement::settings_t settings;
      settings.selected_backend = std::string { image_enhancement::NVIDIA_RTX_VIDEO_BACKEND };
      settings.versions.emplace(std::string { image_enhancement::NVIDIA_RTX_VIDEO_BACKEND }, "fixture");
      return settings;
    }

    void
    create_nr_fixture() {
      const auto directory = root / "tools" / "hdr_enhanced" / "nvidia_dlssnr";
      std::filesystem::create_directories(directory);
      std::ofstream(directory / "foundation_dlssnr_adapter.dll") << "abc";
      std::ofstream(directory / "nvngx_dlssnr.dll") << "abc";
    }

    static constexpr const char *FIXTURE_DIGEST = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  };
}  // namespace

TEST_F(ImageEnhancementConfigTest, MissingDefaultDoesNotCreateAFileOrLoadAComponent) {
  const auto state = store->query();
  ASSERT_EQ(state.status, 200);
  EXPECT_TRUE(state.settings.selected_backend.empty());
  EXPECT_FALSE(store->acquire_selected(image_enhancement::backend_capability_e::hdr));
  const auto saved = store->update(state.settings, state.etag);
  EXPECT_EQ(saved.status, 200);
  EXPECT_FALSE(saved.changed);
  EXPECT_FALSE(std::filesystem::exists(root / "hdr.json"));
}

TEST_F(ImageEnhancementConfigTest, RelativeConfigurationExposesAbsoluteMaintenanceJournal) {
  const std::filesystem::path relative_config = "config/hdr_enhanced.json";
  image_enhancement::manager_t relative_store(relative_config, root / "tools", catalog());
  const auto journal = relative_store.maintenance_path();
  EXPECT_TRUE(journal.is_absolute());
  EXPECT_EQ(journal, std::filesystem::current_path() / "config/hdr_enhanced.maintenance.json");
}

TEST_F(ImageEnhancementConfigTest, RuntimeUsesThePackagedAdapterAndEmbeddedTrustCatalog) {
  const auto settings = trusted_fixture();
  std::ofstream(root / "trusted.json") << "{forged catalog";
  ASSERT_EQ(store->update(settings, store->query().etag).status, 200);
  auto use = store->acquire_selected(image_enhancement::backend_capability_e::hdr);
  ASSERT_TRUE(use);
  EXPECT_EQ(use->path.filename(), "foundation_rtx_video_adapter.dll");
  EXPECT_EQ(store->status()["trusted_components"], catalog());
}

TEST_F(ImageEnhancementConfigTest, CorruptConfigurationCannotBeOverwrittenBySave) {
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

TEST_F(ImageEnhancementConfigTest, JsonNullIsNotAMissingConfigurationFile) {
  std::ofstream(root / "hdr.json") << "null";
  EXPECT_EQ(store->query().status, 500);
  EXPECT_EQ(store->update({}, std::nullopt).status, 500);
}

TEST_F(ImageEnhancementConfigTest, EnforcesConditionalUpdatesAndKnownIdentities) {
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

TEST_F(ImageEnhancementConfigTest, MaintenanceSurvivesRestartAndRequiresItsOwnerToken) {
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  EXPECT_FALSE(operation.empty());
  EXPECT_TRUE(store->status()["maintenance"]);
  EXPECT_EQ(store->finish_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, "wrong").status, 409);
  store.reset();
  store = std::make_unique<image_enhancement::manager_t>(root / "hdr.json", root / "tools", catalog());
  ASSERT_TRUE(store->initialize());
  EXPECT_TRUE(store->status()["maintenance"]);
  EXPECT_EQ(store->finish_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  EXPECT_FALSE(store->status()["maintenance"]);
}

TEST_F(ImageEnhancementConfigTest, DeferredRemovalCompletesOnStartupAndPreservesOtherBackend) {
  auto selected = trusted_fixture();
  create_nr_fixture();
  selected.versions.emplace(std::string { image_enhancement::NVIDIA_DLSSNR_BACKEND }, "other-version");
  ASSERT_EQ(store->update(selected, store->query().etag).status, 200);
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  selected.selected_backend.clear();
  ASSERT_EQ(store->update(selected, store->query().etag, operation).status, 200);
  EXPECT_EQ(store->defer_removal(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, "wrong").status, 409);
  EXPECT_EQ(store->defer_removal(image_enhancement::NVIDIA_DLSSNR_BACKEND, operation).status, 409);
  ASSERT_EQ(store->defer_removal(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  EXPECT_EQ(store->status()["pending_removal"], image_enhancement::NVIDIA_RTX_VIDEO_BACKEND);
  EXPECT_EQ(store->finish_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 409);
  EXPECT_EQ(store->recover_maintenance(image_enhancement::NVIDIA_DLSSNR_BACKEND).status, 409);

  store.reset();
  store = std::make_unique<image_enhancement::manager_t>(root / "hdr.json", root / "tools", catalog());
  ASSERT_TRUE(store->initialize());
  EXPECT_FALSE(std::filesystem::exists(root / "tools/hdr_enhanced/nvidia_rtx_video/nvngx_truehdr.dll"));
  EXPECT_TRUE(std::filesystem::exists(root / "tools/hdr_enhanced/nvidia_dlssnr/foundation_dlssnr_adapter.dll"));
  EXPECT_TRUE(std::filesystem::exists(root / "tools/hdr_enhanced/nvidia_dlssnr/nvngx_dlssnr.dll"));
  EXPECT_FALSE(std::filesystem::exists(store->maintenance_path()));
  EXPECT_FALSE(store->status()["maintenance"]);
  const auto current = store->query();
  ASSERT_EQ(current.status, 200);
  EXPECT_FALSE(current.settings.versions.contains(std::string { image_enhancement::NVIDIA_RTX_VIDEO_BACKEND }));
  EXPECT_EQ(current.settings.versions.at(std::string { image_enhancement::NVIDIA_DLSSNR_BACKEND }), "other-version");
}

TEST_F(ImageEnhancementConfigTest, DeferredRemovalRetriesAfterConfigurationWriteFailure) {
  auto selected = trusted_fixture();
  ASSERT_EQ(store->update(selected, store->query().etag).status, 200);
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  selected.selected_backend.clear();
  ASSERT_EQ(store->update(selected, store->query().etag, operation).status, 200);
  ASSERT_EQ(store->defer_removal(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  std::filesystem::create_directory(root / "hdr.json.tmp");

  store.reset();
  store = std::make_unique<image_enhancement::manager_t>(root / "hdr.json", root / "tools", catalog());
  ASSERT_TRUE(store->initialize());
  EXPECT_TRUE(store->status()["maintenance"]);
  EXPECT_EQ(store->status()["pending_removal"], image_enhancement::NVIDIA_RTX_VIDEO_BACKEND);
  EXPECT_TRUE(std::filesystem::exists(store->maintenance_path()));
  EXPECT_FALSE(std::filesystem::exists(root / "tools/hdr_enhanced/nvidia_rtx_video/nvngx_truehdr.dll"));

  std::filesystem::remove(root / "hdr.json.tmp");
  ASSERT_EQ(store->recover_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND).status, 200);
  EXPECT_FALSE(store->status()["maintenance"]);
  EXPECT_FALSE(std::filesystem::exists(store->maintenance_path()));
  EXPECT_TRUE(store->query().settings.versions.empty());
}

TEST_F(ImageEnhancementConfigTest, ForgedDeferredRemovalCannotDeleteAnotherComponent) {
  const auto selected = trusted_fixture();
  create_nr_fixture();
  ASSERT_EQ(store->update(selected, store->query().etag).status, 200);
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  std::ofstream(store->maintenance_path(), std::ios::trunc)
    << nlohmann::json { { "operation_id", operation },
         { "component_id", image_enhancement::NVIDIA_RTX_VIDEO_BACKEND },
         { "pending_remove", image_enhancement::NVIDIA_DLSSNR_BACKEND } }.dump();

  store.reset();
  store = std::make_unique<image_enhancement::manager_t>(root / "hdr.json", root / "tools", catalog());
  ASSERT_TRUE(store->initialize());
  EXPECT_TRUE(store->status()["maintenance"]);
  EXPECT_EQ(store->status()["pending_removal"], "");
  EXPECT_TRUE(std::filesystem::exists(root / "tools/hdr_enhanced/nvidia_rtx_video/nvngx_truehdr.dll"));
  EXPECT_TRUE(std::filesystem::exists(root / "tools/hdr_enhanced/nvidia_dlssnr/foundation_dlssnr_adapter.dll"));
  EXPECT_TRUE(std::filesystem::exists(root / "tools/hdr_enhanced/nvidia_dlssnr/nvngx_dlssnr.dll"));
  EXPECT_TRUE(store->query().settings.versions.contains(std::string { image_enhancement::NVIDIA_RTX_VIDEO_BACKEND }));
  EXPECT_EQ(store->recover_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND).status, 200);
  EXPECT_FALSE(std::filesystem::exists(store->maintenance_path()));
}

TEST_F(ImageEnhancementConfigTest, DeferredRemovalKeepsJournalWhenConfigurationIsInvalid) {
  auto selected = trusted_fixture();
  ASSERT_EQ(store->update(selected, store->query().etag).status, 200);
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  selected.selected_backend.clear();
  ASSERT_EQ(store->update(selected, store->query().etag, operation).status, 200);
  ASSERT_EQ(store->defer_removal(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  std::ofstream(root / "hdr.json", std::ios::trunc) << "{broken";

  store.reset();
  store = std::make_unique<image_enhancement::manager_t>(root / "hdr.json", root / "tools", catalog());
  EXPECT_FALSE(store->initialize());
  EXPECT_EQ(store->status()["pending_removal"], image_enhancement::NVIDIA_RTX_VIDEO_BACKEND);
  EXPECT_EQ(store->recover_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND).status, 500);
  EXPECT_TRUE(std::filesystem::exists(store->maintenance_path()));
}

TEST_F(ImageEnhancementConfigTest, DeferredRemovalCannotBeCompletedAfterJournalTampering) {
  auto selected = trusted_fixture();
  ASSERT_EQ(store->update(selected, store->query().etag).status, 200);
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  selected.selected_backend.clear();
  ASSERT_EQ(store->update(selected, store->query().etag, operation).status, 200);
  ASSERT_EQ(store->defer_removal(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  std::ofstream(store->maintenance_path(), std::ios::trunc)
    << nlohmann::json { { "operation_id", operation },
         { "component_id", image_enhancement::NVIDIA_RTX_VIDEO_BACKEND } }.dump();

  EXPECT_EQ(store->recover_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND).status, 409);
  EXPECT_TRUE(store->status()["maintenance"]);
  EXPECT_TRUE(std::filesystem::exists(root / "tools/hdr_enhanced/nvidia_rtx_video/nvngx_truehdr.dll"));
}

TEST_F(ImageEnhancementConfigTest, ExistingSessionRetainsItsVersionAfterSelectionIsDisabled) {
  const auto settings = trusted_fixture();
  ASSERT_EQ(store->update(settings, store->query().etag).status, 200);
  EXPECT_EQ(store->status().value("selected_backend", std::string {}), image_enhancement::NVIDIA_RTX_VIDEO_BACKEND);
  auto session = store->acquire_selected(image_enhancement::backend_capability_e::hdr);
  ASSERT_TRUE(session);
  EXPECT_EQ(session->version, "fixture");
  auto disabled = settings;
  disabled.selected_backend.clear();
  ASSERT_EQ(store->update(disabled, store->query().etag).status, 200);
  EXPECT_EQ(store->status().value("selected_backend", std::string {}), "");
  EXPECT_FALSE(store->acquire_selected(image_enhancement::backend_capability_e::hdr));
  std::string operation;
  EXPECT_EQ(store->begin_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 409);
  session.reset();
  ASSERT_EQ(store->begin_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  EXPECT_EQ(store->finish_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
}

TEST_F(ImageEnhancementConfigTest, ConcurrentWritersCannotOverwriteTheSameSnapshot) {
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

TEST_F(ImageEnhancementConfigTest, WriteFailureDoesNotPublishTheNewSelection) {
  const auto selected = trusted_fixture();
  const auto original = store->query();
  std::filesystem::create_directory(root / "hdr.json.tmp");
  EXPECT_EQ(store->update(selected, original.etag).status, 500);
  EXPECT_FALSE(store->acquire_selected(image_enhancement::backend_capability_e::hdr));
  EXPECT_EQ(store->query().etag, original.etag);
  EXPECT_FALSE(std::filesystem::exists(root / "hdr.json"));
}

TEST_F(ImageEnhancementConfigTest, MaintenanceMustBeVerifiedByTheRunningManager) {
  std::string operation;
  EXPECT_EQ(store->verify_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, "invented").status, 409);
  ASSERT_EQ(store->begin_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  EXPECT_EQ(store->verify_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  std::string inspected;
  ASSERT_EQ(store->inspect_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, inspected).status, 200);
  EXPECT_EQ(operation, inspected);
  ASSERT_EQ(store->finish_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  EXPECT_EQ(store->verify_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 409);
}

TEST_F(ImageEnhancementConfigTest, MaintenanceCompletionPreservesConfigurationErrors) {
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  std::ofstream(root / "hdr.json") << "{broken";
  const auto result = store->finish_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation);
  EXPECT_EQ(result.status, 500);
  EXPECT_EQ(result.error, "hdr_config_invalid");
  EXPECT_TRUE(store->status()["maintenance"]);
}

TEST_F(ImageEnhancementConfigTest, ReplacedFilesCannotReuseThePreMaintenanceValidation) {
  const auto selected = trusted_fixture();
  ASSERT_EQ(store->update(selected, store->query().etag).status, 200);
  ASSERT_TRUE(store->acquire_selected(image_enhancement::backend_capability_e::hdr));
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  EXPECT_FALSE(store->status()["selection_verified"]);
  std::ofstream(root / "tools/hdr_enhanced/nvidia_rtx_video/nvngx_truehdr.dll") << "different";
  const auto result = store->update(selected, store->query().etag, operation);
  EXPECT_EQ(result.status, 400);
  EXPECT_EQ(result.error, "hdr_component_untrusted");
  EXPECT_EQ(store->finish_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 409);
  EXPECT_TRUE(store->status()["maintenance"]);
  EXPECT_FALSE(store->acquire_selected(image_enhancement::backend_capability_e::hdr));
  // 恢复允许用户进入修复流程，但不能加载与配置不匹配的文件。
  ASSERT_EQ(store->recover_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND).status, 200);
  EXPECT_FALSE(store->status()["maintenance"]);
  EXPECT_FALSE(store->acquire_selected(image_enhancement::backend_capability_e::hdr));
}

TEST_F(ImageEnhancementConfigTest, DisabledInstallationStillValidatesThePublishedVersion) {
  auto installed = trusted_fixture();
  installed.selected_backend.clear();
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  std::ofstream(root / "tools/hdr_enhanced/nvidia_rtx_video/nvngx_truehdr.dll") << "different";
  EXPECT_EQ(store->update(installed, store->query().etag, operation).status, 400);
  EXPECT_TRUE(store->query().settings.versions.empty());
}

TEST_F(ImageEnhancementConfigTest, DamagedRuntimeCanStillBeDisabledForRemoval) {
  auto selected = trusted_fixture();
  ASSERT_EQ(store->update(selected, store->query().etag).status, 200);
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  std::filesystem::remove(root / "tools/hdr_enhanced/nvidia_rtx_video/nvngx_truehdr.dll");
  selected.selected_backend.clear();
  EXPECT_EQ(store->update(selected, store->query().etag, operation).status, 200);
  EXPECT_FALSE(store->acquire_selected(image_enhancement::backend_capability_e::hdr));
}

#ifdef _WIN32
TEST_F(ImageEnhancementConfigTest, HelperCanCommitConfigurationBeforeReleasingItsFileLock) {
  const auto selected = trusted_fixture();
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  auto path = store->maintenance_path();
  path += ".lock";
  const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  EXPECT_EQ(store->update(selected, store->query().etag, operation).status, 200);
  EXPECT_FALSE(store->acquire_selected(image_enhancement::backend_capability_e::hdr));
  EXPECT_EQ(store->finish_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 409);
  CloseHandle(handle);
  ASSERT_EQ(store->finish_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  EXPECT_TRUE(store->acquire_selected(image_enhancement::backend_capability_e::hdr));
}

TEST_F(ImageEnhancementConfigTest, RecoveryCannotReleaseAnActiveHelperWriteLock) {
  std::string operation;
  ASSERT_EQ(store->begin_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND, operation).status, 200);
  auto path = store->maintenance_path();
  path += ".lock";
  const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  EXPECT_EQ(store->recover_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND).status, 409);
  EXPECT_TRUE(store->status()["maintenance"]);
  CloseHandle(handle);
  EXPECT_EQ(store->recover_maintenance(image_enhancement::NVIDIA_RTX_VIDEO_BACKEND).status, 200);
  EXPECT_FALSE(store->status()["maintenance"]);
  EXPECT_EQ(store->update({}, store->query().etag, operation).status, 409);
}
#endif

TEST(ImageEnhancementSettingsTest, RejectsPathsAndUnregisteredBackends) {
  EXPECT_FALSE(image_enhancement::valid_version("../outside"));
  EXPECT_FALSE(image_enhancement::valid_version("C:\\outside"));
  EXPECT_TRUE(image_enhancement::valid_version("abcd-1234"));
  image_enhancement::settings_t settings;
  EXPECT_FALSE(image_enhancement::parse_settings(nlohmann::json {
                                                 { "schema_version", 1 }, { "selected_backend", "unknown" }, { "backends", nlohmann::json::object() } },
    settings));
  // Capability slots only accept the backend registered for that slot.
  EXPECT_FALSE(image_enhancement::parse_settings(nlohmann::json {
                                                 { "schema_version", 1 },
                                                 { "selected_backend", "alkaidlab.nvidia_dlssnr" },
                                                 { "backends", nlohmann::json::object() } },
    settings));
  EXPECT_FALSE(image_enhancement::parse_settings(nlohmann::json {
                                                 { "schema_version", 2 },
                                                 { "selected", { { "hdr", nullptr }, { "nr", "alkaidlab.nvidia_rtx_video" } } },
                                                 { "backends", nlohmann::json::object() } },
    settings));
  EXPECT_FALSE(image_enhancement::parse_settings(nlohmann::json {
                                                 { "schema_version", 2 },
                                                 { "selected", { { "hdr", nullptr }, { "nr", nullptr } } },
                                                 { "backends", { { "alkaidlab.nvidia_dlssnr", { { "version", "310-8" }, { "runtime_sha256", "zz" } } } } } },
    settings));
  EXPECT_TRUE(image_enhancement::parse_settings(nlohmann::json {
                                                { "schema_version", 2 },
                                                { "selected", { { "hdr", nullptr }, { "nr", "alkaidlab.nvidia_dlssnr" } } },
                                                { "backends", { { "alkaidlab.nvidia_dlssnr", { { "version", "310-8" }, { "runtime_sha256", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" } } } } } },
    settings));
}

TEST_F(ImageEnhancementConfigTest, V1ConfigurationMigratesToSchemaV2OnNextWrite) {
  trusted_fixture();
  std::ofstream(root / "hdr.json")
    << "{\"schema_version\":1,\"selected_backend\":\"alkaidlab.nvidia_rtx_video\","
    << "\"backends\":{\"alkaidlab.nvidia_rtx_video\":{\"version\":\"fixture\"}}}";
  store = std::make_unique<image_enhancement::manager_t>(root / "hdr.json", root / "tools", catalog());
  ASSERT_TRUE(store->initialize());
  const auto state = store->query();
  ASSERT_EQ(state.status, 200);
  EXPECT_EQ(state.settings.selected_backend, image_enhancement::NVIDIA_RTX_VIDEO_BACKEND);
  EXPECT_TRUE(store->acquire_selected(image_enhancement::backend_capability_e::hdr));

  auto disabled = state.settings;
  disabled.selected_backend.clear();
  ASSERT_EQ(store->update(disabled, state.etag).status, 200);
  std::ifstream input(root / "hdr.json");
  const auto document = nlohmann::json::parse(input);
  EXPECT_EQ(document.at("schema_version"), 2);
  EXPECT_TRUE(document.contains("selected"));
  EXPECT_EQ(document.at("selected").at("hdr"), nullptr);
}

TEST_F(ImageEnhancementConfigTest, NrComponentValidatesAgainstTheSettingsPinnedRuntime) {
  create_nr_fixture();
  image_enhancement::settings_t settings;
  settings.selected_nr_backend = image_enhancement::NVIDIA_DLSSNR_BACKEND;
  settings.versions["alkaidlab.nvidia_dlssnr"] = "310-8";
  settings.runtime_pins["alkaidlab.nvidia_dlssnr"] = FIXTURE_DIGEST;
  ASSERT_EQ(store->update(settings, store->query().etag).status, 200);
  auto use = store->acquire_selected(image_enhancement::backend_capability_e::nr);
  ASSERT_TRUE(use);
  EXPECT_EQ(use->runtime_digest, FIXTURE_DIGEST);
  // The NR selection never satisfies the HDR capability slot.
  EXPECT_FALSE(store->acquire_selected(image_enhancement::backend_capability_e::hdr));
}

TEST_F(ImageEnhancementConfigTest, NrComponentRejectsAWrongPinnedRuntimeDigest) {
  create_nr_fixture();
  image_enhancement::settings_t settings;
  settings.selected_nr_backend = image_enhancement::NVIDIA_DLSSNR_BACKEND;
  settings.versions["alkaidlab.nvidia_dlssnr"] = "310-8";
  settings.runtime_pins["alkaidlab.nvidia_dlssnr"] = std::string(64, 'a');
  EXPECT_EQ(store->update(settings, store->query().etag).status, 400);
  EXPECT_FALSE(store->acquire_selected(image_enhancement::backend_capability_e::nr));
}

TEST_F(ImageEnhancementConfigTest, NrComponentAcceptsAnUnpinnedRuntimeForTheRecord) {
  create_nr_fixture();
  image_enhancement::settings_t settings;
  settings.selected_nr_backend = image_enhancement::NVIDIA_DLSSNR_BACKEND;
  settings.versions["alkaidlab.nvidia_dlssnr"] = "310-8";
  ASSERT_EQ(store->update(settings, store->query().etag).status, 200);
  auto use = store->acquire_selected(image_enhancement::backend_capability_e::nr);
  ASSERT_TRUE(use);
  EXPECT_TRUE(use->runtime_digest.empty());
  EXPECT_TRUE(store->status()["nr_selection_verified"]);
}

TEST_F(ImageEnhancementConfigTest, BothCapabilitySlotsCanHoldSelectionsAtOnce) {
  ASSERT_EQ(store->update(trusted_fixture(), store->query().etag).status, 200);
  create_nr_fixture();
  auto state = store->query();
  state.settings.selected_nr_backend = image_enhancement::NVIDIA_DLSSNR_BACKEND;
  state.settings.versions["alkaidlab.nvidia_dlssnr"] = "310-8";
  state.settings.runtime_pins["alkaidlab.nvidia_dlssnr"] = FIXTURE_DIGEST;
  ASSERT_EQ(store->update(state.settings, state.etag).status, 200);
  const auto hdr_use = store->acquire_selected(image_enhancement::backend_capability_e::hdr);
  const auto nr_use = store->acquire_selected(image_enhancement::backend_capability_e::nr);
  ASSERT_TRUE(hdr_use);
  ASSERT_TRUE(nr_use);
  EXPECT_EQ(hdr_use->id, image_enhancement::NVIDIA_RTX_VIDEO_BACKEND);
  EXPECT_EQ(nr_use->id, image_enhancement::NVIDIA_DLSSNR_BACKEND);
}

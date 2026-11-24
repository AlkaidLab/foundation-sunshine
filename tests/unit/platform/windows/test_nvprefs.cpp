/** @file test_nvprefs.cpp
 * Exercise real DRS transactions and recovery journals against a fake driver.
 * No NVIDIA driver settings on the test host are read or modified.
 */
#include "src/platform/windows/nvprefs/driver_settings.h"
#include "src/platform/windows/nvprefs/nvprefs_common.h"
#include "src/platform/windows/nvprefs/nvprefs_interface.h"
#include "src/platform/windows/nvprefs/undo_file.h"
#include <barrier>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <thread>
#include <utility>
#include <vector>

namespace {
  using settings_t = std::map<NvU32, NVDRS_SETTING>;
  settings_t persisted;
  settings_t defaults;
  nvprefs::nvprefs_options options;
  std::optional<nvprefs::nvprefs_options> next_options;
  int option_reads = 0;
  int initializes = 0;
  int saves = 0;
  int deletes = 0;
  bool available = true;
  bool fail_set = false;
  bool fail_application = false;
  bool fail_save = false;
  bool fail_verify = false;
  bool fail_next_load = false;
  struct fake_session_t {
    settings_t staged;
  };

  NVDRS_SETTING
  setting(NvU32 id, NvU32 value, bool predefined = false) {
    NVDRS_SETTING s {};
    s.version = NVDRS_SETTING_VER;
    s.settingId = id;
    s.settingType = NVDRS_DWORD_TYPE;
    s.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
    s.u32CurrentValue = value;
    s.isCurrentPredefined = predefined;
    return s;
  }
  fake_session_t &
  session(NvDRSSessionHandle handle) {
    return *reinterpret_cast<fake_session_t *>(handle);
  }
}  // namespace

namespace nvprefs {
  void
  info_message(const std::string &) {}
  void
  info_message(const std::wstring &) {}
  void
  error_message(const std::string &) {}
  void
  error_message(const std::wstring &) {}
  nvprefs_options
  get_nvprefs_options() {
    ++option_reads;
    const auto snapshot = options;
    // Model a settings save immediately after the transaction reads its policy.
    if (next_options) options = *std::exchange(next_options, std::nullopt);
    return snapshot;
  }
}  // namespace nvprefs

#undef NVAPI_INTERFACE
#define NVAPI_INTERFACE NvAPI_Status __cdecl
NVAPI_INTERFACE
NvAPI_Initialize() {
  ++initializes;
  return available ? NVAPI_OK : NVAPI_LIBRARY_NOT_FOUND;
}
NVAPI_INTERFACE
NvAPI_Unload() { return NVAPI_OK; }
NVAPI_INTERFACE
NvAPI_GetErrorMessage(NvAPI_Status, NvAPI_ShortString message) {
  message[0] = 0;
  return NVAPI_OK;
}
NVAPI_INTERFACE
NvAPI_DRS_CreateSession(NvDRSSessionHandle *handle) {
  *handle = reinterpret_cast<NvDRSSessionHandle>(new fake_session_t {});
  return NVAPI_OK;
}
NVAPI_INTERFACE
NvAPI_DRS_DestroySession(NvDRSSessionHandle handle) {
  delete &session(handle);
  return NVAPI_OK;
}
NVAPI_INTERFACE
NvAPI_DRS_LoadSettings(NvDRSSessionHandle handle) {
  if (std::exchange(fail_next_load, false)) return NVAPI_ERROR;
  session(handle).staged = persisted;
  return NVAPI_OK;
}
NVAPI_INTERFACE
NvAPI_DRS_SaveSettings(NvDRSSessionHandle handle) {
  ++saves;
  if (fail_save) return NVAPI_ERROR;
  persisted = session(handle).staged;
  if (std::exchange(fail_verify, false)) fail_next_load = true;
  return NVAPI_OK;
}
NVAPI_INTERFACE
NvAPI_DRS_GetBaseProfile(NvDRSSessionHandle, NvDRSProfileHandle *profile) {
  *profile = reinterpret_cast<NvDRSProfileHandle>(1);
  return NVAPI_OK;
}
NVAPI_INTERFACE
NvAPI_DRS_FindProfileByName(NvDRSSessionHandle, NvAPI_UnicodeString, NvDRSProfileHandle *profile) {
  if (fail_application) return NVAPI_ERROR;
  *profile = reinterpret_cast<NvDRSProfileHandle>(2);
  return NVAPI_OK;
}
NVAPI_INTERFACE
NvAPI_DRS_CreateProfile(NvDRSSessionHandle, NVDRS_PROFILE *, NvDRSProfileHandle *) { return NVAPI_ERROR; }
NVAPI_INTERFACE
NvAPI_DRS_GetApplicationInfo(NvDRSSessionHandle, NvDRSProfileHandle, NvAPI_UnicodeString, NVDRS_APPLICATION *) { return NVAPI_OK; }
NVAPI_INTERFACE
NvAPI_DRS_CreateApplication(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_APPLICATION *) { return NVAPI_OK; }
NVAPI_INTERFACE
NvAPI_DRS_SetSetting(NvDRSSessionHandle handle, NvDRSProfileHandle, NVDRS_SETTING *value) {
  if (fail_set) return NVAPI_ERROR;
  session(handle).staged[value->settingId] = *value;
  return NVAPI_OK;
}
NVAPI_INTERFACE
NvAPI_DRS_GetSetting(NvDRSSessionHandle handle, NvDRSProfileHandle, NvU32 id, NVDRS_SETTING *value) {
  if (auto it = session(handle).staged.find(id); it != session(handle).staged.end()) {
    *value = it->second;
    return NVAPI_OK;
  }
  if (auto it = defaults.find(id); it != defaults.end()) {
    *value = it->second;
    return NVAPI_OK;
  }
  return NVAPI_SETTING_NOT_FOUND;
}
NVAPI_INTERFACE
NvAPI_DRS_DeleteProfileSetting(NvDRSSessionHandle handle, NvDRSProfileHandle, NvU32 id) {
  ++deletes;
  session(handle).staged.erase(id);
  return NVAPI_OK;
}

namespace {
  class NvPrefs: public testing::Test {
  protected:
    std::filesystem::path root;
    std::wstring previous_program_data;
    std::unique_ptr<nvprefs::nvprefs_interface> prefs;
    std::filesystem::path
    journal() { return root / "Sunshine" / "nvprefs_undo.json"; }
    void
    SetUp() override {
      wchar_t buffer[32768] {};
      GetEnvironmentVariableW(L"ProgramData", buffer, 32768);
      previous_program_data = buffer;
      static unsigned sequence = 0;
      root = std::filesystem::temp_directory_path() /
             ("sunshine-nvprefs-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++));
      std::filesystem::create_directories(root);
      ASSERT_TRUE(SetEnvironmentVariableW(L"ProgramData", root.c_str()));
      options = {};
      options.vdd_force_vsync = true;
      options.opengl_vulkan_on_dxgi = false;
      options.sunshine_high_power_mode = false;
      next_options.reset();
      option_reads = 0;
      persisted = { { VSYNCMODE_ID, setting(VSYNCMODE_ID, VSYNCMODE_FORCEOFF) } };
      defaults.clear();
      initializes = saves = deletes = 0;
      available = true;
      fail_application = fail_set = fail_save = fail_verify = fail_next_load = false;
      prefs = std::make_unique<nvprefs::nvprefs_interface>();
    }
    void
    TearDown() override {
      available = true;
      fail_set = fail_save = fail_next_load = false;
      prefs.reset();
      SetEnvironmentVariableW(L"ProgramData", previous_program_data.empty() ? nullptr : previous_program_data.c_str());
      ASSERT_TRUE(std::filesystem::equivalent(root.parent_path(), std::filesystem::temp_directory_path()));
      std::filesystem::remove_all(root);
    }
    void
    write_journal() {
      nvprefs::undo_data_t undo;
      undo.set_vsync(VSYNCMODE_FORCEON, VSYNCMODE_FORCEOFF);
      std::filesystem::create_directories(journal().parent_path());
      std::ofstream(journal()) << undo.write();
      persisted[VSYNCMODE_ID] = setting(VSYNCMODE_ID, VSYNCMODE_FORCEON);
    }
  };

  TEST_F(NvPrefs, ConfigurationUsesOnePolicySnapshotAndNextCallSeesSavedChanges) {
    options.opengl_vulkan_on_dxgi = true;
    options.sunshine_high_power_mode = true;
    next_options = nvprefs::nvprefs_options { false, false, false, false };
    ASSERT_TRUE(prefs->configure());
    EXPECT_EQ(option_reads, 1);
    EXPECT_EQ(persisted.at(OGL_CPL_PREFER_DXPRESENT_ID).u32CurrentValue, OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED);
    EXPECT_EQ(persisted.at(PREFERRED_PSTATE_ID).u32CurrentValue, PREFERRED_PSTATE_PREFER_MAX);
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEOFF);

    EXPECT_FALSE(prefs->configure());
    EXPECT_EQ(option_reads, 2);
    EXPECT_FALSE(persisted.contains(OGL_CPL_PREFER_DXPRESENT_ID));
    EXPECT_FALSE(std::filesystem::exists(journal()));
  }

  TEST_F(NvPrefs, VsyncUsesOnePolicySnapshotAndReleaseNeedsNoPolicyRead) {
    options.opengl_vulkan_on_dxgi = true;
    next_options = nvprefs::nvprefs_options { false, false, false, false };
    auto lease = prefs->acquire_vsync();
    ASSERT_TRUE(lease);
    EXPECT_EQ(option_reads, 1);
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEON);
    EXPECT_EQ(persisted.at(OGL_CPL_PREFER_DXPRESENT_ID).u32CurrentValue, OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED);

    EXPECT_FALSE(prefs->acquire_vsync());
    EXPECT_EQ(option_reads, 2);
    lease.reset();
    EXPECT_EQ(option_reads, 2);
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEOFF);
    EXPECT_TRUE(std::filesystem::exists(journal()));
    ASSERT_TRUE(prefs->restore());
    EXPECT_EQ(option_reads, 3);
    EXPECT_FALSE(persisted.contains(OGL_CPL_PREFER_DXPRESENT_ID));
    EXPECT_FALSE(std::filesystem::exists(journal()));
  }

  TEST_F(NvPrefs, ApplicationFailureDoesNotSkipIndependentGlobalSettings) {
    fail_application = true;
    options.opengl_vulkan_on_dxgi = true;
    EXPECT_FALSE(prefs->configure());
    EXPECT_EQ(persisted.at(OGL_CPL_PREFER_DXPRESENT_ID).u32CurrentValue, OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED);
    EXPECT_TRUE(std::filesystem::exists(journal()));
  }

  TEST_F(NvPrefs, FailedJournalReplacementPreservesLastCommittedSnapshot) {
    std::filesystem::create_directories(journal().parent_path());
    auto file = nvprefs::undo_file_t::create_new_file(journal());
    ASSERT_TRUE(file);
    nvprefs::undo_data_t original;
    original.set_vsync(VSYNCMODE_FORCEON, VSYNCMODE_FORCEOFF);
    ASSERT_TRUE(file->write_undo_data(original));
    auto temporary_path = journal();
    temporary_path += L".tmp";
    nvprefs::safe_handle blocked(CreateFileW(temporary_path.c_str(), GENERIC_WRITE | DELETE, 0,
      nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    ASSERT_TRUE(blocked);
    nvprefs::undo_data_t replacement;
    replacement.set_vsync(VSYNCMODE_FORCEON, VSYNCMODE_PASSIVE);
    EXPECT_FALSE(file->write_undo_data(replacement));
    file.reset();
    bool denied = false;
    auto reopened = nvprefs::undo_file_t::open_existing_file(journal(), denied);
    ASSERT_TRUE(reopened);
    const auto recovered = reopened->read_undo_data();
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered->get_vsync()->undo_value, VSYNCMODE_FORCEOFF);
    EXPECT_TRUE(reopened->delete_file());
    blocked.reset();
  }

  TEST_F(NvPrefs, AtomicJournalReplacementPersistsCompleteNewSnapshot) {
    std::filesystem::create_directories(journal().parent_path());
    auto file = nvprefs::undo_file_t::create_new_file(journal());
    ASSERT_TRUE(file);
    nvprefs::undo_data_t undo;
    undo.set_opengl_swapchain(1, std::nullopt);
    ASSERT_TRUE(file->write_undo_data(undo));
    undo.set_vsync(VSYNCMODE_FORCEON, VSYNCMODE_FORCEOFF);
    ASSERT_TRUE(file->write_undo_data(undo));
    bool other_denied = false;
    EXPECT_FALSE(nvprefs::undo_file_t::open_existing_file(journal(), other_denied));
    EXPECT_TRUE(other_denied);
    file.reset();
    bool denied = false;
    auto reopened = nvprefs::undo_file_t::open_existing_file(journal(), denied);
    ASSERT_TRUE(reopened);
    const auto recovered = reopened->read_undo_data();
    ASSERT_TRUE(recovered);
    EXPECT_TRUE(recovered->get_opengl_swapchain());
    EXPECT_EQ(recovered->get_vsync()->undo_value, VSYNCMODE_FORCEOFF);
    EXPECT_TRUE(reopened->delete_file());
  }

  TEST_F(NvPrefs, DisabledPolicyDoesNotLoadNvapi) {
    options.vdd_force_vsync = false;
    EXPECT_FALSE(prefs->acquire_vsync());
    options.vdd_force_vsync = true;
    options.manage_driver_settings = false;
    EXPECT_FALSE(prefs->acquire_vsync());
    EXPECT_FALSE(prefs->configure());
    EXPECT_EQ(initializes, 0);
    EXPECT_EQ(saves, 0);
  }
  TEST_F(NvPrefs, StartupDoesNotForceVsyncWithoutSession) {
    EXPECT_TRUE(prefs->configure());
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEOFF);
  }
  TEST_F(NvPrefs, LastSessionRestoresOriginalOverride) {
    auto first = prefs->acquire_vsync();
    ASSERT_TRUE(first);
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEON);
    EXPECT_TRUE(std::filesystem::exists(journal()));
    const auto saved = saves;
    auto second = prefs->acquire_vsync();
    ASSERT_TRUE(second);
    EXPECT_EQ(saves, saved);
    first.reset();
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEON);
    second.reset();
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEOFF);
    EXPECT_FALSE(std::filesystem::exists(journal()));
  }
  TEST_F(NvPrefs, SharedLeaseTransfersFromHandshakeToStream) {
    auto handshake = prefs->acquire_vsync();
    auto stream = handshake;
    handshake.reset();
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEON);
    stream.reset();
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEOFF);
  }
  TEST_F(NvPrefs, LeaseRetainsTransactionStateUntilItsLastOwnerExits) {
    auto lease = prefs->acquire_vsync();
    ASSERT_TRUE(lease);
    prefs.reset();
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEON);
    lease.reset();
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEOFF);
    EXPECT_FALSE(std::filesystem::exists(journal()));
  }
  TEST_F(NvPrefs, InheritedValueIsRestoredByDeletingOverride) {
    persisted.clear();
    defaults[VSYNCMODE_ID] = setting(VSYNCMODE_ID, VSYNCMODE_PASSIVE, true);
    auto lease = prefs->acquire_vsync();
    ASSERT_TRUE(lease);
    lease.reset();
    EXPECT_FALSE(persisted.contains(VSYNCMODE_ID));
    EXPECT_EQ(deletes, 1);
    EXPECT_EQ(defaults.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_PASSIVE);
  }
  TEST_F(NvPrefs, ExistingForceOnNeedsNoWriteOrRestore) {
    persisted[VSYNCMODE_ID] = setting(VSYNCMODE_ID, VSYNCMODE_FORCEON);
    auto lease = prefs->acquire_vsync();
    ASSERT_TRUE(lease);
    lease.reset();
    EXPECT_EQ(saves, 0);
    EXPECT_FALSE(std::filesystem::exists(journal()));
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEON);
  }
  TEST_F(NvPrefs, UserChangesDuringStreamingArePreserved) {
    auto lease = prefs->acquire_vsync();
    ASSERT_TRUE(lease);
    persisted[VSYNCMODE_ID] = setting(VSYNCMODE_ID, VSYNCMODE_PASSIVE);
    lease.reset();
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_PASSIVE);
    EXPECT_FALSE(std::filesystem::exists(journal()));
  }
  TEST_F(NvPrefs, OptOutDoesNotPreventRestoringOwnedChange) {
    auto lease = prefs->acquire_vsync();
    ASSERT_TRUE(lease);
    options.manage_driver_settings = false;
    EXPECT_FALSE(prefs->configure());
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEON);
    lease.reset();
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEOFF);
  }
  TEST_F(NvPrefs, SwapchainUndoSurvivesVsyncSessionCleanup) {
    options.opengl_vulkan_on_dxgi = true;
    ASSERT_TRUE(prefs->configure());
    auto lease = prefs->acquire_vsync();
    ASSERT_TRUE(lease);
    lease.reset();
    EXPECT_TRUE(std::filesystem::exists(journal()));
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEOFF);
    EXPECT_EQ(persisted.at(OGL_CPL_PREFER_DXPRESENT_ID).u32CurrentValue, OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED);
    ASSERT_TRUE(prefs->restore());
    EXPECT_FALSE(persisted.contains(OGL_CPL_PREFER_DXPRESENT_ID));
  }
  TEST_F(NvPrefs, FailedSetDoesNotCommitOrReturnLease) {
    fail_set = true;
    EXPECT_FALSE(prefs->acquire_vsync());
    EXPECT_EQ(saves, 0);
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEOFF);
  }
  TEST_F(NvPrefs, FailedSaveRetainsRecoveryJournal) {
    fail_save = true;
    EXPECT_FALSE(prefs->acquire_vsync());
    EXPECT_TRUE(std::filesystem::exists(journal()));
    fail_save = false;
    EXPECT_TRUE(prefs->restore());
    EXPECT_FALSE(std::filesystem::exists(journal()));
  }
  TEST_F(NvPrefs, FailedReadbackDoesNotAdvertiseActiveAndRetainsUndo) {
    fail_verify = true;
    EXPECT_FALSE(prefs->acquire_vsync());
    EXPECT_EQ(prefs->status().state, "error");
    EXPECT_TRUE(std::filesystem::exists(journal()));
    EXPECT_TRUE(prefs->restore());
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEOFF);
  }
  TEST_F(NvPrefs, RestoreFailureKeepsJournalForRetry) {
    auto lease = prefs->acquire_vsync();
    ASSERT_TRUE(lease);
    fail_save = true;
    lease.reset();
    EXPECT_TRUE(std::filesystem::exists(journal()));
    fail_save = false;
    EXPECT_TRUE(prefs->restore());
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEOFF);
  }
  TEST_F(NvPrefs, AbandonedJournalIsRecoveredBeforeNextConfiguration) {
    write_journal();
    ASSERT_TRUE(prefs->configure());
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEOFF);
    EXPECT_FALSE(std::filesystem::exists(journal()));
  }
  TEST_F(NvPrefs, OptOutStartupRecoversAbandonedChangesWithoutNewWrites) {
    write_journal();
    options.manage_driver_settings = false;
    EXPECT_FALSE(prefs->configure());
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEOFF);
    EXPECT_FALSE(std::filesystem::exists(journal()));
  }
  TEST_F(NvPrefs, RecoveryFailureBlocksNewWritesAndRetainsJournal) {
    write_journal();
    fail_save = true;
    EXPECT_FALSE(prefs->configure());
    EXPECT_TRUE(std::filesystem::exists(journal()));
    EXPECT_FALSE(prefs->acquire_vsync());
    EXPECT_TRUE(std::filesystem::exists(journal()));
  }
  TEST_F(NvPrefs, UnsupportedDriverDoesNotProduceJournal) {
    available = false;
    EXPECT_FALSE(prefs->acquire_vsync());
    EXPECT_FALSE(std::filesystem::exists(journal()));
  }
  TEST_F(NvPrefs, ConcurrentSessionsShareOneDriverTransaction) {
    std::barrier acquired(9);
    std::barrier release(9);
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
      threads.emplace_back([&]() {
        auto lease = prefs->acquire_vsync();
        EXPECT_TRUE(lease);
        acquired.arrive_and_wait();
        release.arrive_and_wait();
      });
    }
    acquired.arrive_and_wait();
    EXPECT_EQ(saves, 1);
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEON);
    release.arrive_and_wait();
    for (auto &thread : threads) thread.join();
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEOFF);
  }
  TEST_F(NvPrefs, JournalCreationFailurePreventsDriverCommit) {
    std::ofstream(root / "Sunshine") << "not a directory";
    EXPECT_FALSE(prefs->acquire_vsync());
    EXPECT_EQ(saves, 0);
    EXPECT_EQ(persisted.at(VSYNCMODE_ID).u32CurrentValue, VSYNCMODE_FORCEOFF);
  }
  TEST_F(NvPrefs, OldJournalsRemainReadable) {
    const std::string old = R"({"opengl_swapchain":{"our_value":1,"undo_value":null}})";
    nvprefs::undo_data_t undo;
    ASSERT_TRUE(undo.read({ old.begin(), old.end() }));
    EXPECT_TRUE(undo.get_opengl_swapchain());
    EXPECT_FALSE(undo.get_vsync());
  }
  TEST_F(NvPrefs, CorruptJournalIsNotDeleted) {
    std::filesystem::create_directories(journal().parent_path());
    std::ofstream(journal()) << "truncated json";
    EXPECT_FALSE(prefs->configure());
    EXPECT_TRUE(std::filesystem::exists(journal()));
    EXPECT_EQ(saves, 0);
  }
}  // namespace

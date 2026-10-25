#ifdef _WIN32

  #include <gtest/gtest.h>

  #include <vector>

  #include "src/platform/windows/display_device/hdr_state_api.h"

namespace {

  namespace hdr = display_device::hdr_state_api;

  class HdrStateApiTest: public testing::Test {
  protected:
    DISPLAYCONFIG_PATH_TARGET_INFO target {};
    LONG modern_status = ERROR_SUCCESS;
    LONG legacy_status = ERROR_SUCCESS;
    LONG write_status = ERROR_SUCCESS;
    UINT32 modern_flags = 1u << 4;
    UINT32 active_mode = 0;
    std::vector<int> reads;
    std::vector<int> writes;
    bool written_enabled = false;

    void
    SetUp() override {
      target.adapterId = { 42, 7 };
      target.id = 9;
    }

    LONG
    get(DISPLAYCONFIG_DEVICE_INFO_HEADER *header) {
      EXPECT_EQ(header->adapterId.LowPart, target.adapterId.LowPart);
      EXPECT_EQ(header->adapterId.HighPart, target.adapterId.HighPart);
      EXPECT_EQ(header->id, target.id);
      reads.push_back(static_cast<int>(header->type));
      if (header->type == static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15)) {
        EXPECT_EQ(header->size, sizeof(hdr::advanced_color_info_2_t));
        auto &info = *reinterpret_cast<hdr::advanced_color_info_2_t *>(header);
        info.flags = modern_flags;
        info.active_color_mode = active_mode;
        return modern_status;
      }
      EXPECT_EQ(header->type, DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO);
      EXPECT_EQ(header->size, sizeof(DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO));
      auto &info = *reinterpret_cast<DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO *>(header);
      info.advancedColorSupported = 1;
      info.advancedColorEnabled = 1;
      return legacy_status;
    }

    LONG
    put(DISPLAYCONFIG_DEVICE_INFO_HEADER *header) {
      EXPECT_EQ(header->adapterId.LowPart, target.adapterId.LowPart);
      EXPECT_EQ(header->adapterId.HighPart, target.adapterId.HighPart);
      EXPECT_EQ(header->id, target.id);
      writes.push_back(static_cast<int>(header->type));
      if (header->type == static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(16)) {
        EXPECT_EQ(header->size, sizeof(hdr::hdr_state_packet_t));
        written_enabled = reinterpret_cast<hdr::hdr_state_packet_t *>(header)->enable_hdr != 0;
      }
      else {
        EXPECT_EQ(header->type, DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE);
        written_enabled = reinterpret_cast<DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE *>(header)->enableAdvancedColor != 0;
      }
      return write_status;
    }

    hdr::state_t
    query() {
      return hdr::query(target, [this](auto *header) { return get(header); });
    }

    LONG
    set(bool enabled) {
      return hdr::set(target, enabled, [this](auto *header) { return get(header); }, [this](auto *header) { return put(header); });
    }
  };

  TEST_F(HdrStateApiTest, AcmWithoutHdrIsNotSavedAsHdr) {
    modern_flags |= (1u << 1) | (1u << 6) | (1u << 7);
    active_mode = 1;  // WCG, with advanced color active and ACM enabled.
    const auto state = query();
    ASSERT_EQ(state.status, ERROR_SUCCESS);
    EXPECT_TRUE(state.supported);
    EXPECT_FALSE(state.enabled);
    EXPECT_EQ(reads, std::vector<int>({ 15 }));
    // Applying/restoring HDR-off never writes the advanced-color/WCG switch.
    EXPECT_EQ(set(state.enabled), ERROR_SUCCESS);
    EXPECT_EQ(writes, std::vector<int>({ 16 }));
    EXPECT_FALSE(written_enabled);
  }

  TEST_F(HdrStateApiTest, PreservesHdrUserSwitchEvenWhenHdrIsTemporarilyInactive) {
    modern_flags |= (1u << 3) | (1u << 5);  // Policy-limited, user HDR on.
    active_mode = 0;
    EXPECT_TRUE(query().enabled);
    EXPECT_EQ(set(true), ERROR_SUCCESS);
    EXPECT_TRUE(written_enabled);
    EXPECT_EQ(writes, std::vector<int>({ 16 }));
  }

  TEST_F(HdrStateApiTest, HdrAndAcmCanBothBeEnabled) {
    modern_flags |= (1u << 1) | (1u << 5) | (1u << 6) | (1u << 7);
    active_mode = 2;
    const auto state = query();
    EXPECT_TRUE(state.supported);
    EXPECT_TRUE(state.enabled);
    EXPECT_EQ(set(state.enabled), ERROR_SUCCESS);
    EXPECT_EQ(writes, std::vector<int>({ 16 }));
    EXPECT_TRUE(written_enabled);
  }

  TEST_F(HdrStateApiTest, WcgOnlyDisplayDoesNotFallBackOrWriteHdr) {
    modern_flags = (1u << 0) | (1u << 1) | (1u << 6) | (1u << 7);
    EXPECT_FALSE(query().supported);
    EXPECT_EQ(set(true), ERROR_NOT_SUPPORTED);
    EXPECT_EQ(reads, std::vector<int>({ 15, 15 }));
    EXPECT_TRUE(writes.empty());
  }

  TEST_F(HdrStateApiTest, OlderSystemsRetainLegacyQueryAndSet) {
    for (const LONG status : { ERROR_INVALID_PARAMETER, ERROR_NOT_SUPPORTED }) {
      modern_status = status;
      reads.clear();
      writes.clear();
      EXPECT_TRUE(query().enabled);
      EXPECT_EQ(set(false), ERROR_SUCCESS);
      EXPECT_FALSE(written_enabled);
      EXPECT_EQ(reads, std::vector<int>({ 15, 9, 15, 9 }));
      EXPECT_EQ(writes, std::vector<int>({ 10 }));
    }
  }

  TEST_F(HdrStateApiTest, QueryFailuresAreNotMaskedByLegacyFallback) {
    for (const LONG status : { ERROR_ACCESS_DENIED, ERROR_GEN_FAILURE, ERROR_INSUFFICIENT_BUFFER }) {
      modern_status = status;
      reads.clear();
      EXPECT_EQ(query().status, status);
      EXPECT_EQ(set(true), status);
      EXPECT_EQ(reads, std::vector<int>({ 15, 15 }));
      EXPECT_TRUE(writes.empty());
    }
    modern_status = ERROR_INVALID_PARAMETER;
    legacy_status = ERROR_GEN_FAILURE;
    EXPECT_EQ(query().status, ERROR_GEN_FAILURE);
    EXPECT_EQ(set(false), ERROR_GEN_FAILURE);
    EXPECT_TRUE(writes.empty());
  }

  TEST_F(HdrStateApiTest, FailedHdrWritesNeverFallBackToAdvancedColor) {
    for (const LONG status : { ERROR_NOT_SUPPORTED, ERROR_INVALID_PARAMETER, ERROR_ACCESS_DENIED }) {
      write_status = status;
      writes.clear();
      EXPECT_EQ(set(true), status);
      EXPECT_EQ(writes, std::vector<int>({ 16 }));
    }
  }

}  // namespace

#endif

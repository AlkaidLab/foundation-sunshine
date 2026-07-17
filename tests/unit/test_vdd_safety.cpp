#include "../tests_common.h"

#ifdef _WIN32

#include "src/display_device/vdd_control_ioctl.h"
#include "src/display_device/vdd_ioctl.h"
#include "src/platform/windows/display_device/settings_topology.h"
#include "src/platform/windows/vdd_frame_channel.h"

namespace {

  platf::dxgi::vdd_frame_channel::shared_frame_metadata_t
  valid_vdd_metadata() {
    platf::dxgi::vdd_frame_channel::shared_frame_metadata_t meta {};
    meta.Magic = platf::dxgi::vdd_frame_channel::meta_magic;
    meta.Version = platf::dxgi::vdd_frame_channel::meta_version;
    meta.Width = 3840;
    meta.Height = 2160;
    meta.DxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    meta.IsHdr = 1;
    meta.MetadataSize = sizeof(meta);
    meta.SlotCount = 4;
    meta.SlotIndex = 2;
    meta.AdapterLuidLowPart = 0x12345678;
    meta.AdapterLuidHighPart = 0x1234;
    meta.ProducerQpcFrequency = 10000000;
    return meta;
  }

  LUID
  valid_vdd_luid() {
    LUID luid {};
    luid.LowPart = 0x12345678;
    luid.HighPart = 0x1234;
    return luid;
  }

  D3D11_TEXTURE2D_DESC
  valid_vdd_texture_desc(const platf::dxgi::vdd_frame_channel::shared_frame_metadata_t &meta) {
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = meta.Width;
    desc.Height = meta.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = static_cast<DXGI_FORMAT>(meta.DxgiFormat);
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    return desc;
  }

}  // namespace

TEST(VddBaselineSafety, DetectsVddOnlyTopology) {
  using namespace display_device;

  EXPECT_FALSE(is_vdd_only_topology({}, "vdd"));
  EXPECT_FALSE(is_vdd_only_topology({ { "vdd" } }, ""));
  EXPECT_FALSE(is_vdd_only_topology({ { "physical" } }, "vdd"));
  EXPECT_FALSE(is_vdd_only_topology({ { "vdd" }, { "physical" } }, "vdd"));
  EXPECT_TRUE(is_vdd_only_topology({ { "vdd" } }, "vdd"));
}

TEST(VddFrameChannelSafety, AcceptsValidMetadataForAttach) {
  using namespace platf::dxgi::vdd_frame_channel;

  auto meta = valid_vdd_metadata();
  auto result = validate_metadata_for_attach(meta, valid_vdd_luid());

  EXPECT_TRUE(result);
  EXPECT_EQ(result.reason, reject_reason::none);
}

TEST(VddFrameChannelSafety, RejectsUnsafeAdapterLuid) {
  using namespace platf::dxgi::vdd_frame_channel;

  auto meta = valid_vdd_metadata();
  meta.AdapterLuidLowPart = 0;
  meta.AdapterLuidHighPart = 0;
  auto result = validate_metadata_for_attach(meta, valid_vdd_luid());
  EXPECT_FALSE(result);
  EXPECT_EQ(result.reason, reject_reason::zero_adapter_luid);

  meta = valid_vdd_metadata();
  LUID wrong_luid {};
  wrong_luid.LowPart = 0x87654321;
  wrong_luid.HighPart = 0x4321;
  result = validate_metadata_for_attach(meta, wrong_luid);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.reason, reject_reason::adapter_luid_mismatch);
}

TEST(VddFrameChannelSafety, RejectsInvalidProbeMetadata) {
  using namespace platf::dxgi::vdd_frame_channel;

  auto meta = valid_vdd_metadata();
  meta.MetadataSize = min_metadata_size - 1;
  auto result = validate_metadata_for_probe(meta);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.reason, reject_reason::metadata_too_small);

  meta = valid_vdd_metadata();
  meta.DxgiFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
  result = validate_metadata_for_probe(meta);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.reason, reject_reason::unsupported_format);

  meta = valid_vdd_metadata();
  meta.SlotCount = max_shared_slots + 1;
  result = validate_metadata_for_probe(meta);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.reason, reject_reason::invalid_slot_count);

  meta = valid_vdd_metadata();
  meta.SlotIndex = meta.SlotCount;
  result = validate_metadata_for_probe(meta);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.reason, reject_reason::slot_index_out_of_range);
}

TEST(VddFrameChannelSafety, ValidatesTextureDescContract) {
  using namespace platf::dxgi::vdd_frame_channel;

  auto meta = valid_vdd_metadata();
  auto desc = valid_vdd_texture_desc(meta);
  auto result = validate_texture_desc(desc, meta);
  EXPECT_TRUE(result);
  EXPECT_EQ(result.reason, reject_reason::none);

  desc = valid_vdd_texture_desc(meta);
  desc.Width += 1;
  result = validate_texture_desc(desc, meta);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.reason, reject_reason::texture_desc_mismatch);

  desc = valid_vdd_texture_desc(meta);
  desc.MiscFlags = 0;
  result = validate_texture_desc(desc, meta);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.reason, reject_reason::texture_missing_keyed_mutex);

  desc = valid_vdd_texture_desc(meta);
  desc.BindFlags = 0;
  result = validate_texture_desc(desc, meta);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.reason, reject_reason::texture_missing_shader_resource);
}

TEST(VddFrameChannelSafety, BoundsPostEventAcquireWait) {
  using namespace platf::dxgi::vdd_frame_channel;

  EXPECT_EQ(bounded_consumer_acquire_timeout_ms(0), 0);
  EXPECT_EQ(bounded_consumer_acquire_timeout_ms(1), 1);
  EXPECT_EQ(bounded_consumer_acquire_timeout_ms(consumer_acquire_wait_budget_ms),
            consumer_acquire_wait_budget_ms);
  EXPECT_EQ(bounded_consumer_acquire_timeout_ms(16),
            consumer_acquire_wait_budget_ms);
  EXPECT_EQ(bounded_consumer_acquire_timeout_ms(1000),
            consumer_acquire_wait_budget_ms);
}

TEST(VddFrameChannelSafety, ReadsOnlyStableMetadataSnapshots) {
  using namespace platf::dxgi::vdd_frame_channel;

  auto meta = valid_vdd_metadata();
  meta.MetadataSequence = (static_cast<UINT32>(7) << 16) | 2u;

  shared_frame_metadata_t snapshot {};
  ASSERT_TRUE(read_stable_metadata(&meta, snapshot, 1));
  EXPECT_EQ(snapshot.FrameCounter, meta.FrameCounter);
  EXPECT_EQ(snapshot.MetadataSequence, meta.MetadataSequence);
  EXPECT_TRUE(metadata_sequence_is_stable(snapshot.MetadataSequence));
  EXPECT_EQ(metadata_sequence_counter(snapshot.MetadataSequence), 2u);
  EXPECT_EQ(metadata_channel_generation(snapshot.MetadataSequence), 7u);

  meta.MetadataSequence = (static_cast<UINT32>(8) << 16) | 3u;
  EXPECT_FALSE(metadata_sequence_is_stable(meta.MetadataSequence));
  EXPECT_FALSE(read_stable_metadata(&meta, snapshot, 1));
  EXPECT_EQ(metadata_sequence_counter(meta.MetadataSequence), 3u);
  EXPECT_EQ(metadata_channel_generation(meta.MetadataSequence), 8u);
}

TEST(VddFrameChannelSafety, ParsesFrameChannelModes) {
  using namespace platf::dxgi::vdd_frame_channel;

  auto mode = parse_channel_mode("auto");
  ASSERT_TRUE(mode);
  EXPECT_EQ(*mode, channel_mode::auto_probe);
  EXPECT_STREQ(channel_mode_name(*mode), "auto");

  mode = parse_channel_mode("legacy");
  ASSERT_TRUE(mode);
  EXPECT_EQ(*mode, channel_mode::legacy_named);
  EXPECT_STREQ(channel_mode_name(*mode), "legacy_named");

  mode = parse_channel_mode("legacy_named");
  ASSERT_TRUE(mode);
  EXPECT_EQ(*mode, channel_mode::legacy_named);

  mode = parse_channel_mode("sealed");
  ASSERT_TRUE(mode);
  EXPECT_EQ(*mode, channel_mode::sealed_required);
  EXPECT_STREQ(channel_mode_name(*mode), "sealed_required");

  EXPECT_FALSE(parse_channel_mode("host_owned"));
  EXPECT_FALSE(parse_channel_mode(""));
}

TEST(VddFrameChannelSafety, NamesFrameChannelOpenStatuses) {
  using namespace display_device::vdd_ioctl;

  EXPECT_STREQ(frame_channel_open_status_name(frame_channel_open_status::opened), "opened");
  EXPECT_STREQ(frame_channel_open_status_name(frame_channel_open_status::not_ready), "not_ready");
  EXPECT_STREQ(frame_channel_open_status_name(frame_channel_open_status::unsupported), "unsupported");
  EXPECT_STREQ(frame_channel_open_status_name(frame_channel_open_status::interface_missing), "interface_missing");
  EXPECT_STREQ(frame_channel_open_status_name(frame_channel_open_status::failed), "failed");
}

TEST(VddFrameChannelSafety, DefinesSealedChannelCapsAbi) {
  VDD_FRAME_CHANNEL_CAPS caps {};
  caps.Size = sizeof(caps);
  caps.Version = VDD_FRAME_CHANNEL_CAPS_VERSION;
  caps.Flags = VDD_FRAME_CHANNEL_FLAG_SEALED_BORROW |
               VDD_FRAME_CHANNEL_FLAG_UNNAMED_HANDLES |
               VDD_FRAME_CHANNEL_FLAG_STRICT_DACL;

  EXPECT_EQ(caps.Size, sizeof(VDD_FRAME_CHANNEL_CAPS));
  EXPECT_EQ(caps.Version, 1u);
  EXPECT_NE(caps.Flags & VDD_FRAME_CHANNEL_FLAG_SEALED_BORROW, 0u);
  EXPECT_NE(caps.Flags & VDD_FRAME_CHANNEL_FLAG_UNNAMED_HANDLES, 0u);
  EXPECT_NE(caps.Flags & VDD_FRAME_CHANNEL_FLAG_STRICT_DACL, 0u);
  EXPECT_NE(IOCTL_VDD_QUERY_FRAME_CHANNEL_CAPS, IOCTL_VDD_COMMAND);
  EXPECT_NE(IOCTL_VDD_QUERY_FRAME_CHANNEL_CAPS, IOCTL_VDD_PING);
}

TEST(VddFrameChannelSafety, DefinesSealedChannelOpenAbi) {
  VDD_FRAME_CHANNEL_OPEN_REQUEST request {};
  request.Size = sizeof(request);
  request.Version = VDD_FRAME_CHANNEL_OPEN_VERSION;
  request.MonitorIndex = 2;
  request.RequiredFlags = VDD_FRAME_CHANNEL_FLAG_SEALED_BORROW |
                          VDD_FRAME_CHANNEL_FLAG_UNNAMED_HANDLES |
                          VDD_FRAME_CHANNEL_FLAG_STRICT_DACL;
  request.TargetProcessId = 1234;
  request.DesiredSlots = VDD_FRAME_CHANNEL_MAX_SLOTS;
  request.AdapterLuidLowPart = 0x12345678;
  request.AdapterLuidHighPart = 0x1234;

  VDD_FRAME_CHANNEL_OPEN_RESPONSE response {};
  response.Size = sizeof(response);
  response.Version = VDD_FRAME_CHANNEL_OPEN_VERSION;
  response.Flags = request.RequiredFlags;
  response.SlotCount = VDD_FRAME_CHANNEL_MAX_SLOTS;
  response.MetadataSize = sizeof(platf::dxgi::vdd_frame_channel::shared_frame_metadata_t);
  response.MetadataHandle = 0x1000;
  response.FrameReadyEventHandle = 0x2000;
  response.Slots[0].TextureHandle = 0x3000;

  EXPECT_EQ(request.Size, sizeof(VDD_FRAME_CHANNEL_OPEN_REQUEST));
  EXPECT_EQ(response.Size, sizeof(VDD_FRAME_CHANNEL_OPEN_RESPONSE));
  EXPECT_EQ(request.Version, 1u);
  EXPECT_EQ(response.Version, 1u);
  EXPECT_EQ(response.SlotCount, VDD_FRAME_CHANNEL_MAX_SLOTS);
  EXPECT_NE(response.MetadataHandle, 0u);
  EXPECT_NE(response.FrameReadyEventHandle, 0u);
  EXPECT_NE(response.Slots[0].TextureHandle, 0u);
  EXPECT_NE(IOCTL_VDD_OPEN_FRAME_CHANNEL, IOCTL_VDD_COMMAND);
  EXPECT_NE(IOCTL_VDD_OPEN_FRAME_CHANNEL, IOCTL_VDD_PING);
  EXPECT_NE(IOCTL_VDD_OPEN_FRAME_CHANNEL, IOCTL_VDD_QUERY_FRAME_CHANNEL_CAPS);
}

#endif

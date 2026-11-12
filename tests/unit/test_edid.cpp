/**
 * @file tests/unit/test_edid.cpp
 * @brief Test the Linux EDID name reader.
 *
 * The parser feeds the display friendly name shown by the WebUI and used by
 * friendly-name lookups, so it is checked against generated EDIDs (the same
 * bytes the virtual display advertises) and against the ways real EDIDs differ:
 * a missing name descriptor, padding, terminators and malformed blobs.
 */
#include <src/platform/linux/edid.h>
#include <src/platform/linux/vdd_edid.h>

#include "../tests_common.h"

#ifndef _WIN32
namespace {
  /// A base block with the given name in its first display descriptor slot.
  std::vector<std::uint8_t>
  edid_with_name(const std::string &name, bool use_second_slot = false) {
    std::vector<std::uint8_t> blob(edid::base_block_bytes, 0x00);
    const std::uint8_t header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    std::copy(std::begin(header), std::end(header), blob.begin());

    // Slots 0 and 2 become dummy descriptors so only the requested one matches.
    for (std::size_t slot = 0; slot < 4; ++slot) {
      const auto offset = edid::descriptor_offset + slot * edid::descriptor_size;
      blob[offset] = 0x00;
      blob[offset + 1] = 0x00;
      blob[offset + 2] = 0x00;
      blob[offset + 3] = 0x10;  // dummy descriptor tag
      blob[offset + 4] = 0x00;
    }

    const auto offset = edid::descriptor_offset +
                        (use_second_slot ? edid::descriptor_size : 0);
    blob[offset] = 0x00;
    blob[offset + 1] = 0x00;
    blob[offset + 2] = 0x00;
    blob[offset + 3] = edid::tag_display_product_name;
    blob[offset + 4] = 0x00;
    for (std::size_t i = 0; i < 13; ++i) {
      blob[offset + 5 + i] = i < name.size() ? static_cast<std::uint8_t>(name[i]) : 0x20;
    }
    blob[offset + 5 + std::min<std::size_t>(name.size(), 12)] = 0x0A;  // terminator
    return blob;
  }
}  // namespace

TEST(Edid, ParsesTheGeneratedVirtualDisplayName) {
  // Round trip against the generator: the name the EDID advertises must come
  // back exactly, which is what makes 'Zako HDR' resolvable by friendly name.
  const auto blob = vdd_edid::generate_virtual_display_edid(1920, 1080, 60, true, ZAKO_NAME);
  const auto name = edid::monitor_name(blob);
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(*name, ZAKO_NAME);
}

TEST(Edid, ParsesNameFromAnyDescriptorSlot) {
  EXPECT_EQ(edid::monitor_name(edid_with_name("DELL U2720Q")), std::optional<std::string> { "DELL U2720Q" });
  EXPECT_EQ(edid::monitor_name(edid_with_name("BOE 0x098E", true)), std::optional<std::string> { "BOE 0x098E" });
}

TEST(Edid, TrimsTerminatorAndPadding) {
  const auto blob = edid_with_name("Panel");
  const auto name = edid::monitor_name(blob);
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(*name, "Panel") << "trailing spaces past the terminator are not part of the name";
}

TEST(Edid, MissingNameDescriptorYieldsNothing) {
  std::vector<std::uint8_t> blob(edid::base_block_bytes, 0x00);
  const std::uint8_t header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
  std::copy(std::begin(header), std::end(header), blob.begin());
  for (std::size_t slot = 0; slot < 4; ++slot) {
    const auto offset = edid::descriptor_offset + slot * edid::descriptor_size;
    blob[offset + 3] = 0x10;  // dummy descriptors only
  }

  EXPECT_FALSE(edid::monitor_name(blob).has_value())
    << "a panel without a name descriptor must fall back to the connector id";
}

TEST(Edid, RejectsMalformedBlobs) {
  const std::vector<std::uint8_t> empty;
  EXPECT_FALSE(edid::monitor_name(empty).has_value());

  std::vector<std::uint8_t> short_blob(64, 0x00);
  EXPECT_FALSE(edid::monitor_name(short_blob).has_value());

  auto bad_header = edid_with_name("Panel");
  bad_header[0] = 0x12;
  EXPECT_FALSE(edid::monitor_name(bad_header).has_value());
}
#endif  // !_WIN32

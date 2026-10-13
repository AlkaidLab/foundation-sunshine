/**
 * @file tests/unit/test_clipboard_wire.cpp
 * @brief Test the clipboard wire codec against the agent's frame contract.
 *
 * The Rust GUI agent (sunshine-control-panel, src-tauri/src/clipboard.rs) is the
 * reference implementation: `version, kind, token (LE32), length (LE32),
 * payload`, with the same rejections for short frames, unknown versions, unknown
 * kinds and truncated payloads, and a JSON descriptor for out-of-band blobs.
 */
#include <src/clipboard_wire.h>

#include "../tests_common.h"

#include <string>
#include <vector>

namespace {
  using clipboard_wire::encode;
  using clipboard_wire::encode_ref;
  using clipboard_wire::encode_text;
  using clipboard_wire::parse_header;
  using clipboard_wire::parse_ref_descriptor;

  clipboard_wire::payload_t
  bytes_of(const std::string &text) {
    return { text.begin(), text.end() };
  }
}  // namespace

TEST(ClipboardWire, TextFrameLayoutAndRoundTrip) {
  const auto frame = encode_text("hello");
  ASSERT_EQ(frame.size(), clipboard_bridge::kFrameHeaderBytes + 5u);
  EXPECT_EQ(frame[0], clipboard_bridge::kWireVersion);
  EXPECT_EQ(frame[1], clipboard_bridge::kKindText);
  EXPECT_EQ(frame[2], 0);  // single-flavor changes stay token 0
  EXPECT_EQ(frame[3], 0);
  EXPECT_EQ(frame[4], 0);
  EXPECT_EQ(frame[5], 0);
  EXPECT_EQ(frame[6], 5);  // length, little endian
  EXPECT_EQ(frame[7], 0);

  const auto header = parse_header(frame);
  ASSERT_TRUE(header.has_value());
  EXPECT_EQ(header->kind, clipboard_bridge::kKindText);
  EXPECT_EQ(header->token, 0u);
  EXPECT_EQ(header->length, 5u);
  EXPECT_EQ(std::string(clipboard_wire::payload_of(frame, *header).begin(),
              clipboard_wire::payload_of(frame, *header).end()),
    "hello");
}

TEST(ClipboardWire, NonZeroTokenIsPreserved) {
  const auto payload = bytes_of("x");
  const auto frame = encode(clipboard_bridge::kKindText, 0x01020304, payload);
  const auto header = parse_header(frame);
  ASSERT_TRUE(header.has_value());
  EXPECT_EQ(header->token, 0x01020304u);
}

TEST(ClipboardWire, HeaderRejectsMalformedFrames) {
  const auto good = encode_text("abc");

  // Too short for a header.
  EXPECT_FALSE(parse_header(std::span<const std::uint8_t> { good.data(), clipboard_bridge::kFrameHeaderBytes - 1 }).has_value());

  // Unknown wire version.
  auto wrong_version = good;
  wrong_version[0] = clipboard_bridge::kWireVersion + 1;
  EXPECT_FALSE(parse_header(wrong_version).has_value());

  // Unknown kind (the agent's Kind::from_byte returns None).
  auto wrong_kind = good;
  wrong_kind[1] = 0x7F;
  EXPECT_FALSE(parse_header(wrong_kind).has_value());

  // Declared length exceeding the buffer.
  auto truncated = good;
  truncated.resize(clipboard_bridge::kFrameHeaderBytes + 2);
  EXPECT_FALSE(parse_header(truncated).has_value());

  // Trailing bytes beyond the declared payload are fine (the agent ignores them).
  auto padded = good;
  padded.push_back(0xAA);
  const auto header = parse_header(padded);
  ASSERT_TRUE(header.has_value());
  EXPECT_EQ(header->length, 3u);
}

TEST(ClipboardWire, RefDescriptorRoundTrip) {
  const auto frame = encode_ref("2f1c4d6e-0000-4000-8000-abcdefabcdef", clipboard_bridge::kMimeText, 123456);
  const auto header = parse_header(frame);
  ASSERT_TRUE(header.has_value());
  EXPECT_EQ(header->kind, clipboard_bridge::kKindRef);

  const auto payload = clipboard_wire::payload_of(frame, *header);
  const auto descriptor = parse_ref_descriptor(
    std::string_view { reinterpret_cast<const char *>(payload.data()), payload.size() });
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->id, "2f1c4d6e-0000-4000-8000-abcdefabcdef");
  EXPECT_EQ(descriptor->mime, clipboard_bridge::kMimeText);
  EXPECT_EQ(descriptor->size, 123456u);
}

TEST(ClipboardWire, RefDescriptorRejections) {
  EXPECT_FALSE(parse_ref_descriptor("{not json").has_value());
  EXPECT_FALSE(parse_ref_descriptor("[1,2,3]").has_value());
  EXPECT_FALSE(parse_ref_descriptor(R"({"mime":"text/plain"})").has_value()) << "an id is mandatory";
  EXPECT_FALSE(parse_ref_descriptor(R"({"id":42})").has_value()) << "the id must be a string";
  EXPECT_FALSE(parse_ref_descriptor(R"({"id":""})").has_value());

  const std::string oversized(clipboard_bridge::kMaxRefIdLength + 1, 'a');
  EXPECT_FALSE(parse_ref_descriptor(R"({"id":")" + oversized + R"("})").has_value());

  // Missing mime/size is tolerated: the provider checks the mime itself.
  const auto partial = parse_ref_descriptor(R"({"id":"abc"})");
  ASSERT_TRUE(partial.has_value());
  EXPECT_TRUE(partial->mime.empty());
  EXPECT_EQ(partial->size, 0u);
}

TEST(ClipboardWire, TextMimeDetection) {
  EXPECT_TRUE(clipboard_wire::is_text_mime(clipboard_bridge::kMimeText));
  EXPECT_TRUE(clipboard_wire::is_text_mime("text/html"));
  EXPECT_FALSE(clipboard_wire::is_text_mime(clipboard_bridge::kMimePng));
  EXPECT_FALSE(clipboard_wire::is_text_mime(""));
}

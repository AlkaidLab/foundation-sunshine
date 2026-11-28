#include <gtest/gtest.h>

#include "src/telemetry/error_code.h"
#include "src/telemetry/log_template.h"

TEST(ErrorCode, ApostropheInsideAWordIsPartOfTheWord) {
  EXPECT_EQ(telemetry::error_head("Couldn't read packet: end of file"), "couldn't read packet");
  EXPECT_EQ(telemetry::error_head("Couldn't accept incoming connections: aborted"), "couldn't accept incoming connections");
  EXPECT_NE(telemetry::error_code_of("Couldn't read packet: x"), telemetry::error_code_of("Couldn't accept incoming connections: x"));
  // an apostrophe that opens a value still ends the head
  EXPECT_EQ(telemetry::error_head("Client 'Alice' refused"), "client");
}

TEST(ErrorCode, AComponentPrefixIsKeptSoItsMessagesStayApart) {
  EXPECT_EQ(telemetry::error_head("NvEnc: NvEncOpenEncodeSessionEx() failed: 8"), "nvenc nvencopenencodesessionex");
  EXPECT_NE(telemetry::error_code_of("NvEnc: NvEncOpenEncodeSessionEx() failed: 8"), telemetry::error_code_of("NvEnc: encoding format is not supported by the gpu, NVENCAPI_VERSION: 13"));
  EXPECT_EQ(telemetry::error_head("Audio capture: failed to initialize microphone"), "audio capture failed to initialize microphone");
  // a longer first part is a message, not a prefix: the colon ends the head; what follows (a reason from the system, often translated) is not part of the code
  EXPECT_EQ(telemetry::error_head("Couldn't accept incoming connections: x: y"), "couldn't accept incoming connections");
  EXPECT_EQ(telemetry::error_head("Failed to apply the profile: access denied"), "failed to apply the profile");
}

TEST(ErrorCode, ALeadingTagIsPartOfTheHeadAndNumbersAreNot) {
  EXPECT_EQ(telemetry::error_head("[Display Init] Configured adapter [0x1] differs"), "display init configured adapter");
  EXPECT_EQ(telemetry::error_head("[vdd_capture] failed to query IDXGIDevice for adapter LUID: 0x80070005"), "vdd_capture failed to query idxgidevice for adapter luid");
  EXPECT_NE(telemetry::error_code_of("[A b] one"), telemetry::error_code_of("[A b] two"));
  // a tag with digits or symbols is data, not a tag
  EXPECT_EQ(telemetry::error_head("[0x7ff] oops"), "");
  EXPECT_EQ(telemetry::error_head("[user@example] oops"), "");
}

TEST(ErrorCode, TheSameMessageWithOtherValuesIsTheSameCode) {
  EXPECT_EQ(telemetry::error_code_of("Download failed or rejected [https://a/b.jpg, code:23] [src/x.cpp:838:f]"), telemetry::error_code_of("Download failed or rejected [https://c/d.png, code:35] [src/x.cpp:838:f]"));
  EXPECT_EQ(telemetry::error_code_of("Failed to create display for: <name>"), telemetry::error_code_of("Failed to create display for: "));
  EXPECT_EQ(telemetry::error_code_of("x").size(), 9u);
}

TEST(ErrorCode, ALongMessageWithMultiByteCharactersIsCutAtBytesNotCharacters) {
  // 1 + 40 * 3 bytes, no space: the head is cut at 96 bytes, in the middle of a character
  std::string no_space = "x";
  for (int i = 0; i < 40; ++i) no_space += "错";
  const auto head = telemetry::error_head(no_space);
  EXPECT_EQ(head.size(), 96u);
  // spaces: cut at 96 bytes, then back to the last space
  std::string spaced = "加载失败";
  for (int i = 0; i < 12; ++i) spaced += " 加载失败";
  const auto spaced_head = telemetry::error_head(spaced);
  EXPECT_LE(spaced_head.size(), 96u);
  EXPECT_EQ(spaced_head.back() & 0x80, 0x80);  // it ends on the last whole word, not on a space
  // the codes are pinned: scripts/telemetry/test_error_code_catalogue.py holds the same two values (the dictionary tool must agree with this function byte for byte)
  EXPECT_EQ(telemetry::error_code_of(no_space), "ec2d441a4");
  EXPECT_EQ(telemetry::error_code_of(spaced), "ed4859417");
}

TEST(ErrorCode, TheHeadIsBounded) {
  const auto head = telemetry::error_head(std::string(300, 'a') + " " + std::string(300, 'b'));
  EXPECT_LE(head.size(), 96u);
}

TEST(ErrorCode, AGeneratedValueFirstTemplateRetainsTheOperation) {
  constexpr std::string_view file = "src/platform/windows/display_device/windows_utils.cpp";
  int line = 1;
  while (line <= 10'000 && telemetry::log_template_at(file, line) != "{} failed to set HDR state!") ++line;
  ASSERT_LE(line, 10'000);
  EXPECT_EQ(telemetry::log_template_at("src\\checkout\\src\\platform\\windows\\display_device\\windows_utils.cpp", line),
    "{} failed to set HDR state!");
  EXPECT_TRUE(telemetry::log_template_at("src/does_not_exist.cpp", line).empty());
}

TEST(ErrorCode, TheLegacyUpnpCodesWereTheTwoMappingProtocols) {
  EXPECT_EQ(telemetry::error_code_of("Failed to map UDP 1: 718"), "e1147faac");
  EXPECT_EQ(telemetry::error_code_of("Failed to map TCP 1: 501"), "e857e5618");
}

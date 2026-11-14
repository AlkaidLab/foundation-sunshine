/**
 * @file tests/unit/platform/windows/test_misc.cpp
 * @brief Test Windows timing helpers.
 */
#ifdef _WIN32

  #include <chrono>
  #include <cstdint>
  #include <filesystem>
  #include <fstream>
  #include <iterator>
  #include <limits>
  #include <string>

  #include <boost/log/core.hpp>
  #include <boost/log/sinks/sync_frontend.hpp>
  #include <boost/log/sinks/text_file_backend.hpp>

  #include <src/logging.h>
  #include <src/platform/windows/misc.h>

  #include "../../../tests_common.h"

using namespace std::chrono_literals;

TEST(WindowsQpcTiming, ConvertsTicksUsingTicksPerSecond) {
  constexpr std::int64_t frequency = 10'000'000;

  EXPECT_EQ(platf::qpc_ticks_to_duration(100'000, frequency), 10ms);
  EXPECT_EQ(platf::qpc_ticks_to_duration(-100'000, frequency), -10ms);
}

TEST(WindowsQpcTiming, PreservesSubMillisecondPrecision) {
  constexpr std::int64_t frequency = 10'000'000;

  EXPECT_EQ(platf::qpc_ticks_to_duration(1, frequency), 100ns);
  EXPECT_EQ(platf::qpc_ticks_to_duration(15, frequency), 1500ns);
}

TEST(WindowsQpcTiming, RejectsInvalidFrequency) {
  EXPECT_EQ(platf::qpc_ticks_to_duration(123, 0), 0ns);
  EXPECT_EQ(platf::qpc_ticks_to_duration(123, -1), 0ns);
}

TEST(WindowsQpcTiming, SaturatesExtremeTicksAtLowFrequency) {
  EXPECT_EQ(platf::qpc_ticks_to_duration(2, 1), 2s);
  EXPECT_EQ(platf::qpc_ticks_to_duration(-2, 1), -2s);
  EXPECT_EQ(
    platf::qpc_ticks_to_duration(std::numeric_limits<std::int64_t>::max(), 1),
    std::chrono::nanoseconds::max());
  EXPECT_EQ(
    platf::qpc_ticks_to_duration(std::numeric_limits<std::int64_t>::min(), 1),
    std::chrono::nanoseconds::min());
}

TEST(WindowsQpcTiming, SaturatesExtremeCounterDifferences) {
  EXPECT_EQ(
    platf::qpc_time_difference(
      std::numeric_limits<std::int64_t>::max(),
      std::numeric_limits<std::int64_t>::min()),
    std::chrono::nanoseconds::max());
  EXPECT_EQ(
    platf::qpc_time_difference(
      std::numeric_limits<std::int64_t>::min(),
      std::numeric_limits<std::int64_t>::max()),
    std::chrono::nanoseconds::min());
}

TEST(WindowsTextEncoding, ConvertsGbkBytesToUtf8) {
  const std::string gbk = "\xcf\xb5\xcd\xb3\xd5\xd2\xb2\xbb\xb5\xbd\xd6\xb8\xb6\xa8\xb5\xc4\xce\xc4\xbc\xfe\xa1\xa3";
  const std::string utf8 = "\xe7\xb3\xbb\xe7\xbb\x9f\xe6\x89\xbe\xe4\xb8\x8d\xe5\x88\xb0\xe6\x8c\x87\xe5\xae\x9a\xe7\x9a\x84\xe6\x96\x87\xe4\xbb\xb6\xe3\x80\x82";

  EXPECT_FALSE(platf::is_valid_utf8(gbk));
  EXPECT_TRUE(platf::is_valid_utf8(utf8));
  EXPECT_EQ(platf::to_utf8(gbk, 936), utf8);
  EXPECT_EQ(platf::to_utf8_best_effort(gbk, 936), utf8);
  EXPECT_EQ(platf::to_utf8_best_effort("plain ASCII", 936), "plain ASCII");
}

TEST(WindowsTextEncoding, WideConversionRoundTrips) {
  const std::string utf8 = "\xe7\xb3\xbb\xe7\xbb\x9f\xe6\x89\xbe\xe4\xb8\x8d\xe5\x88\xb0\xe6\x8c\x87\xe5\xae\x9a\xe7\x9a\x84\xe6\x96\x87\xe4\xbb\xb6\xe3\x80\x82";
  const std::wstring wide = platf::from_utf8(utf8);
  EXPECT_FALSE(wide.empty());
  EXPECT_EQ(platf::to_utf8(wide), utf8);
}

TEST(WindowsTextEncoding, FormatsSystemMessagesAsUtf8) {
  const auto missing = platf::format_message_utf8(ERROR_FILE_NOT_FOUND);
  EXPECT_FALSE(missing.empty());
  EXPECT_TRUE(platf::is_valid_utf8(missing));
  EXPECT_EQ(missing.find_first_of("\r\n"), std::string::npos);

  const auto hresult = platf::format_message_utf8(static_cast<DWORD>(0x80070005u));
  EXPECT_FALSE(hresult.empty());
  EXPECT_TRUE(platf::is_valid_utf8(hresult));

  const auto unknown = platf::format_message_utf8(static_cast<DWORD>(0x88890004u));
  EXPECT_TRUE(platf::is_valid_utf8(unknown));
  EXPECT_NE(unknown.find("unknown system message"), std::string::npos);
}

TEST(WindowsTextEncoding, FileLogKeepsSystemMessagesUtf8) {
  namespace fs = std::filesystem;
  const auto path = fs::temp_directory_path() / "m3e-logs-windows-system.log";
  auto backend = boost::make_shared<boost::log::sinks::text_file_backend>(
    boost::log::keywords::file_name = path.string(),
    boost::log::keywords::open_mode = std::ios_base::out | std::ios_base::trunc);
  using sink_t = boost::log::sinks::synchronous_sink<boost::log::sinks::text_file_backend>;
  auto sink = boost::make_shared<sink_t>(backend);
  sink->set_formatter(&logging::formatter);
  boost::log::core::get()->add_sink(sink);
  BOOST_LOG(info) << "Windows system message (zh-CN): "
                  << platf::format_message_utf8(ERROR_FILE_NOT_FOUND, MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED));
  BOOST_LOG(info) << "Windows system message (en-US): "
                  << platf::format_message_utf8(ERROR_FILE_NOT_FOUND, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
  const std::string legacy_gbk = "\xcf\xb5\xcd\xb3\xd5\xd2\xb2\xbb\xb5\xbd\xd6\xb8\xb6\xa8\xb5\xc4\xce\xc4\xbc\xfe\xa1\xa3";
  const std::string legacy_utf8 = "\xe7\xb3\xbb\xe7\xbb\x9f\xe6\x89\xbe\xe4\xb8\x8d\xe5\x88\xb0\xe6\x8c\x87\xe5\xae\x9a\xe7\x9a\x84\xe6\x96\x87\xe4\xbb\xb6\xe3\x80\x82";
  BOOST_LOG(info) << "Legacy tool output: " << legacy_gbk;
  boost::log::core::get()->flush();
  boost::log::core::get()->remove_sink(sink);

  std::ifstream in(path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  ASSERT_FALSE(bytes.empty());
  EXPECT_TRUE(platf::is_valid_utf8(bytes));
  EXPECT_NE(bytes.find("Windows system message"), std::string::npos);
  EXPECT_NE(bytes.find(legacy_utf8), std::string::npos);
}

#endif

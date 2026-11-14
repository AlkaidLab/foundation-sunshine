/**
 * @file tests/unit/test_client_interface.cpp
 * @brief Contract and compatibility tests for the specialised client facts.
 */
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <gtest/gtest.h>

#include "src/client_interface.h"

namespace {
  namespace pt = boost::property_tree;

  pt::ptree
  parse_xml(const char *xml) {
    pt::ptree tree;
    std::istringstream input { xml };
    pt::read_xml(input, tree);
    return tree;
  }
}  // namespace

TEST(ClientInterface, InfersStableKindsAndSources) {
  EXPECT_EQ(client_interface::parse_kind("bigscreen"), client_interface::fs_kind_e::bigscreen);
  EXPECT_EQ(client_interface::parse_kind(" desktop "), client_interface::fs_kind_e::desktop);
  EXPECT_FALSE(client_interface::parse_kind("unknown"));
  EXPECT_EQ(client_interface::infer_kind(false, false, ""), client_interface::fs_kind_e::desktop);
  EXPECT_EQ(client_interface::infer_kind(false, true, "steam://open/bigpicture"), client_interface::fs_kind_e::tool);
  EXPECT_EQ(client_interface::infer_kind(true, false, ""), client_interface::fs_kind_e::bigscreen);
  EXPECT_EQ(client_interface::infer_source("steam://rungameid/123"), "steam");
  EXPECT_EQ(client_interface::infer_source("/opt/custom/game"), "custom");
}

TEST(ClientInterface, ArtVersionChangesWhenAnAssetPathChanges) {
  const auto first = client_interface::make_art_version("42", "poster.png", "", "", "");
  const auto second = client_interface::make_art_version("42", "poster.png", "hero.jpg", "", "");
  EXPECT_FALSE(first.empty());
  EXPECT_NE(first, second);
}

TEST(ClientInterface, LegacyXmlFieldsRemainReadableWithExtensions) {
  const auto tree = parse_xml(
    "<root><App><IsHdrSupported>1</IsHdrSupported><AppTitle>Desktop</AppTitle><ID>42</ID>"
    "<FsKind>desktop</FsKind><FsLastPlayed>10</FsLastPlayed><FsArtVersion>1</FsArtVersion></App></root>");
  const auto &app = tree.get_child("root.App");
  EXPECT_EQ(app.get<int>("IsHdrSupported"), 1);
  EXPECT_EQ(app.get<std::string>("AppTitle"), "Desktop");
  EXPECT_EQ(app.get<std::string>("ID"), "42");
}

TEST(ClientInterface, AGameWithLauncherOrToolInItsPathIsStillAGame) {
  EXPECT_EQ(client_interface::infer_kind(false, true, "C:\\Games\\Tools\\Game\\Launcher.exe"), client_interface::fs_kind_e::game);
  EXPECT_EQ(client_interface::infer_kind(false, true, "steam://open/bigpicture"), client_interface::fs_kind_e::tool);
  EXPECT_EQ(client_interface::infer_kind(false, true, "C:\\Program Files\\DaVinci Resolve\\Resolve.exe"), client_interface::fs_kind_e::tool);
}

TEST(ClientInterface, OnlyImagesAreArt) {
  for (const char *extension : { ".png", ".PNG", ".jpg", ".jpeg", ".webp", ".ico", ".svg" }) EXPECT_TRUE(client_interface::is_art_extension(extension)) << extension;
  for (const char *extension : { "", ".json", ".exe", ".txt", ".conf" }) EXPECT_FALSE(client_interface::is_art_extension(extension)) << extension;
}

TEST(ClientInterface, ASessionCountsForItsSpanAndNeverForASessionTheHostLost) {
  EXPECT_EQ(client_interface::measured_session_seconds(1000, 1600, 5), 600);
  EXPECT_EQ(client_interface::measured_session_seconds(0, 1600, 90), 90) << "no start recorded: the fallback";
  EXPECT_EQ(client_interface::measured_session_seconds(0, 1600, -5), 0);
  EXPECT_EQ(client_interface::measured_session_seconds(0, 1600, 10 * 24 * 3600), 24 * 3600) << "never more than a day without a recorded start";
  EXPECT_EQ(client_interface::measured_session_seconds(1000, 1000 + 8 * 24 * 3600, 30), 30) << "a start older than a week was never ended: not play time";
  EXPECT_EQ(client_interface::measured_session_seconds(2000, 1000, 7), 7) << "a clock that went back";
}

TEST(ClientInterface, TheCrcIsStableAndTheArtVersionChangesWithEachPart) {
  EXPECT_EQ(client_interface::crc32_text("123456789"), std::to_string(0xCBF43926U));
  EXPECT_NE(client_interface::make_art_version("1", "a", "b", "c", "d"), client_interface::make_art_version("1", "a", "b", "c", "e"));
}

TEST(ClientInterface, TheArtVersionCarriesTheFilesThemselves) {
  EXPECT_EQ(client_interface::live_art_version("4272669307", ""), "4272669307") << "no file present: the configured tag as before";
  const auto first = client_interface::live_art_version("4272669307", "poster:1024:1709208000;");
  EXPECT_NE(first, "4272669307");
  EXPECT_EQ(first, client_interface::live_art_version("4272669307", "poster:1024:1709208000;"));
  EXPECT_NE(first, client_interface::live_art_version("4272669307", "poster:2048:1709208000;")) << "a replaced picture with the same time";
  EXPECT_NE(first, client_interface::live_art_version("4272669307", "poster:1024:1709208001;")) << "the same picture written again";
  EXPECT_NE(first, client_interface::live_art_version("4272669307", "poster:1024:1709208000;hero:1:2;")) << "another art file joins";
}

TEST(ClientInterface, AnEntityTagIsFoundInAnyIfNoneMatchValue) {
  constexpr std::string_view etag = "\"784111777-poster\"";
  EXPECT_TRUE(client_interface::etag_matches("\"784111777-poster\"", etag));
  EXPECT_TRUE(client_interface::etag_matches("*", etag)) << "the origin server has a current representation";
  EXPECT_TRUE(client_interface::etag_matches("W/\"784111777-poster\"", etag)) << "weak comparison";
  EXPECT_TRUE(client_interface::etag_matches(" \"784111777-hero\" , W/\"784111777-poster\"", etag)) << "a list with optional whitespace";
  EXPECT_TRUE(client_interface::etag_matches("\t\"784111777-poster\"", etag)) << "surrounding whitespace";
  EXPECT_FALSE(client_interface::etag_matches("", etag));
  EXPECT_FALSE(client_interface::etag_matches("\"784111777-hero\"", etag));
  EXPECT_FALSE(client_interface::etag_matches("784111777-poster", etag)) << "an unquoted word is not an entity tag";
  EXPECT_FALSE(client_interface::etag_matches("\"784111777-poster\"", "\"784111777-hero\""));
}

TEST(ClientInterface, AllThreeHttpDateFormsAreAcceptedAndFixdateIsGenerated) {
  EXPECT_EQ(client_interface::format_http_date(0), "Thu, 01 Jan 1970 00:00:00 GMT");
  EXPECT_EQ(client_interface::format_http_date(784111777), "Sun, 06 Nov 1994 08:49:37 GMT");
  EXPECT_EQ(client_interface::format_http_date(1582934400), "Sat, 29 Feb 2020 00:00:00 GMT");

  EXPECT_EQ(client_interface::parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT"), std::optional<std::int64_t> { 784111777 });
  EXPECT_EQ(client_interface::parse_http_date(" \tSun, 06 Nov 1994 08:49:37 GMT "), std::optional<std::int64_t> { 784111777 }) << "optional whitespace";
  EXPECT_EQ(client_interface::parse_http_date("Sunday, 06-Nov-94 08:49:37 GMT"), std::optional<std::int64_t> { 784111777 }) << "the obsolete RFC 850 form";
  EXPECT_EQ(client_interface::parse_http_date("Sunday, 06-Nov-68 08:49:37 GMT"), std::optional<std::int64_t> { -36342623 }) << "a two-digit year after 49 is the past century";
  EXPECT_EQ(client_interface::parse_http_date("Sun Nov  6 08:49:37 1994"), std::optional<std::int64_t> { 784111777 }) << "the asctime form with a one-digit day";
  EXPECT_EQ(client_interface::parse_http_date("Sun Nov 16 08:49:37 1994"), std::optional<std::int64_t> { 784975777 }) << "the asctime form with a two-digit day";
  EXPECT_EQ(client_interface::parse_http_date("Sat, 30 Jun 2012 23:59:60 GMT"), std::optional<std::int64_t> { 1341100799 }) << "a leap second stands for the second before";
  for (const std::int64_t moment : { std::int64_t { 0 }, std::int64_t { 784111777 }, std::int64_t { 1582934400 }, std::int64_t { 946684799 }, std::int64_t { 1709208000 } }) {
    EXPECT_EQ(client_interface::parse_http_date(client_interface::format_http_date(moment)), std::optional<std::int64_t> { moment }) << moment;
  }

  EXPECT_FALSE(client_interface::parse_http_date("Sun, 06 Nov 1994 08:49:37").has_value()) << "no zone";
  EXPECT_FALSE(client_interface::parse_http_date("Sun, 06 Nov 1994 08:49:37 UTC").has_value()) << "the zone is GMT";
  EXPECT_FALSE(client_interface::parse_http_date("Sun, 6 Nov 1994 08:49:37 GMT").has_value()) << "the day is two digits";
  EXPECT_FALSE(client_interface::parse_http_date("Sun, 06 Nov 94 08:49:37 GMT").has_value()) << "the year is four digits";
  EXPECT_FALSE(client_interface::parse_http_date("Sun, 06 Xxx 1994 08:49:37 GMT").has_value());
  EXPECT_FALSE(client_interface::parse_http_date("Mon, 31 Feb 1994 08:49:37 GMT").has_value()) << "February 1994 had 28 days";
  EXPECT_TRUE(client_interface::parse_http_date("Thu, 29 Feb 2024 08:49:37 GMT").has_value());
  EXPECT_TRUE(client_interface::parse_http_date("Tue, 29 Feb 2000 08:49:37 GMT").has_value()) << "a century leap year has 29";
  EXPECT_EQ(client_interface::parse_http_date("Tue, 29 Feb 2000 08:49:37 GMT"), std::optional<std::int64_t> { 951814177 });
  EXPECT_FALSE(client_interface::parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT extra").has_value());
  EXPECT_FALSE(client_interface::parse_http_date("Sun, 32 Nov 1994 08:49:37 GMT").has_value());
  EXPECT_FALSE(client_interface::parse_http_date("Sun, 06 Nov 1994 24:49:37 GMT").has_value());
  EXPECT_FALSE(client_interface::parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT, Sun, 06 Nov 1994 08:49:37 GMT").has_value()) << "a date is one member";
  EXPECT_FALSE(client_interface::parse_http_date("").has_value());
  EXPECT_FALSE(client_interface::parse_http_date("not a date").has_value());
}

TEST(ClientInterface, IfNoneMatchDecidesAndIfModifiedSinceServesWithoutIt) {
  constexpr std::string_view etag = "\"784111777-poster\"";
  EXPECT_TRUE(client_interface::conditional_not_modified(true, "\"784111777-poster\"", "", etag, 100));
  EXPECT_TRUE(client_interface::conditional_not_modified(true, "*", "", etag, 100));
  EXPECT_FALSE(client_interface::conditional_not_modified(true, "\"784111777-hero\"", "Thu, 01 Jan 1970 00:01:40 GMT", etag, 100))
    << "a tag that matches nothing is a full response even when the date alone would say not modified";
  EXPECT_FALSE(client_interface::conditional_not_modified(true, "", "Thu, 01 Jan 1970 00:00:01 GMT", etag, 100))
    << "a present field that matches nothing is a full response, and the date stays ignored";
  EXPECT_TRUE(client_interface::conditional_not_modified(false, "", "Thu, 01 Jan 1970 00:00:01 GMT", etag, 1)) << "unchanged at the very second";
  EXPECT_FALSE(client_interface::conditional_not_modified(false, "", "Thu, 01 Jan 1970 00:00:00 GMT", etag, 1)) << "changed after the date";
  EXPECT_FALSE(client_interface::conditional_not_modified(false, "", "not a date", etag, 1)) << "an invalid date is ignored";
  EXPECT_FALSE(client_interface::conditional_not_modified(false, "", "Thu, 01 Jan 1970 00:00:01 GMT", etag, -1)) << "no modification date available";
  EXPECT_FALSE(client_interface::conditional_not_modified(false, "", "", etag, 1)) << "no condition at all";
}

TEST(ClientInterface, TheListTagChangesWithThePlayHistoryAndStaysANumber) {
  EXPECT_EQ(client_interface::combine_list_etag("12345", ""), "12345") << "no history file: the tag as before";
  const auto first = client_interface::combine_list_etag("12345", "120:1000");
  const auto second = client_interface::combine_list_etag("12345", "150:2000");
  EXPECT_NE(first, "12345");
  EXPECT_NE(first, second) << "a session that ends changes the tag";
  EXPECT_EQ(first, client_interface::combine_list_etag("12345", "120:1000"));
  for (const auto character : first) EXPECT_TRUE(character >= '0' && character <= '9') << first;
}

TEST(ClientInterface, ApplicationListTagFollowsTheArtFilesAndOnlyThem) {
  namespace fs = std::filesystem;
  const auto dir = fs::temp_directory_path() / "client-interface-art-tag";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const auto hero = dir / "hero.jpg";
  EXPECT_EQ(client_interface::art_file_fact("hero", hero), "") << "no file: no fact";
  {
    std::ofstream out { hero, std::ios::binary };
    out << "first picture";
  }
  const auto fact = client_interface::art_file_fact("hero", hero);
  EXPECT_EQ(fact.rfind("hero:13:", 0), 0u);
  EXPECT_EQ(fact, client_interface::art_file_fact("hero", hero)) << "asked twice, nothing written: the same fact";
  const auto version = client_interface::live_art_version("4272669307", fact);
  const auto apps_art = std::string { "1:" } + version + ";2:" + client_interface::live_art_version("1", "") + ";";
  const auto tag = client_interface::combine_art_etag("12345", apps_art);
  EXPECT_EQ(tag, client_interface::combine_art_etag("12345", apps_art)) << "the tag is stable while no file changes";
  EXPECT_EQ(client_interface::combine_art_etag("12345", ""), "12345") << "no application: the list's own tag";
  EXPECT_NE(tag, "12345");
  EXPECT_FALSE(tag.empty());
  EXPECT_TRUE(std::all_of(tag.begin(), tag.end(), [](unsigned char c) { return std::isdigit(c); })) << "decimal digits, as the list tag was";
  // the hero is replaced by a picture of another size: the app's art version and the list tag both move
  {
    std::ofstream out { hero, std::ios::binary | std::ios::trunc };
    out << "a second, larger picture";
  }
  const auto changed = client_interface::art_file_fact("hero", hero);
  EXPECT_NE(changed, fact);
  const auto changed_apps_art = std::string { "1:" } + client_interface::live_art_version("4272669307", changed) + ";2:" + client_interface::live_art_version("1", "") + ";";
  EXPECT_NE(client_interface::combine_art_etag("12345", changed_apps_art), tag);
  // the same picture written again with another time changes the fact too
  fs::last_write_time(hero, fs::last_write_time(hero) + std::chrono::seconds(5));
  EXPECT_NE(client_interface::art_file_fact("hero", hero), changed);
  // another list tag (an app was added) moves the combined tag with the art unchanged
  EXPECT_NE(client_interface::combine_art_etag("12346", apps_art), tag);
  fs::remove_all(dir);
}

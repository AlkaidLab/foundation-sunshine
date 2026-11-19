/**
 * @file tests/unit/test_version_compare.cpp
 * @brief version_compare against the shared table tests/fixtures/version_vectors.json (the panel and the Web UI run the same table).
 */
#include <src/version_compare.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>

namespace {
  nlohmann::json
  load_vectors() {
    std::ifstream in(SUNSHINE_VERSION_VECTORS, std::ios::binary);
    EXPECT_TRUE(in.is_open()) << SUNSHINE_VERSION_VECTORS;
    return nlohmann::json::parse(in);
  }

  // UTF-8 text as the wide string the Windows installer reads from the registry
  std::wstring
  widen(const std::string &utf8) {
    std::wstring out;
    for (std::size_t i = 0; i < utf8.size();) {
      const unsigned char c = utf8[i];
      char32_t cp;
      std::size_t n;
      if (c < 0x80) { cp = c; n = 1; }
      else if (c < 0xE0) { cp = c & 0x1F; n = 2; }
      else if (c < 0xF0) { cp = c & 0x0F; n = 3; }
      else { cp = c & 0x07; n = 4; }
      for (std::size_t k = 1; k < n && i + k < utf8.size(); ++k) cp = (cp << 6) | (utf8[i + k] & 0x3F);
      i += n;
      if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
        cp -= 0x10000;
        out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
        out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
      }
      else out.push_back(static_cast<wchar_t>(cp));
    }
    return out;
  }
}  // namespace

TEST(VersionCompare, TableIsNotEmpty) {
  const auto v = load_vectors();
  EXPECT_EQ(v["schema"], 1);
  EXPECT_GE(v["parse"].size(), 25u);
  EXPECT_GE(v["compare"].size(), 15u);
}

TEST(VersionCompare, ParseMatchesTable) {
  for (const auto &row : load_vectors()["parse"]) {
    const std::string input = row["input"];
    const auto narrow = version_compare::parse(std::string_view(input));
    const auto wide = version_compare::parse(std::wstring_view(widen(input)));
    if (row["key"].is_null()) {
      EXPECT_FALSE(narrow) << input;
      EXPECT_FALSE(wide) << input;
    }
    else {
      const version_compare::key_t want { row["key"][0], row["key"][1], row["key"][2] };
      ASSERT_TRUE(narrow) << input;
      ASSERT_TRUE(wide) << input;
      EXPECT_EQ(*narrow, want) << input;
      EXPECT_EQ(*wide, want) << input;
    }
  }
}

TEST(VersionCompare, CompareMatchesTable) {
  for (const auto &row : load_vectors()["compare"]) {
    const std::string left = row["left"], right = row["right"];
    const auto narrow = version_compare::compare(std::string_view(left), std::string_view(right));
    const auto wide = version_compare::compare(std::wstring_view(widen(left)), std::wstring_view(widen(right)));
    if (row["order"].is_null()) {
      EXPECT_FALSE(narrow) << left << " vs " << right;
      EXPECT_FALSE(wide) << left << " vs " << right;
    }
    else {
      const int want = row["order"];
      ASSERT_TRUE(narrow) << left << " vs " << right;
      ASSERT_TRUE(wide) << left << " vs " << right;
      EXPECT_EQ(*narrow, want) << left << " vs " << right;
      EXPECT_EQ(*wide, want) << left << " vs " << right;
    }
  }
}

TEST(VersionCompare, CompareIsAntisymmetric) {
  for (const auto &row : load_vectors()["compare"]) {
    const std::string left = row["left"], right = row["right"];
    const auto a = version_compare::compare(std::string_view(left), std::string_view(right));
    const auto b = version_compare::compare(std::string_view(right), std::string_view(left));
    ASSERT_EQ(a.has_value(), b.has_value());
    if (a) {
      EXPECT_EQ(*a, -*b);
    }
  }
}

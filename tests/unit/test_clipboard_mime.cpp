/**
 * @file tests/unit/test_clipboard_mime.cpp
 * @brief Unit tests for the desktop-selection flavour policy.
 *
 * `clipboard_mime::choose()` decides which representation of an offered clipboard
 * item the host asks the compositor for. It is pure (no Wayland, no bus), so the
 * rules that matter — images first, UTF-8 text before Latin-1 text, and "ask for
 * nothing" rather than fetching a flavour the wire cannot carry — are pinned here
 * instead of being inferred from live clipboard behaviour.
 */
#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "src/platform/linux/clipboard_mime.h"

namespace {
  using platf::clipboard_mime::choose;
  using platf::clipboard_mime::kind_e;

  std::vector<std::string>
  offers(std::initializer_list<const char *> mimes) {
    std::vector<std::string> out;
    out.reserve(mimes.size());
    for (const auto *mime: mimes) {
      out.emplace_back(mime);
    }
    return out;
  }
}  // namespace

TEST(ClipboardMime, PrefersPngOverAnyTextFlavour) {
  const auto picked = choose(offers({ "text/plain;charset=utf-8", "image/png", "text/html" }));
  EXPECT_EQ(picked.kind, kind_e::image);
  EXPECT_EQ(picked.mime, "image/png");
}

TEST(ClipboardMime, IgnoresPngParametersWhenMatching) {
  // Some toolkits advertise the flavour with a parameter list.
  const auto picked = choose(offers({ "image/png;charset=binary" }));
  EXPECT_EQ(picked.kind, kind_e::image);
  // The request must use the bare mime the wire and the compositor expect.
  EXPECT_EQ(picked.mime, "image/png");
}

TEST(ClipboardMime, PicksUtf8TextBeforePlainText) {
  const auto picked = choose(offers({ "text/plain", "text/plain;charset=utf-8" }));
  EXPECT_EQ(picked.kind, kind_e::text);
  EXPECT_EQ(picked.mime, "text/plain;charset=utf-8");
}

TEST(ClipboardMime, AcceptsSpacedAndUppercasedCharsetParameter) {
  const auto picked = choose(offers({ "text/plain", "TEXT/PLAIN; charset=UTF-8" }));
  EXPECT_EQ(picked.kind, kind_e::text);
  // The offered spelling is kept verbatim: it is what receive() must name.
  EXPECT_EQ(picked.mime, "TEXT/PLAIN; charset=UTF-8");
}

TEST(ClipboardMime, PrefersX11Utf8StringOverPlainText) {
  const auto picked = choose(offers({ "text/plain", "UTF8_STRING" }));
  EXPECT_EQ(picked.kind, kind_e::text);
  EXPECT_EQ(picked.mime, "UTF8_STRING");
}

TEST(ClipboardMime, FallsBackToAnyTextFlavour) {
  // text/html is not text we can carry faithfully, but it is text on the wire,
  // and the klipper half has always posted whatever klipper returned.
  const auto picked = choose(offers({ "text/html" }));
  EXPECT_EQ(picked.kind, kind_e::text);
  EXPECT_EQ(picked.mime, "text/html");
}

TEST(ClipboardMime, NothingCarriableMeansNoRequest) {
  const auto picked = choose(offers({ "application/x-kde-cutselection", "image/svg+xml", "STRING" }));
  EXPECT_EQ(picked.kind, kind_e::none);
  EXPECT_TRUE(picked.mime.empty());
}

TEST(ClipboardMime, EmptyOfferListMeansNoRequest) {
  const auto picked = choose({});
  EXPECT_EQ(picked.kind, kind_e::none);
  EXPECT_TRUE(picked.mime.empty());
}

TEST(ClipboardMime, IsTextMimeRejectsBinaryAndLegacyAtoms) {
  EXPECT_TRUE(platf::clipboard_mime::is_text_mime("text/plain"));
  EXPECT_TRUE(platf::clipboard_mime::is_text_mime("text/plain;charset=utf-8"));
  EXPECT_TRUE(platf::clipboard_mime::is_text_mime("UTF8_STRING"));
  // COMPOUND_TEXT/STRING are single-byte encodings: carrying them as UTF-8
  // would hand clients mojibake, so they are refused rather than mangled.
  EXPECT_FALSE(platf::clipboard_mime::is_text_mime("STRING"));
  EXPECT_FALSE(platf::clipboard_mime::is_text_mime("COMPOUND_TEXT"));
  EXPECT_FALSE(platf::clipboard_mime::is_text_mime("image/png"));
}

TEST(ClipboardMime, TextOffersLeadWithUtf8AndIncludeX11Atom) {
  const auto offered = platf::clipboard_mime::text_offers();
  ASSERT_FALSE(offered.empty());
  EXPECT_EQ(offered.front(), "text/plain;charset=utf-8");
  EXPECT_NE(std::find(offered.begin(), offered.end(), "UTF8_STRING"), offered.end());
  EXPECT_NE(std::find(offered.begin(), offered.end(), "text/plain"), offered.end());
  // Everything we advertise must be something send() will serve.
  for (const auto &mime: offered) {
    EXPECT_TRUE(platf::clipboard_mime::is_text_mime(mime)) << mime;
  }
}

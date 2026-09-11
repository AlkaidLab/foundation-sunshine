/**
 * @file tests/unit/test_clipboard_echo.cpp
 * @brief Test the clipboard echo-suppression ring.
 *
 * Mirrors the GUI agent's EchoState: 16 entries of (kind, payload hash) with a
 * TTL. The bug it prevents: a single-slot comparison dropped the older of two
 * consecutive client copies, so a host copy of that older value was broadcast
 * back to the clients.
 */
#include <src/clipboard_echo.h>

#include "../tests_common.h"

#include <chrono>
#include <string>

namespace {
  using clipboard_echo::ring_t;

  std::span<const std::uint8_t>
  bytes_of(const std::string &text) {
    return { reinterpret_cast<const std::uint8_t *>(text.data()), text.size() };
  }
}  // namespace

TEST(ClipboardEcho, MatchesWhatThisHostWrote) {
  ring_t ring;
  const auto t0 = std::chrono::steady_clock::now();

  ring.record(clipboard_bridge::kKindText, bytes_of("alpha"), t0);
  EXPECT_TRUE(ring.is_echo(clipboard_bridge::kKindText, bytes_of("alpha"), t0));
  EXPECT_FALSE(ring.is_echo(clipboard_bridge::kKindText, bytes_of("beta"), t0));
  EXPECT_FALSE(ring.is_echo(clipboard_bridge::kKindPng, bytes_of("alpha"), t0))
    << "the kind is part of the identity";
}

TEST(ClipboardEcho, RemembersSeveralConsecutiveWrites) {
  ring_t ring;
  const auto t0 = std::chrono::steady_clock::now();

  ring.record(clipboard_bridge::kKindText, bytes_of("first"), t0);
  ring.record(clipboard_bridge::kKindText, bytes_of("second"), t0);

  // Both stay suppressed: this is what the single-slot version got wrong.
  EXPECT_TRUE(ring.is_echo(clipboard_bridge::kKindText, bytes_of("first"), t0));
  EXPECT_TRUE(ring.is_echo(clipboard_bridge::kKindText, bytes_of("second"), t0));
  EXPECT_EQ(ring.size(), 2u);
}

TEST(ClipboardEcho, EntriesExpireWithTheTtl) {
  ring_t ring;
  const auto t0 = std::chrono::steady_clock::now();

  ring.record(clipboard_bridge::kKindText, bytes_of("value"), t0, std::chrono::seconds { 5 });
  EXPECT_TRUE(ring.is_echo(clipboard_bridge::kKindText, bytes_of("value"), t0 + std::chrono::seconds { 4 }));
  EXPECT_FALSE(ring.is_echo(clipboard_bridge::kKindText, bytes_of("value"), t0 + std::chrono::seconds { 6 }));
  EXPECT_EQ(ring.size(), 0u) << "checking prunes expired entries";
}

TEST(ClipboardEcho, OldestEntryIsDroppedAtCapacity) {
  ring_t ring;
  const auto t0 = std::chrono::steady_clock::now();

  for (std::size_t i = 0; i < clipboard_echo::kCapacity + 1; ++i) {
    ring.record(clipboard_bridge::kKindText, bytes_of("entry-" + std::to_string(i)), t0);
  }

  EXPECT_EQ(ring.size(), clipboard_echo::kCapacity);
  EXPECT_FALSE(ring.is_echo(clipboard_bridge::kKindText, bytes_of("entry-0"), t0)) << "the oldest fell out";
  EXPECT_TRUE(ring.is_echo(clipboard_bridge::kKindText, bytes_of("entry-16"), t0));

  // The ring keeps working after wrapping around.
  ring.record(clipboard_bridge::kKindText, bytes_of("entry-17"), t0);
  EXPECT_EQ(ring.size(), clipboard_echo::kCapacity);
  EXPECT_TRUE(ring.is_echo(clipboard_bridge::kKindText, bytes_of("entry-17"), t0));
  EXPECT_FALSE(ring.is_echo(clipboard_bridge::kKindText, bytes_of("entry-1"), t0));
}

TEST(ClipboardEcho, ClearForgetsEverything) {
  ring_t ring;
  ring.record(clipboard_bridge::kKindText, bytes_of("gone"));
  ring.clear();
  EXPECT_EQ(ring.size(), 0u);
  EXPECT_FALSE(ring.is_echo(clipboard_bridge::kKindText, bytes_of("gone")));
}

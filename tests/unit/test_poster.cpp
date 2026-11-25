#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"  // the static stb implementation carries entry points this file never calls
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "src/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "src/stb_image_write.h"
#pragma GCC diagnostic pop

#include "src/nvhttp/poster.h"
#include "src/platform/common.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "test_temp_paths.h"
#include <random>
#include <string_view>
#include <thread>
#include <vector>

namespace {

  std::filesystem::path
  test_root() {
    const auto root = test_support::unique_temp_path("poster-unit");
    std::filesystem::create_directories(root);
    return root;
  }

  std::filesystem::path
  write_png(const std::filesystem::path &path, int width, int height, bool random_pixels) {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    std::mt19937 generator(0x5eed);
    for (std::size_t index = 0; index < pixels.size(); index += 4) {
      pixels[index] = random_pixels ? static_cast<unsigned char>(generator()) : 20;
      pixels[index + 1] = random_pixels ? static_cast<unsigned char>(generator()) : 40;
      pixels[index + 2] = random_pixels ? static_cast<unsigned char>(generator()) : 60;
      pixels[index + 3] = 255;
    }
    int size = 0;
    auto *encoded = stbi_write_png_to_mem(pixels.data(), width * 4, width, height, 4, &size);
    EXPECT_NE(encoded, nullptr);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(encoded), size);
    STBIW_FREE(encoded);
    return path;
  }

  std::vector<unsigned char>
  decode(const std::filesystem::path &path, int &width, int &height) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = static_cast<int>(input.tellg());
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(bytes.data()), size);
    int channels = 0;
    auto *pixels = stbi_load_from_memory(bytes.data(), size, &width, &height, &channels, 4);
    EXPECT_NE(pixels, nullptr);
    std::vector<unsigned char> result(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return result;
  }

  std::filesystem::path
  utf8_path(std::u8string_view text) {
    return std::filesystem::path { text };
  }

  void
  write_undecodable(const std::filesystem::path &path, std::size_t size) {
    // Large invalid input makes the worker give up immediately.
    std::vector<unsigned char> bytes(size, 'x');
    std::mt19937 generator(0xbad5);
    for (std::size_t index = 8; index < bytes.size(); ++index) {
      bytes[index] = static_cast<unsigned char>(generator());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

}  // namespace

TEST(Poster, FullRepresentationPreservesPixelsAndUsesByteEtag) {
  const auto root = test_root();
  const auto source = write_png(root / "source.png", 1024, 768, true);
  nvhttp::apps::poster::schedule(source);
  const auto full = nvhttp::apps::poster::full(source);
  ASSERT_TRUE(full);
  int source_width = 0;
  int source_height = 0;
  int full_width = 0;
  int full_height = 0;
  EXPECT_EQ(decode(source, source_width, source_height), decode(full->path, full_width, full_height));
  EXPECT_EQ(source_width, full_width);
  EXPECT_EQ(source_height, full_height);
  EXPECT_TRUE(full->etag.size() == 66 && full->etag.front() == '"' && full->etag.back() == '"');
  // Wait for the worker before removing the source on Windows.
  EXPECT_TRUE(nvhttp::apps::poster::wait_idle());
  EXPECT_TRUE(nvhttp::apps::poster::preview(source)) << "once idle, the pass has finished and its preview is on disk";
  test_support::remove_tree_quiet(root);
}

TEST(Poster, BackgroundPreviewIsBoundedAndScaled) {
  const auto root = test_root();
  const auto source = write_png(root / "source.png", 1600, 900, true);
  nvhttp::apps::poster::schedule(source);
  std::optional<nvhttp::apps::poster::representation_t> preview;
  for (int attempt = 0; attempt < 100 && !preview; ++attempt) {
    preview = nvhttp::apps::poster::preview(source);
    if (!preview) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ASSERT_TRUE(preview);
  EXPECT_LE(preview->length, nvhttp::apps::poster::preview_limit_bytes);
  int width = 0;
  int height = 0;
  const auto pixels = decode(preview->path, width, height);
  EXPECT_FALSE(pixels.empty());
  EXPECT_LE(std::max(width, height), nvhttp::apps::poster::preview_longest_side);
  EXPECT_TRUE(nvhttp::apps::poster::wait_idle());
  test_support::remove_tree_quiet(root);
}

TEST(Poster, EvictedPreviewIsGeneratedAgain) {
  // A successfully generated version can be rebuilt after its cache file is removed.
  const auto root = test_root();
  const auto source = write_png(root / "source.png", 1600, 900, true);
  nvhttp::apps::poster::schedule(source);
  ASSERT_TRUE(nvhttp::apps::poster::wait_idle());
  auto preview = nvhttp::apps::poster::preview(source);
  ASSERT_TRUE(preview);
  std::filesystem::remove(preview->path);

  nvhttp::apps::poster::schedule(source);
  ASSERT_TRUE(nvhttp::apps::poster::wait_idle());
  preview = nvhttp::apps::poster::preview(source);
  EXPECT_TRUE(preview) << "the preview is generated again after the cached file was evicted";
  EXPECT_TRUE(nvhttp::apps::poster::wait_idle());
  test_support::remove_tree_quiet(root);
}

TEST(Poster, RepeatedScheduleDoesNotRebuildVariants) {
  const auto root = test_root();
  const auto source = write_png(root / "source.png", 1600, 900, true);
  nvhttp::apps::poster::schedule(source);
  std::optional<nvhttp::apps::poster::representation_t> preview;
  for (int attempt = 0; attempt < 100 && !preview; ++attempt) {
    preview = nvhttp::apps::poster::preview(source);
    if (!preview) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ASSERT_TRUE(preview);
  EXPECT_TRUE(nvhttp::apps::poster::wait_idle());
  const auto built = std::filesystem::last_write_time(preview->path);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  for (int round = 0; round < 3; ++round) {
    nvhttp::apps::poster::schedule(source);
    EXPECT_TRUE(nvhttp::apps::poster::wait_idle());
  }
  EXPECT_EQ(std::filesystem::last_write_time(preview->path), built);
  test_support::remove_tree_quiet(root);
}

TEST(Poster, SmallSourcePreviewUsesFullRepresentation) {
  const auto root = test_root();
  const auto source = write_png(root / "small.png", 8, 8, false);
  const auto full = nvhttp::apps::poster::full(source);
  const auto preview = nvhttp::apps::poster::preview(source);
  ASSERT_TRUE(full);
  ASSERT_TRUE(preview);
  EXPECT_EQ(full->path, preview->path);
  EXPECT_EQ(full->etag, preview->etag);
  test_support::remove_tree_quiet(root);
}

TEST(Poster, RangeAndConditionalValidators) {
  const auto range = nvhttp::apps::poster::parse_range("bytes=10-19", 100);
  ASSERT_TRUE(range);
  EXPECT_EQ(range->first, 10u);
  EXPECT_EQ(range->last, 19u);
  const auto open = nvhttp::apps::poster::parse_range("bytes=90-", 100);
  ASSERT_TRUE(open);
  EXPECT_EQ(open->last, 99u);
  EXPECT_FALSE(nvhttp::apps::poster::parse_range("bytes=-10", 100));
  EXPECT_FALSE(nvhttp::apps::poster::parse_range("bytes=100-", 100));
  EXPECT_TRUE(nvhttp::apps::poster::if_none_match("\"abc\", \"def\"", "\"def\""));
  EXPECT_TRUE(nvhttp::apps::poster::if_none_match("*", "\"def\""));
  EXPECT_FALSE(nvhttp::apps::poster::if_none_match("\"abc\"", "\"def\""));
  EXPECT_TRUE(nvhttp::apps::poster::if_range_matches("\"def\"", "\"def\""));
  EXPECT_TRUE(nvhttp::apps::poster::if_range_matches("  \"def\"  ", "\"def\""));
  EXPECT_FALSE(nvhttp::apps::poster::if_range_matches("\"abc\"", "\"def\""));
  EXPECT_FALSE(nvhttp::apps::poster::if_range_matches("W/\"def\"", "\"def\""));
  EXPECT_FALSE(nvhttp::apps::poster::if_range_matches("Wed, 21 Oct 2015 07:28:00 GMT", "\"def\""));
}

TEST(Poster, NonAsciiSourceCachesUnderAppdataWithAnAsciiName) {
  // Cache paths use appdata and ASCII keys, so the source directory is unchanged.
  const auto root = test_root();
  const auto directory = root / utf8_path(u8"下载 空格");
  std::filesystem::create_directories(directory);
  const auto source = write_png(directory / utf8_path(u8"鸿蒙壁纸1.png"), 1200, 700, true);
  nvhttp::apps::poster::schedule(source);
  // Random pixels may need several shrinking passes.
  const auto preview = nvhttp::apps::poster::preview_when_ready(source, std::chrono::seconds(30));
  ASSERT_TRUE(preview);
  EXPECT_EQ(preview->path.parent_path(), platf::appdata() / "covers" / "cache" / "poster");
  const auto name = preview->path.filename().string();
  ASSERT_FALSE(name.empty());
  EXPECT_TRUE(std::all_of(name.begin(), name.end(), [](unsigned char character) { return character < 0x80; }));
  EXPECT_NE(name.rfind("-preview."), std::string::npos);
  ASSERT_TRUE(nvhttp::apps::poster::wait_idle());
  // The source directory contains only the source file.
  const auto entries = std::distance(std::filesystem::directory_iterator { directory }, std::filesystem::directory_iterator {});
  EXPECT_EQ(entries, 1);
  test_support::remove_tree_quiet(root);
}

TEST(Poster, OverwrittenSourceGetsANewPreviewAndTheOldOneStays) {
  // A changed source version gets a new cache key.
  const auto root = test_root();
  const auto source = write_png(root / "source.png", 1600, 900, true);
  nvhttp::apps::poster::schedule(source);
  const auto first = nvhttp::apps::poster::preview_when_ready(source, std::chrono::seconds(30));
  ASSERT_TRUE(first);
  ASSERT_TRUE(nvhttp::apps::poster::wait_idle());
  write_png(source, 1500, 900, true);
  EXPECT_FALSE(nvhttp::apps::poster::preview(source)) << "the new version has no preview yet";
  nvhttp::apps::poster::schedule(source);
  const auto second = nvhttp::apps::poster::preview_when_ready(source, std::chrono::seconds(30));
  ASSERT_TRUE(second);
  EXPECT_NE(first->path, second->path);
  EXPECT_TRUE(std::filesystem::exists(first->path));
  ASSERT_TRUE(nvhttp::apps::poster::wait_idle());
  test_support::remove_tree_quiet(root);
}

TEST(Poster, CleanupRemovesLeftoverTemporaryFilesAndKeepsCacheFiles) {
  const auto root = test_root();
  const auto cache = root / "poster-cache";
  std::filesystem::create_directories(cache);
  const auto kept = cache / "1-preview.jpg";
  {
    std::ofstream output(kept, std::ios::binary);
    output << "cache";
  }
  const auto leftover = cache / "1-preview.jpg.tmp-4242";
  {
    std::ofstream output(leftover, std::ios::binary);
    output << "half";
  }
  const auto result = nvhttp::apps::poster::cleanup_cache_dir(cache);
  EXPECT_TRUE(result.available);
  EXPECT_EQ(result.removed_temporary, 1u);
  EXPECT_EQ(result.evicted, 0u);
  EXPECT_FALSE(std::filesystem::exists(leftover));
  EXPECT_TRUE(std::filesystem::exists(kept));
  // Only the cache file remains after the write probe is removed.
  const auto entries = std::distance(std::filesystem::directory_iterator { cache }, std::filesystem::directory_iterator {});
  EXPECT_EQ(entries, 1);
  test_support::remove_tree_quiet(root);
}

TEST(Poster, CleanupKeepsTheNewestCacheFilesWithinTheLimit) {
  const auto root = test_root();
  const auto cache = root / "poster-cache";
  std::filesystem::create_directories(cache);
  const auto written = std::filesystem::file_time_type::clock::now() - std::chrono::hours(2);
  for (std::size_t index = 0; index < nvhttp::apps::poster::cache_file_limit + 6; ++index) {
    const auto file = cache / (std::to_string(index) + "-preview.jpg");
    {
      std::ofstream output(file, std::ios::binary);
      output << "cache";
    }
    std::error_code error;
    std::filesystem::last_write_time(file, written + std::chrono::seconds(index), error);  // one file per second, oldest first
    ASSERT_FALSE(error) << file;
  }
  const auto result = nvhttp::apps::poster::cleanup_cache_dir(cache);
  EXPECT_TRUE(result.available);
  EXPECT_EQ(result.evicted, 6u);
  EXPECT_EQ(result.removed_temporary, 0u);
  const auto remaining = std::distance(std::filesystem::directory_iterator { cache }, std::filesystem::directory_iterator {});
  EXPECT_EQ(remaining, static_cast<std::ptrdiff_t>(nvhttp::apps::poster::cache_file_limit));
  EXPECT_FALSE(std::filesystem::exists(cache / "5-preview.jpg"));
  EXPECT_TRUE(std::filesystem::exists(cache / "6-preview.jpg"));
  EXPECT_TRUE(std::filesystem::exists(cache / "69-preview.jpg"));
  test_support::remove_tree_quiet(root);
}

TEST(Poster, CleanupReportsAnUnavailableDirectoryWithoutThrowing) {
  const auto root = test_root();
  const auto occupied = root / "occupied";
  {
    std::ofstream output(occupied, std::ios::binary);
    output << "file";
  }
  EXPECT_FALSE(nvhttp::apps::poster::cleanup_cache_dir(occupied).available);
  EXPECT_FALSE(nvhttp::apps::poster::cleanup_cache_dir(occupied / "cache").available);
  EXPECT_TRUE(std::filesystem::exists(occupied)) << "the file in the way is left alone";
  // Missing parent directories are created.
  const auto made = root / "made" / "poster";
  const auto result = nvhttp::apps::poster::cleanup_cache_dir(made);
  EXPECT_TRUE(result.available);
  EXPECT_TRUE(std::filesystem::is_directory(made));
  test_support::remove_tree_quiet(root);
}

TEST(Poster, WaitReturnsAtOnceWhenNothingWillProduceThePreview) {
  const auto root = test_root();
  const auto source = root / "broken.png";
  write_undecodable(source, 300 * 1024);
  EXPECT_FALSE(nvhttp::apps::poster::preview(source));
  const auto started = std::chrono::steady_clock::now();
  const auto waited = nvhttp::apps::poster::preview_when_ready(source, std::chrono::seconds(5));
  EXPECT_FALSE(waited);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
  test_support::remove_tree_quiet(root);
}

TEST(Poster, WaitReturnsThePreviewOnceThePassHasProducedIt) {
  const auto root = test_root();
  const auto source = write_png(root / "source.png", 1600, 900, true);
  nvhttp::apps::poster::schedule(source);
  const auto preview = nvhttp::apps::poster::preview_when_ready(source, std::chrono::seconds(30));
  ASSERT_TRUE(preview);
  EXPECT_LE(preview->length, nvhttp::apps::poster::preview_limit_bytes);
  EXPECT_TRUE(std::filesystem::exists(preview->path));
  ASSERT_TRUE(nvhttp::apps::poster::wait_idle());
  test_support::remove_tree_quiet(root);
}

TEST(Poster, WaitEndsWhenThePassGivesUpOnTheSource) {
  const auto root = test_root();
  const auto source = root / "broken.png";
  write_undecodable(source, 300 * 1024);
  nvhttp::apps::poster::schedule(source);
  const auto started = std::chrono::steady_clock::now();
  const auto waited = nvhttp::apps::poster::preview_when_ready(source, std::chrono::seconds(20));
  EXPECT_FALSE(waited);
  // An invalid source ends the wait before the timeout.
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));
  ASSERT_TRUE(nvhttp::apps::poster::wait_idle());
  test_support::remove_tree_quiet(root);
}

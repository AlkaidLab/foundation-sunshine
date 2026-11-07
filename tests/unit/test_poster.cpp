#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "src/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "src/stb_image_write.h"

#include "src/nvhttp/poster.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

namespace {

  std::filesystem::path
  test_root() {
    const auto root = std::filesystem::temp_directory_path() / ("poster-unit-" + std::to_string(std::random_device {}()));
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
  std::filesystem::remove_all(root);
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
  std::filesystem::remove_all(root);
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
  std::filesystem::remove_all(root);
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

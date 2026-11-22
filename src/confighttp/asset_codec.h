#pragma once

#include <array>
#include <brotli/decode.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace confighttp::assets {
  // Decodes one complete Brotli stream (RFC 7932) from the shipped copy of a
  // Web UI asset for a client that did not accept Content-Encoding: br. Fails
  // closed: damaged input, trailing bytes after the stream, or a result larger
  // than limit bytes returns no value and the caller answers 500, never a
  // partially decoded body.
  inline std::optional<std::string>
  brotli_decode(std::string_view input, std::size_t limit) {
    auto *state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) return std::nullopt;
    std::string output;
    std::array<std::uint8_t, 65536> chunk {};
    const auto *next_in = reinterpret_cast<const std::uint8_t *>(input.data());
    std::size_t available_in = input.size();
    bool complete = false;
    while (true) {
      auto *next_out = chunk.data();
      std::size_t available_out = chunk.size();
      const auto result = BrotliDecoderDecompressStream(state, &available_in, &next_in, &available_out, &next_out, nullptr);
      output.append(reinterpret_cast<const char *>(chunk.data()), chunk.size() - available_out);
      if (output.size() > limit) break;
      if (result == BROTLI_DECODER_RESULT_SUCCESS) {
        // The whole file was fed at once; anything left is trailing data.
        complete = available_in == 0;
        break;
      }
      if (result != BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) break;
    }
    BrotliDecoderDestroyInstance(state);
    if (!complete) return std::nullopt;
    return output;
  }
}  // namespace confighttp::assets

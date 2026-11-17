#pragma once
// SHA-256 (FIPS 180-4) for config conflict checks. No external crypto dependency.
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sparxie::sha256 {
  inline std::uint32_t
  rotr(std::uint32_t x, std::uint32_t n) { return (x >> n) | (x << (32 - n)); }

  inline std::string
  hex(const std::uint8_t *data, std::size_t size) {
    std::string out;
    out.resize(size * 2);
    static const char *digits = "0123456789abcdef";
    for (std::size_t i = 0; i < size; ++i) {
      out[i * 2] = digits[data[i] >> 4];
      out[i * 2 + 1] = digits[data[i] & 0x0f];
    }
    return out;
  }

  inline std::string
  hash(const std::uint8_t *data, std::size_t size) {
    static const std::uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    std::uint32_t h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    std::uint32_t h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
    std::vector<std::uint8_t> msg(data, data + size);
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<std::uint8_t>((bits >> (i * 8)) & 0xff));
    for (std::size_t off = 0; off < msg.size(); off += 64) {
      std::uint32_t w[64];
      for (int i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t(msg[off + i * 4]) << 24) | (std::uint32_t(msg[off + i * 4 + 1]) << 16) |
               (std::uint32_t(msg[off + i * 4 + 2]) << 8) | std::uint32_t(msg[off + i * 4 + 3]);
      }
      for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      }
      std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
      for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t t1 = h + S1 + ch + k[i] + w[i];
        const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
      }
      h0 += a;
      h1 += b;
      h2 += c;
      h3 += d;
      h4 += e;
      h5 += f;
      h6 += g;
      h7 += h;
    }
    std::uint8_t out[32];
    std::uint32_t hs[8] = { h0, h1, h2, h3, h4, h5, h6, h7 };
    for (int i = 0; i < 8; ++i) {
      out[i * 4] = std::uint8_t(hs[i] >> 24);
      out[i * 4 + 1] = std::uint8_t(hs[i] >> 16);
      out[i * 4 + 2] = std::uint8_t(hs[i] >> 8);
      out[i * 4 + 3] = std::uint8_t(hs[i]);
    }
    return hex(out, 32);
  }

  inline std::string
  hash(const std::string &text) {
    return hash(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
  }
}  // namespace sparxie::sha256

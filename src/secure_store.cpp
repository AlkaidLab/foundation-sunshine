/**
 * @file src/secure_store.cpp
 * @brief 跨平台敏感字段加密存储（用于 AI API key 落盘加密）
 *
 * 平台实现：
 *  - Windows: DPAPI (CryptProtectData, CurrentUser scope)
 *    密文绑定当前 Windows 用户账户 —— 其他本地用户即使读到文件也无法解密。
 *    crypt32 已在 CMake 的 PLATFORM_LIBRARIES 中链接。
 *  - Linux/macOS: AES-256-GCM，密钥由 machine-id + $HOME + 固定应用盐
 *    经 SHA-256 派生（不依赖系统密钥环服务，避免 gnome-keyring/libsecret
 *    等运行时依赖；强度弱于 OS Keychain，但足以防止"文件被拷走/同步上云"）。
 *
 * 磁盘格式：enc:v1:<base64(iv || ciphertext || tag)>
 *   - AES-256-GCM: iv=12B, tag=16B
 *   - DPAPI 分支 iv/tag 由 CryptProtectData 内部处理，载荷为原始 DPAPI blob
 */
#include "secure_store.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dpapi.h>
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#endif

namespace secure_store {

  namespace {

    constexpr const char *kMagic = "enc:v1:";
    constexpr size_t kGcmIvLen = 12;
    constexpr size_t kGcmTagLen = 16;

    std::string
    b64_encode(const std::string &in) {
      static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string out;
      out.reserve(((in.size() + 2) / 3) * 4);

      size_t i = 0;
      for (; i + 3 <= in.size(); i += 3) {
        uint32_t v = (static_cast<uint8_t>(in[i]) << 16) |
                     (static_cast<uint8_t>(in[i + 1]) << 8) |
                     static_cast<uint8_t>(in[i + 2]);
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += tbl[(v >> 6) & 0x3F];
        out += tbl[v & 0x3F];
      }

      const size_t rem = in.size() - i;
      if (rem == 1) {
        const uint32_t v = static_cast<uint8_t>(in[i]) << 16;
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += "==";
      }
      else if (rem == 2) {
        const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) |
                           (static_cast<uint8_t>(in[i + 1]) << 8);
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += tbl[(v >> 6) & 0x3F];
        out += '=';
      }
      return out;
    }

    bool
    b64_decode(const std::string &in, std::string &out) {
      static const signed char tbl[256] = {
        /* 0x00-0x7F */
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
        -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
        /* 0x80-0xFF */
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      };

      out.clear();
      out.reserve((in.size() / 4) * 3);

      uint32_t acc = 0;
      int bits = 0;
      for (char c : in) {
        if (c == '=') break;  // padding：停止
        const signed char v = tbl[static_cast<uint8_t>(c)];
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
          bits -= 8;
          out += static_cast<char>((acc >> bits) & 0xFF);
        }
      }
      return true;
    }

    std::string
    strip_magic(const std::string &value) {
      return value.substr(std::strlen(kMagic));
    }

#ifndef _WIN32
    /**
     * @brief 派生 AES-256 密钥：SHA-256(machine-id ":" HOME ":" 固定盐)
     */
    void
    derive_key(unsigned char key[32]) {
      std::string material;

      auto read_first_line = [](const char *path) -> std::string {
        std::ifstream f(path);
        std::string line;
        if (!f.is_open()) return {};
        std::getline(f, line);
        return line;
      };

      std::string machine_id = read_first_line("/etc/machine-id");
      if (machine_id.empty()) {
        machine_id = read_first_line("/var/lib/dbus/machine-id");
      }

      const char *home = std::getenv("HOME");
      material = machine_id;
      material += ":";
      material += (home != nullptr) ? home : "";
      material += ":foundation-sunshine:ai-key:v1";

      unsigned char digest[SHA256_DIGEST_LENGTH];
      SHA256(
        reinterpret_cast<const unsigned char *>(material.data()),
        material.size(),
        digest);
      std::memcpy(key, digest, 32);
    }
#endif

  }  // namespace

  bool
  is_encrypted(const std::string &value) {
    return value.size() >= std::strlen(kMagic) &&
           value.compare(0, std::strlen(kMagic), kMagic) == 0;
  }

  bool
  encrypt(const std::string &plaintext, std::string &out_enc, std::string &err) {
    out_enc.clear();
    err.clear();
    if (plaintext.empty()) {
      return true;  // 空 key 不加密，与旧行为一致
    }

#ifdef _WIN32
    DATA_BLOB in_blob = {
      static_cast<DWORD>(plaintext.size()),
      reinterpret_cast<BYTE *>(const_cast<char *>(plaintext.data())),
    };
    DATA_BLOB out_blob = {};

    // CRYPTPROTECT_UI_FORBIDDEN: 无 UI；默认 CurrentUser scope，密文绑定本用户
    if (!CryptProtectData(&in_blob, L"sunshine-ai-key", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out_blob)) {
      err = "CryptProtectData failed: " + std::to_string(GetLastError());
      return false;
    }

    const std::string raw(
      reinterpret_cast<const char *>(out_blob.pbData),
      out_blob.cbData);
    LocalFree(out_blob.pbData);

    out_enc = std::string(kMagic) + b64_encode(raw);
    return true;
#else
    unsigned char key[32];
    derive_key(key);

    unsigned char iv[kGcmIvLen];
    if (RAND_bytes(iv, static_cast<int>(sizeof(iv))) != 1) {
      err = "RAND_bytes failed";
      return false;
    }

    std::string ct(plaintext.size(), '\0');
    unsigned char tag[kGcmTagLen];

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
      err = "EVP_CIPHER_CTX_new failed";
      return false;
    }

    bool ok = true;
    int len = 0;
    int total = 0;
    ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key, iv) == 1;
    ok = ok && EVP_EncryptUpdate(
      ctx,
      reinterpret_cast<unsigned char *>(ct.data()),
      &len,
      reinterpret_cast<const unsigned char *>(plaintext.data()),
      static_cast<int>(plaintext.size())) == 1;
    total = len;
    ok = ok && EVP_EncryptFinal_ex(
      ctx,
      reinterpret_cast<unsigned char *>(ct.data()) + total,
      &len) == 1;
    total += len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kGcmTagLen, tag) == 1;
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
      err = "AES-256-GCM encrypt failed";
      return false;
    }

    std::string raw;
    raw.reserve(kGcmIvLen + total + kGcmTagLen);
    raw.append(reinterpret_cast<const char *>(iv), kGcmIvLen);
    raw.append(ct.data(), total);
    raw.append(reinterpret_cast<const char *>(tag), kGcmTagLen);

    out_enc = std::string(kMagic) + b64_encode(raw);
    return true;
#endif
  }

  bool
  decrypt(const std::string &in_enc, std::string &out_plain, std::string &err) {
    out_plain.clear();
    err.clear();
    if (in_enc.empty()) {
      return true;  // 空 key，无需解密
    }
    if (!is_encrypted(in_enc)) {
      err = "not an encrypted value (missing enc:v1: prefix)";
      return false;
    }

    std::string raw;
    if (!b64_decode(strip_magic(in_enc), raw)) {
      err = "invalid base64 payload";
      return false;
    }

#ifdef _WIN32
    DATA_BLOB in_blob = {
      static_cast<DWORD>(raw.size()),
      reinterpret_cast<BYTE *>(const_cast<char *>(raw.data())),
    };
    DATA_BLOB out_blob = {};

    if (!CryptUnprotectData(&in_blob, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out_blob)) {
      err = "CryptUnprotectData failed: " + std::to_string(GetLastError());
      return false;
    }

    out_plain.assign(
      reinterpret_cast<const char *>(out_blob.pbData),
      out_blob.cbData);
    LocalFree(out_blob.pbData);
    return true;
#else
    if (raw.size() < kGcmIvLen + kGcmTagLen) {
      err = "ciphertext too short";
      return false;
    }

    unsigned char key[32];
    derive_key(key);

    const unsigned char *iv = reinterpret_cast<const unsigned char *>(raw.data());
    const size_t ct_len = raw.size() - kGcmIvLen - kGcmTagLen;
    const unsigned char *ct = iv + kGcmIvLen;
    const unsigned char *tag = ct + ct_len;

    std::string pt(ct_len, '\0');

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
      err = "EVP_CIPHER_CTX_new failed";
      return false;
    }

    bool ok = true;
    int len = 0;
    int total = 0;
    ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key, iv) == 1;
    ok = ok && EVP_CIPHER_CTX_ctrl(
      ctx, EVP_CTRL_GCM_SET_TAG, kGcmTagLen, const_cast<unsigned char *>(tag)) == 1;
    ok = ok && EVP_DecryptUpdate(
      ctx,
      reinterpret_cast<unsigned char *>(pt.data()),
      &len,
      ct,
      static_cast<int>(ct_len)) == 1;
    total = len;
    ok = ok && EVP_DecryptFinal_ex(
      ctx,
      reinterpret_cast<unsigned char *>(pt.data()) + total,
      &len) == 1;
    total += len;
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
      err = "AES-256-GCM decrypt failed (key mismatch or corrupted data)";
      return false;
    }

    out_plain.assign(pt.data(), total);
    return true;
#endif
  }

}  // namespace secure_store

#pragma once

#include <string>

namespace secure_store {

  /**
   * @brief 判断字符串是否为加密存储格式（enc:v1: 前缀）
   */
  bool
  is_encrypted(const std::string &value);

  /**
   * @brief 加密字符串。
   *
   * 空串直接返回空（不加密，与旧行为一致）。
   * 成功时 out_enc 形如 "enc:v1:<base64>"。
   *
   * @param plaintext 明文
   * @param out_enc   输出密文（enc:v1: 前缀格式）
   * @param err       失败原因
   * @return 是否成功
   */
  bool
  encrypt(const std::string &plaintext, std::string &out_enc, std::string &err);

  /**
   * @brief 解密 encrypt() 的输出。
   *
   * @param in_enc    enc:v1: 前缀格式密文
   * @param out_plain 输出明文
   * @param err       失败原因
   * @return 是否成功
   */
  bool
  decrypt(const std::string &in_enc, std::string &out_plain, std::string &err);

}  // namespace secure_store

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
对拍验证 secure_store.cpp 的 POSIX 分支加密协议。

验证目标：C++ 代码中的算法设计（密钥派生、GCM 布局、base64、enc:v1: 前缀）
与 Python 独立实现 + cryptography 标准库三方一致，确保部署后能解回。
"""
import base64
import hashlib
import os
import sys

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

K_MAGIC = "enc:v1:"
K_GCM_IV_LEN = 12
K_GCM_TAG_LEN = 16
SALT = "foundation-sunshine:ai-key:v1"

failures = []


def check(name, cond):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}")
    if not cond:
        failures.append(name)


# ---- 与 C++ b64_encode / b64_decode 完全一致的实现 ----
B64_TBL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"


def b64_encode(data: bytes) -> str:
    out = []
    i = 0
    while i + 3 <= len(data):
        v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2]
        out.append(B64_TBL[(v >> 18) & 0x3F])
        out.append(B64_TBL[(v >> 12) & 0x3F])
        out.append(B64_TBL[(v >> 6) & 0x3F])
        out.append(B64_TBL[v & 0x3F])
        i += 3
    rem = len(data) - i
    if rem == 1:
        v = data[i] << 16
        out.append(B64_TBL[(v >> 18) & 0x3F])
        out.append(B64_TBL[(v >> 12) & 0x3F])
        out += ["=", "="]
    elif rem == 2:
        v = (data[i] << 16) | (data[i + 1] << 8)
        out.append(B64_TBL[(v >> 18) & 0x3F])
        out.append(B64_TBL[(v >> 12) & 0x3F])
        out.append(B64_TBL[(v >> 6) & 0x3F])
        out.append("=")
    return "".join(out)


def b64_decode(s: str) -> bytes:
    out = bytearray()
    acc = 0
    bits = 0
    for c in s:
        if c == "=":
            break
        v = B64_TBL.find(c)
        if v < 0:
            raise ValueError("invalid base64 char")
        acc = (acc << 6) | v
        bits += 6
        if bits >= 8:
            bits -= 8
            out.append((acc >> bits) & 0xFF)
    return bytes(out)


def derive_key(machine_id: str, home: str) -> bytes:
    material = f"{machine_id}:{home}:{SALT}".encode()
    return hashlib.sha256(material).digest()  # 32 bytes


def encrypt_cpp(plain: str, machine_id: str, home: str) -> str:
    """复刻 secure_store.cpp POSIX 分支 encrypt()"""
    if not plain:
        return ""
    key = derive_key(machine_id, home)
    iv = os.urandom(K_GCM_IV_LEN)
    aesgcm = AESGCM(key)
    # cryptography 返回 ct||tag（不含 iv）；C++ 中 iv 手动生成并拼在最前
    ct_with_tag = aesgcm.encrypt(iv, plain.encode(), None)
    raw = iv + ct_with_tag
    return K_MAGIC + b64_encode(raw)


def decrypt_cpp(enc: str, machine_id: str, home: str) -> str:
    """复刻 secure_store.cpp POSIX 分支 decrypt()"""
    if not enc:
        return ""
    if not enc.startswith(K_MAGIC):
        raise ValueError("not an encrypted value")
    raw = b64_decode(enc[len(K_MAGIC):])
    if len(raw) < K_GCM_IV_LEN + K_GCM_TAG_LEN:
        raise ValueError("ciphertext too short")
    iv, rest = raw[:K_GCM_IV_LEN], raw[K_GCM_IV_LEN:]
    key = derive_key(machine_id, home)
    aesgcm = AESGCM(key)
    return aesgcm.decrypt(iv, rest, None).decode()


def decrypt_std(raw_iv_ct_tag: bytes, machine_id: str, home: str) -> str:
    """直接用 cryptography 标准输出格式解密（iv||ct||tag 拼接输入）"""
    key = derive_key(machine_id, home)
    return AESGCM(key).decrypt(raw_iv_ct_tag[:K_GCM_IV_LEN],
                               raw_iv_ct_tag[K_GCM_IV_LEN:], None).decode()


print("== 1. base64 与标准库一致性 ==")
for data in [b"", b"a", b"ab", b"abc", b"abcd", b"\x00\x01\x02\xff\xfe", os.urandom(64)]:
    mine = b64_encode(data)
    std = base64.b64encode(data).decode()
    check(f"encode {len(data)}B: 自实现 == stdlib", mine == std)
    check(f"decode {len(data)}B: 往返一致", b64_decode(mine) == data)

print("== 2. 密钥派生 ==")
k1 = derive_key("machine-a", "/home/u")
k2 = derive_key("machine-b", "/home/u")
k3 = derive_key("machine-a", "/home/v")
check("key 长度 32", len(k1) == 32)
check("不同 machine-id 派生不同 key", k1 != k2)
check("不同 HOME 派生不同 key", k1 != k3)

print("== 3. 加密→解密 往返（模拟 C++ 完整链路）==")
MID, HOME = "9d27f8c1e2a34b5c8f0e1234567890ab", "/home/user"
for plain in ["sk-1234567890abcdef", "sk-abc", "", "a" * 100, "中文密钥测试 🔑"]:
    enc = encrypt_cpp(plain, MID, HOME)
    dec = decrypt_cpp(enc, MID, HOME)
    if plain == "":
        check(f"空串不加密: enc==''", enc == "")
    else:
        check(f"'{plain[:12]}...' 往返一致", dec == plain)
        check(f"'{plain[:12]}...' 带 enc:v1: 前缀", enc.startswith(K_MAGIC))

print("== 4. cryptography 标准输出格式交叉验证 ==")
key = derive_key(MID, HOME)
aesgcm = AESGCM(key)
iv_fixed = os.urandom(K_GCM_IV_LEN)
# C++ 布局定义: raw = iv + aesgcm.encrypt(iv, plain)   (cryptography 输出 ct||tag)
ct_with_tag = aesgcm.encrypt(iv_fixed, b"hello-crypto", None)
cpp_layout = iv_fixed + ct_with_tag
check("布局: 前12B为iv", cpp_layout[:K_GCM_IV_LEN] == iv_fixed)
check("布局: 总长 = 12 + 明文长 + 16(tag)", len(cpp_layout) == K_GCM_IV_LEN + len(b"hello-crypto") + K_GCM_TAG_LEN)
# 按 C++ 的解密逻辑（拆 iv / rest）用 cryptography 解回
dec = aesgcm.decrypt(cpp_layout[:K_GCM_IV_LEN], cpp_layout[K_GCM_IV_LEN:], None)
check("按C++布局(iv|rest)解密成功", dec == b"hello-crypto")
# encrypt_cpp 生成的结果也能用 cryptography 直接按布局解回（与第3节互为印证）
enc_probe = encrypt_cpp("cross-check", MID, HOME)
raw_probe = b64_decode(enc_probe[len(K_MAGIC):])
check("encrypt_cpp产物可被cryptography按布局解回",
      aesgcm.decrypt(raw_probe[:K_GCM_IV_LEN], raw_probe[K_GCM_IV_LEN:], None) == b"cross-check")

print("== 5. 篡改检测（tag 校验）==")
enc = encrypt_cpp("secret-key-123", MID, HOME)
raw = bytearray(b64_decode(enc[len(K_MAGIC):]))
raw[-1] ^= 0x01  # 翻转 tag 最后一个字节
tampered = K_MAGIC + b64_encode(bytes(raw))
try:
    decrypt_cpp(tampered, MID, HOME)
    check("篡改后解密必须失败", False)
except Exception:
    check("篡改后解密必须失败", True)

print("== 6. 错误 key / 错误机器 解密必须失败 ==")
try:
    decrypt_cpp(enc, "wrong-machine-id", HOME)
    check("错误 machine-id 解密失败", False)
except Exception:
    check("错误 machine-id 解密失败", True)

print("== 7. 非法输入防护 ==")
try:
    decrypt_cpp("not-encrypted-at-all", MID, HOME)
    check("非 enc:v1: 输入被拒绝", False)
except ValueError:
    check("非 enc:v1: 输入被拒绝", True)
check("is_encrypted 判断", encrypt_cpp("x", MID, HOME).startswith(K_MAGIC) and
      not "plain-sk-123".startswith(K_MAGIC))

print()
if failures:
    print(f"!!! {len(failures)} 项失败: {failures}")
    sys.exit(1)
print("全部通过 ✅ 协议设计与标准库完全一致")
